//! API интерфейса: всё, что делают четыре вкладки приложения. Функции принимают пул/ключи и время параметрами —
//! поэтому тестируются без окна; тонкие обёртки `#[tauri::command]` лежат в `lib.rs`.

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sqlx::SqlitePool;

use crate::ai::worker::enqueue_ai_rewrite;
use crate::catalog;
use crate::db::{PlatformType, PricingRuleRow, Store};
use crate::error::{AppError, AppResult};
use crate::pricing::{PricingRule, RuleSpec};
use crate::security::CredentialVault;
use crate::settings;
use crate::stores::{self, NewStore};

const SEARCH_SCAN_LIMIT: i64 = 5_000;

fn invalid<T>(message: &str) -> AppResult<T> {
    Err(AppError::Validation(message.to_string()))
}

// ---------------------------------------------------------------------------------------------
// Вкладки «Импорт и Черновики» / «Мои товары»
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum ProductFilter {
    /// Не привязаны ни к одному товару площадки: ещё готовятся.
    Drafts,
    /// Привязаны хотя бы одним вариантом: цены и остатки уходят на витрину.
    Mine,
    All,
}

#[derive(Debug, Serialize, sqlx::FromRow)]
pub struct ProductRow {
    pub id: i64,
    pub title: String,
    pub raw_title: String,
    pub ai_status: String,
    pub monitor_status: String,
    pub supplier_url: String,
    pub variants: i64,
    pub linked: i64,
    pub min_retail_minor: Option<i64>,
    pub stock: i64,
    pub last_checked_at: i64,
    pub image: Option<String>,
}

pub async fn list_products(pool: &SqlitePool, filter: ProductFilter, search: &str, limit: i64, offset: i64) -> AppResult<Vec<ProductRow>> {
    let having = match filter {
        ProductFilter::Drafts => "HAVING linked = 0",
        ProductFilter::Mine => "HAVING linked > 0",
        ProductFilter::All => "",
    };
    let needle = search.trim().to_lowercase();
    // Поиск без учёта регистра делаем в Rust: SQLite приводит к нижнему регистру только латиницу, а у нас кириллица.
    let (sql_limit, sql_offset) = if needle.is_empty() { (limit.clamp(1, 200), offset.max(0)) } else { (SEARCH_SCAN_LIMIT, 0) };
    let sql = format!(
        "SELECT p.id AS id, COALESCE(NULLIF(p.ai_seo_title, ''), p.raw_title) AS title, p.raw_title AS raw_title, p.ai_status AS ai_status, \
                p.monitor_status AS monitor_status, p.supplier_url AS supplier_url, COUNT(v.id) AS variants, \
                COALESCE(SUM(CASE WHEN v.external_variant_id IS NOT NULL AND v.external_variant_id <> '' THEN 1 ELSE 0 END), 0) AS linked, \
                MIN(NULLIF(v.retail_price_minor, 0)) AS min_retail_minor, COALESCE(SUM(v.supplier_stock), 0) AS stock, \
                p.last_checked_at AS last_checked_at, json_extract(p.images_json, '$[0]') AS image \
         FROM products p LEFT JOIN product_variants v ON v.product_id = p.id \
         GROUP BY p.id {having} ORDER BY p.id DESC LIMIT ?1 OFFSET ?2"
    );
    let rows: Vec<ProductRow> = sqlx::query_as(&sql).bind(sql_limit).bind(sql_offset).fetch_all(pool).await?;
    if needle.is_empty() {
        return Ok(rows);
    }
    let matching = rows.into_iter().filter(|r| r.title.to_lowercase().contains(&needle) || r.raw_title.to_lowercase().contains(&needle));
    Ok(matching.skip(offset.max(0) as usize).take(limit.clamp(1, 200) as usize).collect())
}

#[derive(Debug, Serialize, sqlx::FromRow)]
pub struct VariantDetail {
    pub id: i64,
    pub supplier_sku: String,
    pub attributes_json: String,
    pub supplier_price_minor: i64,
    pub shipping_price_minor: i64,
    pub retail_price_minor: i64,
    pub min_price_minor: i64,
    pub old_price_minor: i64,
    pub supplier_stock: i64,
    pub is_in_stock: bool,
    pub external_variant_id: Option<String>,
    pub sync_state: String,
}

#[derive(Debug, Serialize, sqlx::FromRow)]
pub struct HistoryPoint {
    pub variant_id: i64,
    pub supplier_price_minor: i64,
    pub retail_price_minor: i64,
    pub supplier_stock: i64,
    pub recorded_at: i64,
}

