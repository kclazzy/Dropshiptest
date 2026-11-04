//! Резервные поставщики (ТЗ 4.1): витрина не обнуляется, пока есть кто-то, у кого товар есть и сделка выгодна.
//!
//! Порядок выбора по приоритету (1 — основной):
//! 1. остаток не ниже порога **и** прибыль при уже опубликованной цене не ниже минимума;
//! 2. остаток не ниже порога (цена на витрине пересчитается под нового поставщика);
//! 3. никого нет — витрина получает остаток 0 (цена основного сохраняется).
//!
//! Возврат на основного происходит сам, как только он снова проходит проверку (без «залипания» на резервном).

use sqlx::{SqliteConnection, SqlitePool};

use crate::catalog::{self, apply_variant_state, ApplyOutcome};
use crate::error::{AppError, AppResult};
use crate::pricing::{profit_at, RuleSet};

pub const MAX_PRIORITY: i64 = 3;

#[derive(Debug, Clone, PartialEq, Eq, sqlx::FromRow)]
pub struct Candidate {
    pub id: i64,
    pub priority: i64,
    #[sqlx(rename = "supplier_price_minor")]
    pub price_minor: i64,
    #[sqlx(rename = "shipping_price_minor")]
    pub shipping_minor: i64,
    #[sqlx(rename = "supplier_stock")]
    pub stock: i64,
    #[sqlx(rename = "is_available")]
    pub available: bool,
}

/// Чистая функция выбора. `current_retail == 0` — цена ещё не опубликована (проверка прибыли пропускается).
pub fn select(candidates: &[Candidate], rules: Option<&RuleSet>, current_retail: i64) -> Option<i64> {
    let mut ordered: Vec<&Candidate> = candidates.iter().filter(|c| c.available).collect();
    ordered.sort_by_key(|c| (c.priority, c.id));
    let threshold = |c: &Candidate| rules.and_then(|r| r.find(c.price_minor)).map_or(1, |r| r.min_stock_threshold().max(1));
    let has_stock = |c: &&Candidate| c.stock >= threshold(c);
    let profitable = |c: &&Candidate| {
        if current_retail <= 0 {
            return true;
        }
        // Правила есть, а для цены этого поставщика диапазона нет — прибыль не посчитать, значит «не подходит».
        let Some(set) = rules else { return true };
        let Some(rule) = set.find(c.price_minor) else { return false };
        let Some(cost) = c.price_minor.checked_add(c.shipping_minor) else { return false };
        profit_at(current_retail, cost, rule).is_ok_and(|p| p >= rule.min_profit_minor())
    };
    ordered
        .iter()
        .find(|c| has_stock(c) && profitable(c))
        .or_else(|| ordered.iter().find(|c| has_stock(c)))
        .map(|c| c.id)
}

async fn load(conn: &mut SqliteConnection, variant_id: i64) -> AppResult<Vec<Candidate>> {
    Ok(sqlx::query_as("SELECT id, priority, supplier_price_minor, shipping_price_minor, supplier_stock, is_available FROM variant_suppliers WHERE variant_id = ? ORDER BY priority, id")
        .bind(variant_id)
        .fetch_all(&mut *conn)
        .await?)
}

/// Создаёт строку основного поставщика (приоритет 1) из данных варианта и товара, если её ещё нет.
pub async fn ensure_primary(conn: &mut SqliteConnection, variant_id: i64, now: i64) -> AppResult<()> {
    sqlx::query(
        "INSERT OR IGNORE INTO variant_suppliers (variant_id, priority, supplier_url, supplier_sku, supplier_price_minor, shipping_price_minor, supplier_stock, last_checked_at) \
         SELECT v.id, 1, p.supplier_url, v.supplier_sku, v.supplier_price_minor, v.shipping_price_minor, v.supplier_stock, ? \
         FROM product_variants v JOIN products p ON p.id = v.product_id WHERE v.id = ?",
    )
    .bind(now)
    .bind(variant_id)
    .execute(&mut *conn)
    .await?;
    Ok(())
}

pub async fn has_suppliers(conn: &mut SqliteConnection, variant_id: i64) -> AppResult<bool> {
    let n: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM variant_suppliers WHERE variant_id = ?").bind(variant_id).fetch_one(&mut *conn).await?;
    Ok(n > 0)
}

/// Выбирает активного поставщика и применяет его цену/остаток к варианту. `None` — у варианта нет таблицы поставщиков.
pub async fn refresh_variant(conn: &mut SqliteConnection, variant_id: i64, now: i64) -> AppResult<Option<ApplyOutcome>> {
    let candidates = load(conn, variant_id).await?;
    if candidates.is_empty() {
        return Ok(None);
    }
    let (store_id, current_retail): (i64, i64) = sqlx::query_as("SELECT p.store_id, v.retail_price_minor FROM product_variants v JOIN products p ON p.id = v.product_id WHERE v.id = ?")
        .bind(variant_id)
        .fetch_one(&mut *conn)
        .await?;
    let rules = catalog::load_rule_set(conn, store_id).await?;
    let chosen = select(&candidates, rules.as_ref(), current_retail);
    let (id, price, shipping, stock) = match chosen.and_then(|id| candidates.iter().find(|c| c.id == id)) {
        Some(c) => (Some(c.id), c.price_minor, c.shipping_minor, c.stock),
        None => {
            let primary = candidates.iter().min_by_key(|c| (c.priority, c.id));
            let (price, shipping) = primary.map_or((0, 0), |c| (c.price_minor, c.shipping_minor));
            (None, price, shipping, 0)
        }
    };
    sqlx::query("UPDATE product_variants SET active_supplier_id = ? WHERE id = ?").bind(id).bind(variant_id).execute(&mut *conn).await?;
    Ok(Some(apply_variant_state(conn, rules.as_ref(), variant_id, price, shipping, stock, now).await?))
}

