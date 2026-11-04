//! Мониторинг цен и остатков поставщика: расписание → задача PRICE_SYNC → загрузка страницы → разбор → пересчёт.
//!
//! Принцип: любая ошибка мониторинга не должна молча «обнулять» товар. Остаток падает в 0 только
//! когда сайт явно сообщил об отсутствии (или страница удалена — 404/410).

pub mod fetch;
pub mod parse;

use serde_json::json;
use sqlx::SqlitePool;

use crate::catalog;
use crate::failover;
use crate::db::{self, TaskType};
use crate::error::AppResult;
use fetch::{FetchError, PageFetcher};
use parse::Offer;

const LOCK_TIMEOUT_SECS: i64 = 300;
const BLOCKED_RETRY_SECS: i64 = 900;
const RETRY_DELAY_SECS: i64 = 600;
const PRICE_SYNC_PRIORITY: i64 = 6;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SyncOutcome {
    NoTask,
    /// Страница разобрана; сколько вариантов изменилось.
    Updated { product_id: i64, changed: usize },
    /// Товар удалён у поставщика: остатки обнулены, мониторинг остановлен.
    Gone { product_id: i64 },
    /// Сайт заблокировал запрос (403/429) или сеть недоступна — повторим позже.
    Deferred { product_id: i64, reason: String },
    /// Разметки цены нет или её нельзя сопоставить с вариантами — нужен ручной повторный импорт.
    Unreadable { product_id: i64, reason: String },
    /// Адрес поставщика запрещён политикой безопасности.
    Forbidden { product_id: i64 },
}

/// Ставит PRICE_SYNC для товаров, которым пора на проверку. Идемпотентно: открытая задача не дублируется.
pub async fn schedule_due(pool: &SqlitePool, now: i64) -> AppResult<usize> {
    let due: Vec<(i64, i64)> =
        sqlx::query_as("SELECT id, check_interval_sec FROM products WHERE monitor_status = 'ACTIVE' AND next_check_at <= ? ORDER BY next_check_at LIMIT 500")
            .bind(now)
            .fetch_all(pool)
            .await?;
    let mut queued = 0;
    for (product_id, interval) in due {
        let payload = json!({ "product_id": product_id }).to_string();
        let open: Option<i64> = sqlx::query_scalar("SELECT id FROM task_queue WHERE task_type = 'PRICE_SYNC' AND status IN ('PENDING','IN_PROGRESS') AND payload_json = ?")
            .bind(&payload)
            .fetch_optional(pool)
            .await?;
        if open.is_none() {
            db::enqueue_task(pool, TaskType::PriceSync, PRICE_SYNC_PRIORITY, &payload, now, 3).await?;
            queued += 1;
        }
        sqlx::query("UPDATE products SET next_check_at = ? WHERE id = ?").bind(now + interval.max(60)).bind(product_id).execute(pool).await?;
    }
    Ok(queued)
}

fn match_offers(offers: &[Offer], variants: &[(i64, String)]) -> Option<Vec<(i64, Offer)>> {
    let mut matched = Vec::new();
    for (variant_id, sku) in variants {
        let found = offers.iter().find(|o| o.sku.as_deref() == Some(sku.as_str())).or(if variants.len() == 1 && offers.len() == 1 { offers.first() } else { None });
        matched.push((*variant_id, found?.clone()));
    }
    Some(matched)
}

async fn cookies_for(pool: &SqlitePool, host: &str, vault: Option<&crate::security::CredentialVault>) -> Option<String> {
    let vault = vault?;
    let stored = crate::settings::get(pool, &format!("supplier_cookies:{host}")).await.ok()??;
    let bytes = crate::security::hex_decode(&stored)?;
    String::from_utf8(vault.decrypt(&bytes).ok()?).ok()
}

/// Сохраняет cookies сессии поставщика (шифрованно), присланные расширением.
pub async fn save_supplier_cookies(pool: &SqlitePool, vault: &crate::security::CredentialVault, host: &str, cookie_header: &str, now: i64) -> AppResult<()> {
    use crate::error::AppError;
    let host_ok = !host.is_empty() && host.len() <= 253 && host.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-'));
    if !host_ok || cookie_header.len() > 16_384 || cookie_header.contains(['\r', '\n']) {
        return Err(AppError::Validation("некорректные данные сессии поставщика".into()));
    }
    let encrypted = vault.encrypt(cookie_header.as_bytes())?;
    crate::settings::set(pool, &format!("supplier_cookies:{}", host.to_ascii_lowercase()), &crate::security::hex_encode(&encrypted), now).await
}