#[derive(sqlx::FromRow)]
struct ProductTexts {
    raw_title: String,
    raw_description: Option<String>,
    ai_title: Option<String>,
    ai_description: Option<String>,
    supplier_url: String,
    ai_status: String,
    monitor_status: String,
}

pub async fn product_detail(pool: &SqlitePool, product_id: i64) -> AppResult<Value> {
    let product: Option<ProductTexts> = sqlx::query_as(
        "SELECT raw_title, raw_description, ai_seo_title AS ai_title, ai_description, supplier_url, ai_status, monitor_status FROM products WHERE id = ?",
    )
    .bind(product_id)
    .fetch_optional(pool)
    .await?;
    let Some(ProductTexts { raw_title, raw_description, ai_title, ai_description, supplier_url, ai_status, monitor_status }) = product else { return invalid("товар не найден") };
    let variants: Vec<VariantDetail> = sqlx::query_as(
        "SELECT id, supplier_sku, attributes_json, supplier_price_minor, shipping_price_minor, retail_price_minor, min_price_minor, old_price_minor, \
                supplier_stock, is_in_stock, external_variant_id, \
                CASE WHEN external_variant_id IS NULL OR external_variant_id = '' THEN 'not_linked' \
                     WHEN state_hash = COALESCE(published_hash, '') THEN 'synced' \
                     WHEN state_hash = COALESCE(rejected_hash, '') THEN 'rejected' ELSE 'pending' END AS sync_state \
         FROM product_variants WHERE product_id = ? ORDER BY id",
    )
    .bind(product_id)
    .fetch_all(pool)
    .await?;
    let history: Vec<HistoryPoint> = sqlx::query_as(
        "SELECT h.variant_id, h.supplier_price_minor, h.retail_price_minor, h.supplier_stock, h.recorded_at FROM price_history h \
         JOIN product_variants v ON v.id = h.variant_id WHERE v.product_id = ? ORDER BY h.recorded_at DESC, h.id DESC LIMIT 200",
    )
    .bind(product_id)
    .fetch_all(pool)
    .await?;
    Ok(json!({
        "id": product_id, "raw_title": raw_title, "raw_description": raw_description, "ai_title": ai_title, "ai_description": ai_description,
        "supplier_url": supplier_url, "ai_status": ai_status, "monitor_status": monitor_status, "variants": variants, "history": history,
    }))
}

/// Ручная правка текста карточки: пользовательский текст важнее сгенерированного.
pub async fn update_product_text(pool: &SqlitePool, product_id: i64, title: &str, description: &str) -> AppResult<()> {
    let (title, description) = (title.trim(), description.trim());
    if title.is_empty() || title.chars().count() > 500 || description.chars().count() > 20_000 {
        return invalid("название — от 1 до 500 символов, описание — до 20 000");
    }
    let done = sqlx::query("UPDATE products SET ai_seo_title = ?, ai_description = ?, ai_status = 'READY' WHERE id = ?").bind(title).bind(description).bind(product_id).execute(pool).await?;
    if done.rows_affected() == 0 {
        return invalid("товар не найден");
    }
    Ok(())
}

pub async fn retry_ai(pool: &SqlitePool, product_id: i64, now: i64) -> AppResult<bool> {
    enqueue_ai_rewrite(pool, product_id, now).await
}

/// Привязка варианта к товару на площадке (offer_id Ozon / id объявления Avito). Пусто — отвязать.
pub async fn link_variant(pool: &SqlitePool, variant_id: i64, external_id: &str) -> AppResult<()> {
    let external = external_id.trim();
    if external.len() > 200 || external.chars().any(char::is_control) {
        return invalid("идентификатор товара на площадке некорректен");
    }
    let value = (!external.is_empty()).then_some(external);
    let done = sqlx::query("UPDATE product_variants SET external_variant_id = ?, published_hash = NULL, rejected_hash = NULL WHERE id = ?").bind(value).bind(variant_id).execute(pool).await?;
    if done.rows_affected() == 0 {
        return invalid("вариант не найден");
    }
    Ok(())
}

pub async fn set_monitoring(pool: &SqlitePool, product_id: i64, active: bool) -> AppResult<()> {
    let status = if active { "ACTIVE" } else { "PAUSED" };
    let done = sqlx::query("UPDATE products SET monitor_status = ?, next_check_at = 0 WHERE id = ?").bind(status).bind(product_id).execute(pool).await?;
    if done.rows_affected() == 0 {
        return invalid("товар не найден");
    }
    Ok(())
}