/// Единая точка входа для наблюдаемого состояния основной страницы: с резервами — через выбор, без — напрямую.
pub async fn apply_observed(conn: &mut SqliteConnection, rules: Option<&RuleSet>, variant_id: i64, price: i64, shipping: i64, stock: i64, now: i64) -> AppResult<ApplyOutcome> {
    if has_suppliers(conn, variant_id).await? {
        sqlx::query("UPDATE variant_suppliers SET supplier_price_minor = ?, shipping_price_minor = ?, supplier_stock = ?, is_available = 1, last_checked_at = ? WHERE variant_id = ? AND priority = 1")
            .bind(price)
            .bind(shipping)
            .bind(stock)
            .bind(now)
            .bind(variant_id)
            .execute(&mut *conn)
            .await?;
        if let Some(outcome) = refresh_variant(conn, variant_id, now).await? {
            return Ok(outcome);
        }
    }
    apply_variant_state(conn, rules, variant_id, price, shipping, stock, now).await
}

pub struct NewSupplier<'a> {
    pub priority: i64,
    pub url: &'a str,
    pub sku: &'a str,
    pub price_minor: i64,
    pub shipping_minor: i64,
    pub stock: i64,
}

/// Добавляет/заменяет резервного поставщика (приоритет 2–3).
pub async fn upsert_backup(pool: &SqlitePool, variant_id: i64, s: &NewSupplier<'_>, now: i64) -> AppResult<()> {
    let bad = |m: &str| Err(AppError::Validation(m.to_string()));
    if !(2..=MAX_PRIORITY).contains(&s.priority) {
        return bad("резервный поставщик: приоритет 2 или 3");
    }
    let url = url::Url::parse(s.url.trim()).ok().filter(|u| matches!(u.scheme(), "http" | "https") && u.host_str().is_some());
    let Some(url) = url else { return bad("некорректная ссылка на поставщика") };
    if s.sku.trim().is_empty() || s.sku.len() > 200 || s.price_minor <= 0 || s.shipping_minor < 0 || s.stock < 0 {
        return bad("артикул, цена (больше 0), доставка и остаток должны быть заполнены корректно");
    }
    let mut tx = pool.begin().await?;
    let exists: Option<i64> = sqlx::query_scalar("SELECT id FROM product_variants WHERE id = ?").bind(variant_id).fetch_optional(&mut *tx).await?;
    if exists.is_none() {
        return bad("вариант не найден");
    }
    ensure_primary(&mut tx, variant_id, now).await?;
    sqlx::query(
        "INSERT INTO variant_suppliers (variant_id, priority, supplier_url, supplier_sku, supplier_price_minor, shipping_price_minor, supplier_stock, is_available, last_checked_at) \
         VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?) ON CONFLICT(variant_id, priority) DO UPDATE SET supplier_url = excluded.supplier_url, supplier_sku = excluded.supplier_sku, \
         supplier_price_minor = excluded.supplier_price_minor, shipping_price_minor = excluded.shipping_price_minor, supplier_stock = excluded.supplier_stock, is_available = 1, last_checked_at = excluded.last_checked_at",
    )
    .bind(variant_id)
    .bind(s.priority)
    .bind(url.as_str())
    .bind(s.sku.trim())
    .bind(s.price_minor)
    .bind(s.shipping_minor)
    .bind(s.stock)
    .bind(now)
    .execute(&mut *tx)
    .await?;
    refresh_variant(&mut tx, variant_id, now).await?;
    tx.commit().await?;
    Ok(())
}

pub async fn remove_backup(pool: &SqlitePool, variant_id: i64, priority: i64, now: i64) -> AppResult<()> {
    if !(2..=MAX_PRIORITY).contains(&priority) {
        return Err(AppError::Validation("удалять можно только резервных поставщиков (приоритет 2–3)".into()));
    }
    let mut tx = pool.begin().await?;
    sqlx::query("DELETE FROM variant_suppliers WHERE variant_id = ? AND priority = ?").bind(variant_id).bind(priority).execute(&mut *tx).await?;
    refresh_variant(&mut tx, variant_id, now).await?;
    tx.commit().await?;
    Ok(())
}

/// Результат проверки резервной страницы (вызывается мониторингом).
pub async fn record_backup_observation(pool: &SqlitePool, supplier_id: i64, price: Option<i64>, stock: Option<i64>, available: bool, now: i64) -> AppResult<()> {
    let mut tx = pool.begin().await?;
    let variant: Option<i64> = sqlx::query_scalar("UPDATE variant_suppliers SET supplier_price_minor = COALESCE(?, supplier_price_minor), supplier_stock = COALESCE(?, supplier_stock), is_available = ?, last_checked_at = ? WHERE id = ? RETURNING variant_id")
        .bind(price)
        .bind(stock)
        .bind(available)
        .bind(now)
        .bind(supplier_id)
        .fetch_optional(&mut *tx)
        .await?;
    if let Some(variant_id) = variant {
        refresh_variant(&mut tx, variant_id, now).await?;
    }
    tx.commit().await?;
    Ok(())
}

#[cfg(test)]
mod tests;