/// Берёт одну задачу PRICE_SYNC и выполняет её.
pub async fn process_next_price_sync(pool: &SqlitePool, fetcher: &dyn PageFetcher, vault: Option<&crate::security::CredentialVault>, now: i64) -> AppResult<SyncOutcome> {
    let Some(task) = db::dequeue_next_task_of(pool, now, LOCK_TIMEOUT_SECS, &[TaskType::PriceSync]).await? else { return Ok(SyncOutcome::NoTask) };
    let product_id = serde_json::from_str::<serde_json::Value>(&task.payload_json).ok().and_then(|v| v["product_id"].as_i64());
    let Some(product_id) = product_id else {
        db::fail_task(pool, task.id, "повреждённая задача", now, RETRY_DELAY_SECS, false).await?;
        return Ok(SyncOutcome::Unreadable { product_id: 0, reason: "повреждённая задача".into() });
    };
    let row: Option<(i64, String)> = sqlx::query_as("SELECT store_id, supplier_url FROM products WHERE id = ? AND monitor_status = 'ACTIVE'").bind(product_id).fetch_optional(pool).await?;
    let Some((store_id, supplier_url)) = row else {
        db::complete_task(pool, task.id).await?; // товар удалён или на паузе — делать нечего
        return Ok(SyncOutcome::NoTask);
    };

    let host = url::Url::parse(&supplier_url).ok().and_then(|u| u.host_str().map(str::to_ascii_lowercase)).unwrap_or_default();
    let cookies = cookies_for(pool, &host, vault).await;
    let proxy = fetch_proxy(pool, now).await?;
    let result = fetcher.fetch(&supplier_url, cookies.as_deref(), proxy.as_ref().map(|(_, u)| u.as_str())).await;

    let outcome = match result {
        Err(FetchError::Forbidden(reason)) => {
            db::fail_task(pool, task.id, &reason, now, RETRY_DELAY_SECS, false).await?;
            sqlx::query("UPDATE products SET monitor_status = 'ERROR' WHERE id = ?").bind(product_id).execute(pool).await?;
            SyncOutcome::Forbidden { product_id }
        }
        Err(FetchError::Transport(reason)) => {
            report(pool, &proxy, false, now).await?;
            db::fail_task(pool, task.id, &reason, now, RETRY_DELAY_SECS, true).await?;
            SyncOutcome::Deferred { product_id, reason }
        }
        Ok(page) if matches!(page.status, 403 | 429) => {
            report(pool, &proxy, false, now).await?;
            db::defer_task(pool, task.id, now + BLOCKED_RETRY_SECS).await?;
            SyncOutcome::Deferred { product_id, reason: format!("сайт поставщика временно блокирует запросы (HTTP {})", page.status) }
        }
        Ok(page) if matches!(page.status, 404 | 410) => {
            report(pool, &proxy, true, now).await?;
            mark_gone(pool, product_id, store_id, now).await?;
            db::complete_task(pool, task.id).await?;
            SyncOutcome::Gone { product_id }
        }
        Ok(page) if page.status >= 500 => {
            db::fail_task(pool, task.id, &format!("HTTP {}", page.status), now, RETRY_DELAY_SECS, true).await?;
            SyncOutcome::Deferred { product_id, reason: format!("сайт поставщика недоступен (HTTP {})", page.status) }
        }
        Ok(page) => {
            report(pool, &proxy, true, now).await?;
            apply_page(pool, task.id, product_id, store_id, &page.body, now).await?
        }
    };
    Ok(outcome)
}

async fn fetch_proxy(pool: &SqlitePool, now: i64) -> AppResult<Option<(i64, String)>> {
    fetch::pick_proxy(pool, now).await
}

async fn report(pool: &SqlitePool, proxy: &Option<(i64, String)>, ok: bool, now: i64) -> AppResult<()> {
    match proxy {
        Some((id, _)) => fetch::report_proxy(pool, *id, ok, now).await,
        None => Ok(()),
    }
}