/// Удаление товара вместе с вариантами и историей. Заказы остаются (ссылка на вариант обнуляется).
pub async fn delete_product(pool: &SqlitePool, product_id: i64) -> AppResult<()> {
    let mut tx = pool.begin().await?;
    sqlx::query("UPDATE orders SET variant_id = NULL WHERE variant_id IN (SELECT id FROM product_variants WHERE product_id = ?)").bind(product_id).execute(&mut *tx).await?;
    let done = sqlx::query("DELETE FROM products WHERE id = ?").bind(product_id).execute(&mut *tx).await?;
    if done.rows_affected() == 0 {
        return invalid("товар не найден");
    }
    tx.commit().await?;
    Ok(())
}

// ---------------------------------------------------------------------------------------------
// Вкладка «Настройки»: магазины, правила цен, прокси
// ---------------------------------------------------------------------------------------------

pub async fn list_stores(pool: &SqlitePool) -> AppResult<Vec<Value>> {
    let stores: Vec<Store> = sqlx::query_as("SELECT * FROM stores ORDER BY id").fetch_all(pool).await?;
    let mut out = Vec::new();
    for s in stores {
        let (products,): (i64,) = sqlx::query_as("SELECT COUNT(*) FROM products WHERE store_id = ?").bind(s.id).fetch_one(pool).await?;
        let last_error: Option<String> = sqlx::query_scalar("SELECT last_error FROM stores WHERE id = ?").bind(s.id).fetch_one(pool).await?;
        let mut value = serde_json::to_value(&s).unwrap_or_default();
        if let Some(map) = value.as_object_mut() {
            map.insert("products".into(), json!(products));
            map.insert("last_error".into(), json!(last_error));
        }
        out.push(value);
    }
    Ok(out)
}

#[derive(Debug, Deserialize)]
pub struct NewStoreInput {
    pub name: String,
    pub platform: String,
    pub credentials: Value,
    pub warehouse_id: Option<String>,
}

pub async fn add_store(pool: &SqlitePool, vault: &CredentialVault, input: &NewStoreInput, now: i64) -> AppResult<i64> {
    let platform = match input.platform.as_str() {
        "ozon" => PlatformType::Ozon,
        "avito" => PlatformType::Avito,
        _ => return invalid("пока поддерживаются Ozon и Avito"),
    };
    let credentials = input.credentials.to_string();
    stores::add_store(pool, vault, NewStore { name: &input.name, platform, credentials_json: &credentials, warehouse_id: input.warehouse_id.as_deref(), api_url: None, now }).await
}

pub async fn set_store_active(pool: &SqlitePool, store_id: i64, active: bool) -> AppResult<()> {
    sqlx::query("UPDATE stores SET is_active = ? WHERE id = ?").bind(active).bind(store_id).execute(pool).await?;
    Ok(())
}

/// Магазин с товарами не удаляется: каскад стёр бы весь каталог. Сначала удалите или перенесите товары.
pub async fn delete_store(pool: &SqlitePool, store_id: i64) -> AppResult<()> {
    let (products,): (i64,) = sqlx::query_as("SELECT COUNT(*) FROM products WHERE store_id = ?").bind(store_id).fetch_one(pool).await?;
    if products > 0 {
        return invalid("в магазине есть товары — сначала удалите их, чтобы не потерять каталог");
    }
    sqlx::query("DELETE FROM stores WHERE id = ?").bind(store_id).execute(pool).await?;
    Ok(())
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RuleInput {
    pub rule_name: String,
    pub price_from_minor: i64,
    pub price_to_minor: i64,
    pub margin_percent: f64,
    pub fixed_markup_minor: i64,
    pub marketplace_fee_percent: f64,
    pub acquiring_and_tax_percent: f64,
    pub discount_striken_percent: f64,
    pub rounding_minor: i64,
    pub min_stock_threshold: i64,
    pub min_profit_minor: i64,
}

impl RuleInput {
    /// Разумный старт для новичка: наценка 50%, комиссия 15%, налог/эквайринг 6%, минимальная прибыль 300 ₽.
    pub fn standard() -> Self {
        Self {
            rule_name: "Стандартное правило".into(),
            price_from_minor: 0,
            price_to_minor: 100_000_000_000,
            margin_percent: 50.0,
            fixed_markup_minor: 0,
            marketplace_fee_percent: 15.0,
            acquiring_and_tax_percent: 6.0,
            discount_striken_percent: 30.0,
            rounding_minor: 100,
            min_stock_threshold: 5,
            min_profit_minor: 30_000,
        }
    }

    fn to_spec(&self) -> RuleSpec {
        RuleSpec {
            price_from_minor: self.price_from_minor,
            price_to_minor: self.price_to_minor,
            margin_percent: self.margin_percent,
            fixed_markup_minor: self.fixed_markup_minor,
            marketplace_fee_percent: self.marketplace_fee_percent,
            acquiring_and_tax_percent: self.acquiring_and_tax_percent,
            discount_striken_percent: self.discount_striken_percent,
            rounding_minor: self.rounding_minor,
            min_stock_threshold: self.min_stock_threshold,
            min_profit_minor: self.min_profit_minor,
        }
    }
}

pub async fn list_rules(pool: &SqlitePool, store_id: i64) -> AppResult<Vec<PricingRuleRow>> {
    Ok(sqlx::query_as("SELECT * FROM pricing_rules WHERE store_id = ? ORDER BY price_from_minor").bind(store_id).fetch_all(pool).await?)
}

/// Заменяет все правила магазина целиком (после проверки всего набора) и пересчитывает цены каталога.
pub async fn save_rules(pool: &SqlitePool, store_id: i64, rules: &[RuleInput], now: i64) -> AppResult<usize> {
    if rules.is_empty() || rules.len() > 50 {
        return invalid("нужно от 1 до 50 правил");
    }
    let checked = rules.iter().map(|r| PricingRule::new(r.to_spec())).collect::<Result<Vec<_>, _>>()?;
    crate::pricing::RuleSet::new(checked)?;
    if catalog::store_exists(pool, store_id).await?.is_none() {
        return invalid("магазин не найден");
    }
    let mut tx = pool.begin().await?;
    sqlx::query("DELETE FROM pricing_rules WHERE store_id = ?").bind(store_id).execute(&mut *tx).await?;
    for r in rules {
        sqlx::query(
            "INSERT INTO pricing_rules (store_id, rule_name, price_from_minor, price_to_minor, margin_percent, fixed_markup_minor, marketplace_fee_percent, \
             acquiring_and_tax_percent, discount_striken_percent, rounding_minor, min_stock_threshold, min_profit_minor) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        )
        .bind(store_id)
        .bind(r.rule_name.trim())
        .bind(r.price_from_minor)
        .bind(r.price_to_minor)
        .bind(r.margin_percent)
        .bind(r.fixed_markup_minor)
        .bind(r.marketplace_fee_percent)
        .bind(r.acquiring_and_tax_percent)
        .bind(r.discount_striken_percent)
        .bind(r.rounding_minor)
        .bind(r.min_stock_threshold)
        .bind(r.min_profit_minor)
        .execute(&mut *tx)
        .await?;
    }
    tx.commit().await?;
    catalog::recalculate_store(pool, store_id, now).await?;
    Ok(rules.len())
}

pub async fn list_proxies(pool: &SqlitePool) -> AppResult<Vec<Value>> {
    let rows: Vec<crate::db::Proxy> = sqlx::query_as("SELECT * FROM proxies ORDER BY id").fetch_all(pool).await?;
    Ok(rows.iter().filter_map(|p| serde_json::to_value(p).ok()).collect())
}

pub async fn remove_proxy(pool: &SqlitePool, proxy_id: i64) -> AppResult<()> {
    sqlx::query("DELETE FROM proxies WHERE id = ?").bind(proxy_id).execute(pool).await?;
    Ok(())
}

// ---------------------------------------------------------------------------------------------
// Мастер первого запуска и расширение
// ---------------------------------------------------------------------------------------------

const ONBOARDING_KEY: &str = "onboarding_done";

pub async fn onboarding_state(pool: &SqlitePool) -> AppResult<Value> {
    let done = settings::get(pool, ONBOARDING_KEY).await?.is_some();
    let (stores,): (i64,) = sqlx::query_as("SELECT COUNT(*) FROM stores").fetch_one(pool).await?;
    let (rules,): (i64,) = sqlx::query_as("SELECT COUNT(*) FROM pricing_rules").fetch_one(pool).await?;
    let ai_configured = settings::get(pool, "ai").await?.is_some();
    let paired = settings::get(pool, "bridge_token_sha256").await?.is_some();
    Ok(json!({ "done": done, "has_store": stores > 0, "has_rules": rules > 0, "ai_configured": ai_configured, "extension_paired": paired }))
}

/// Шаг мастера «Начать с нуля»: локальный каталог + стандартные правила цены. Повторный вызов ничего не дублирует.
pub async fn setup_default_catalog(pool: &SqlitePool, now: i64) -> AppResult<i64> {
    let store_id = catalog::ensure_default_store(pool, now).await?;
    if list_rules(pool, store_id).await?.is_empty() {
        save_rules(pool, store_id, &[RuleInput::standard()], now).await?;
    }
    Ok(store_id)
}

pub async fn finish_onboarding(pool: &SqlitePool, now: i64) -> AppResult<()> {
    settings::set(pool, ONBOARDING_KEY, "1", now).await
}

#[cfg(test)]
mod tests;