async fn apply_page(pool: &SqlitePool, task_id: i64, product_id: i64, store_id: i64, body: &str, now: i64) -> AppResult<SyncOutcome> {
    let offers = parse::parse_offers(body);
    let variants: Vec<(i64, String)> = sqlx::query_as("SELECT id, supplier_sku FROM product_variants WHERE product_id = ?").bind(product_id).fetch_all(pool).await?;
    let Some(matched) = (!offers.is_empty()).then(|| match_offers(&offers, &variants)).flatten() else {
        let reason = "на странице поставщика нет читаемой цены — обновите товар через расширение".to_string();
        db::fail_task(pool, task_id, &reason, now, RETRY_DELAY_SECS, false).await?;
        return Ok(SyncOutcome::Unreadable { product_id, reason });
    };
    let mut tx = pool.begin().await?;
    let rules = catalog::load_rule_set(&mut tx, store_id).await?;
    let mut changed = 0;
    for (variant_id, offer) in matched {
        let shipping: i64 = sqlx::query_scalar("SELECT shipping_price_minor FROM product_variants WHERE id = ?").bind(variant_id).fetch_one(&mut *tx).await?;
        let applied = failover::apply_observed(&mut tx, rules.as_ref(), variant_id, offer.price_minor, shipping, offer.stock, now).await?;
        if !matches!(applied, catalog::ApplyOutcome::Unchanged) {
            changed += 1;
        }
    }
    sqlx::query("UPDATE products SET last_checked_at = ? WHERE id = ?").bind(now).bind(product_id).execute(&mut *tx).await?;
    tx.commit().await?;
    db::complete_task(pool, task_id).await?;
    Ok(SyncOutcome::Updated { product_id, changed })
}

async fn mark_gone(pool: &SqlitePool, product_id: i64, store_id: i64, now: i64) -> AppResult<()> {
    let mut tx = pool.begin().await?;
    let rules = catalog::load_rule_set(&mut tx, store_id).await?;
    let variants: Vec<(i64, i64, i64)> = sqlx::query_as("SELECT id, supplier_price_minor, shipping_price_minor FROM product_variants WHERE product_id = ?").bind(product_id).fetch_all(&mut *tx).await?;
    for (id, price, shipping) in variants {
        failover::apply_observed(&mut tx, rules.as_ref(), id, price, shipping, 0, now).await?;
    }
    // Если витрину удерживает резервный поставщик — товар остаётся под наблюдением.
    let alive: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM product_variants WHERE product_id = ? AND is_in_stock = 1").bind(product_id).fetch_one(&mut *tx).await?;
    let status = if alive > 0 { "ACTIVE" } else { "ARCHIVED" };
    sqlx::query("UPDATE products SET monitor_status = ?, last_checked_at = ? WHERE id = ?").bind(status).bind(now).bind(product_id).execute(&mut *tx).await?;
    tx.commit().await?;
    Ok(())
}

const BACKUP_INTERVAL_SECS: i64 = 1_800;

/// Проверка страниц резервных поставщиков (приоритет 2–3). Возвращает число обработанных.
/// Сбой сети или блокировка ничего не меняют (кроме отметки времени, чтобы не долбить сайт); 404/410 — поставщик недоступен.
pub async fn check_backups(pool: &SqlitePool, fetcher: &dyn PageFetcher, vault: Option<&crate::security::CredentialVault>, now: i64, limit: i64) -> AppResult<usize> {
    let due: Vec<(i64, String, String)> = sqlx::query_as(
        "SELECT id, supplier_url, supplier_sku FROM variant_suppliers WHERE priority > 1 AND last_checked_at + ? <= ? ORDER BY last_checked_at LIMIT ?",
    )
    .bind(BACKUP_INTERVAL_SECS)
    .bind(now)
    .bind(limit)
    .fetch_all(pool)
    .await?;
    for (id, url, sku) in &due {
        let host = url::Url::parse(url).ok().and_then(|u| u.host_str().map(str::to_ascii_lowercase)).unwrap_or_default();
        let cookies = cookies_for(pool, &host, vault).await;
        match fetcher.fetch(url, cookies.as_deref(), None).await {
            Ok(page) if matches!(page.status, 404 | 410) => failover::record_backup_observation(pool, *id, None, Some(0), false, now).await?,
            Ok(page) if page.status == 200 => {
                let offers = parse::parse_offers(&page.body);
                let offer = offers.iter().find(|o| o.sku.as_deref() == Some(sku.as_str())).or(if offers.len() == 1 { offers.first() } else { None });
                match offer {
                    Some(o) => failover::record_backup_observation(pool, *id, Some(o.price_minor), Some(o.stock), true, now).await?,
                    None => touch_backup(pool, *id, now).await?,
                }
            }
            _ => touch_backup(pool, *id, now).await?,
        }
    }
    Ok(due.len())
}

async fn touch_backup(pool: &SqlitePool, id: i64, now: i64) -> AppResult<()> {
    sqlx::query("UPDATE variant_suppliers SET last_checked_at = ? WHERE id = ?").bind(now).bind(id).execute(pool).await?;
    Ok(())
}

#[cfg(test)]
mod tests;
