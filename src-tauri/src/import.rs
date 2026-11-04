//! Импорт товара, присланного расширением (`IMPORT_PRODUCT_PAYLOAD`).
//! Данные проверяются один раз — на входе (`parse_import_payload`); внутри им доверяют.
//! Идемпотентно: тот же товар (по каноническому URL) обновляет цены/остатки, а не создаёт копию.

use serde_json::Value;
use sha2::{Digest, Sha256};
use sqlx::SqlitePool;
use url::Url;

use crate::ai::worker::enqueue_ai_rewrite;
use crate::catalog::{self, apply_variant_state};
use crate::error::{AppError, AppResult};

const MAX_TITLE: usize = 500;
const MAX_DESCRIPTION: usize = 20_000;
const MAX_URL: usize = 2_000;
const MAX_IMAGES: usize = 30;
const MAX_VARIANTS: usize = 200;
const MAX_SKU: usize = 200;
const MAX_AMOUNT: i64 = 1_000_000_000_000;
const MAX_STOCK: i64 = 1_000_000_000;
const DEFAULT_CHECK_INTERVAL_SECS: i64 = 1_800;

#[derive(Debug, Clone, PartialEq)]
pub struct VariantPayload {
    pub sku: String,
    pub attributes_json: String,
    pub price_minor: i64,
    pub shipping_minor: i64,
    pub stock: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ImportPayload {
    pub url: String,
    pub title: String,
    pub description: Option<String>,
    pub images: Vec<String>,
    pub variants: Vec<VariantPayload>,
    pub store_id: Option<i64>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ImportOutcome {
    pub product_id: i64,
    pub duplicate: bool,
    pub variants: usize,
}

fn invalid<T>(message: &str) -> AppResult<T> {
    Err(AppError::Validation(message.to_string()))
}

/// Канонический URL: схема и хост в нижнем регистре, без фрагмента и рекламных меток (utm_*, gclid, fbclid, yclid).
pub fn canonical_url(raw: &str) -> AppResult<String> {
    if raw.len() > MAX_URL {
        return invalid("ссылка слишком длинная");
    }
    let mut url = Url::parse(raw.trim()).map_err(|_| AppError::Validation("некорректная ссылка на товар".into()))?;
    if url.scheme() != "http" && url.scheme() != "https" {
        return invalid("ссылка должна начинаться с http:// или https://");
    }
    if url.host_str().is_none() {
        return invalid("в ссылке нет адреса сайта");
    }
    url.set_fragment(None);
    let kept: Vec<(String, String)> = url
        .query_pairs()
        .filter(|(k, _)| !(k.starts_with("utm_") || matches!(k.as_ref(), "gclid" | "fbclid" | "yclid")))
        .map(|(k, v)| (k.into_owned(), v.into_owned()))
        .collect();
    if kept.is_empty() {
        url.set_query(None);
    } else {
        url.query_pairs_mut().clear().extend_pairs(kept);
    }
    Ok(url.to_string())
}

pub fn url_sha256(canonical: &str) -> String {
    Sha256::digest(canonical.as_bytes()).iter().map(|b| format!("{b:02x}")).collect()
}

fn text(value: &Value, key: &str, max: usize) -> AppResult<Option<String>> {
    match value.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(s)) => {
            if s.chars().count() > max {
                return invalid(&format!("поле {key} слишком длинное"));
            }
            Ok(Some(s.trim().to_string()))
        }
        Some(_) => invalid(&format!("поле {key} должно быть строкой")),
    }
}

fn amount(value: &Value, key: &str, max: i64, required: bool) -> AppResult<i64> {
    match value.get(key) {
        None | Some(Value::Null) if !required => Ok(0),
        Some(v) => v
            .as_i64()
            .filter(|n| (0..=max).contains(n))
            .ok_or_else(|| AppError::Validation(format!("поле {key} должно быть целым числом в допустимых пределах"))),
        None => invalid(&format!("нет поля {key}")),
    }
}

fn parse_variant(value: &Value) -> AppResult<VariantPayload> {
    let sku = text(value, "sku", MAX_SKU)?.filter(|s| !s.is_empty()).ok_or_else(|| AppError::Validation("у варианта нет артикула (sku)".into()))?;
    let price = amount(value, "price_cents", MAX_AMOUNT, true)?;
    if price == 0 {
        return invalid("цена варианта должна быть больше нуля");
    }
    let attributes = match value.get("attributes") {
        None | Some(Value::Null) => Value::Object(Default::default()),
        Some(Value::Object(map)) if map.len() <= 50 && map.values().all(|v| matches!(v, Value::String(_) | Value::Number(_)) && v.to_string().len() <= 400) => {
            Value::Object(map.clone())
        }
        Some(_) => return invalid("атрибуты варианта некорректны"),
    };
    Ok(VariantPayload {
        sku,
        attributes_json: attributes.to_string(),
        price_minor: price,
        shipping_minor: amount(value, "shipping_cents", MAX_AMOUNT, false)?,
        stock: amount(value, "stock", MAX_STOCK, true)?,
    })
}

pub fn parse_import_payload(value: &Value) -> AppResult<ImportPayload> {
    if !value.is_object() {
        return invalid("данные товара должны быть объектом");
    }
    let url = canonical_url(&text(value, "url", MAX_URL)?.unwrap_or_default())?;
    let title = text(value, "title", MAX_TITLE)?.filter(|t| !t.is_empty()).ok_or_else(|| AppError::Validation("у товара нет названия".into()))?;
    let description = text(value, "description", MAX_DESCRIPTION)?.filter(|d| !d.is_empty());
    let images = match value.get("images") {
        None | Some(Value::Null) => Vec::new(),
        Some(Value::Array(list)) if list.len() <= MAX_IMAGES => list
            .iter()
            .map(|v| v.as_str().filter(|s| s.len() <= MAX_URL && (s.starts_with("https://") || s.starts_with("http://"))).map(str::to_string))
            .collect::<Option<Vec<_>>>()
            .ok_or_else(|| AppError::Validation("ссылки на изображения некорректны".into()))?,
        Some(_) => return invalid("слишком много изображений"),
    };
    let variants = match value.get("variants") {
        Some(Value::Array(list)) if !list.is_empty() && list.len() <= MAX_VARIANTS => list.iter().map(parse_variant).collect::<AppResult<Vec<_>>>()?,
        _ => return invalid("у товара должен быть хотя бы один вариант (не более 200)"),
    };
    let mut skus: Vec<&str> = variants.iter().map(|v| v.sku.as_str()).collect();
    skus.sort_unstable();
    skus.dedup();
    if skus.len() != variants.len() {
        return invalid("артикулы вариантов повторяются");
    }
    let store_id = match value.get("store_id") {
        None | Some(Value::Null) => None,
        Some(v) => Some(v.as_i64().filter(|n| *n > 0).ok_or_else(|| AppError::Validation("номер магазина некорректен".into()))?),
    };
    Ok(ImportPayload { url, title, description, images, variants, store_id })
}

/// Сохраняет товар. Новый — ставит генерацию карточки ИИ; уже известный — обновляет состояние вариантов.
pub async fn import_product(pool: &SqlitePool, payload: &ImportPayload, now: i64) -> AppResult<ImportOutcome> {
    let store_id = match payload.store_id {
        Some(id) => catalog::store_exists(pool, id).await?.map(|s| s.id).ok_or_else(|| AppError::Validation("такого магазина нет".into()))?,
        None => catalog::ensure_default_store(pool, now).await?,
    };
    let sha = url_sha256(&payload.url);

    let mut tx = pool.begin().await?;
    let rules = catalog::load_rule_set(&mut tx, store_id).await?;
    let existing: Option<i64> = sqlx::query_scalar("SELECT id FROM products WHERE url_sha256 = ?").bind(&sha).fetch_optional(&mut *tx).await?;
    let duplicate = existing.is_some();
    let product_id = match existing {
        Some(id) => {
            sqlx::query("UPDATE products SET raw_title = ?, raw_description = ?, images_json = ? WHERE id = ?")
                .bind(&payload.title)
                .bind(&payload.description)
                .bind(serde_json::to_string(&payload.images).unwrap_or_else(|_| "[]".into()))
                .bind(id)
                .execute(&mut *tx)
                .await?;
            id
        }
        None => {
            let supplier_product_id = payload.variants.first().map(|v| v.sku.clone()).unwrap_or_default();
            sqlx::query_scalar(
                "INSERT INTO products (store_id, external_offer_id, supplier_url, supplier_product_id, url_sha256, raw_title, \
                 raw_description, images_json, check_interval_sec, next_check_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id",
            )
            .bind(store_id)
            .bind(format!("dp-{}", &sha[..12]))
            .bind(&payload.url)
            .bind(supplier_product_id)
            .bind(&sha)
            .bind(&payload.title)
            .bind(&payload.description)
            .bind(serde_json::to_string(&payload.images).unwrap_or_else(|_| "[]".into()))
            .bind(DEFAULT_CHECK_INTERVAL_SECS)
            .bind(now + DEFAULT_CHECK_INTERVAL_SECS)
            .fetch_one(&mut *tx)
            .await?
        }
    };

    for variant in &payload.variants {
        let variant_id: i64 = match sqlx::query_scalar("SELECT id FROM product_variants WHERE product_id = ? AND supplier_sku = ?")
            .bind(product_id)
            .bind(&variant.sku)
            .fetch_optional(&mut *tx)
            .await?
        {
            Some(id) => id,
            None => {
                sqlx::query_scalar(
                    "INSERT INTO product_variants (product_id, supplier_sku, attributes_json, supplier_price_minor, shipping_price_minor, \
                     retail_price_minor, min_price_minor, old_price_minor, supplier_stock, state_hash, updated_at) \
                     VALUES (?, ?, ?, ?, ?, 0, 0, 0, ?, '', ?) RETURNING id",
                )
                .bind(product_id)
                .bind(&variant.sku)
                .bind(&variant.attributes_json)
                .bind(variant.price_minor)
                .bind(variant.shipping_minor)
                .bind(variant.stock)
                .bind(now)
                .fetch_one(&mut *tx)
                .await?
            }
        };
        apply_variant_state(&mut tx, rules.as_ref(), variant_id, variant.price_minor, variant.shipping_minor, variant.stock, now).await?;
    }
    tx.commit().await?;

    if !duplicate {
        enqueue_ai_rewrite(pool, product_id, now).await?;
    }
    Ok(ImportOutcome { product_id, duplicate, variants: payload.variants.len() })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn payload() -> Value {
        json!({
            "url": "https://Shop.Example/item/1?utm_source=x&color=red#reviews",
            "title": " Наушники ", "description": "Bluetooth",
            "images": ["https://img.example/1.jpg"],
            "variants": [
                {"sku": "A", "attributes": {"color": "Black"}, "price_cents": 100000, "stock": 8},
                {"sku": "B", "price_cents": 120000, "shipping_cents": 5000, "stock": 0}
            ]
        })
    }

    async fn pool() -> (tempfile::TempDir, SqlitePool) {
        let dir = tempfile::tempdir().unwrap();
        (dir.path().to_owned().into(), crate::db::init_pool(&dir.path().join("i.db")).await.unwrap()).into_pair(dir)
    }

    trait IntoPair {
        fn into_pair(self, dir: tempfile::TempDir) -> (tempfile::TempDir, SqlitePool);
    }
    impl IntoPair for (std::path::PathBuf, SqlitePool) {
        fn into_pair(self, dir: tempfile::TempDir) -> (tempfile::TempDir, SqlitePool) {
            (dir, self.1)
        }
    }

    #[test]
    fn canonical_url_drops_tracking_and_fragment_and_lowercases() {
        assert_eq!(canonical_url("https://Shop.Example/item/1?utm_source=x&color=red#reviews").unwrap(), "https://shop.example/item/1?color=red");
        assert_eq!(canonical_url("https://shop.example/a?utm_medium=1&gclid=2").unwrap(), "https://shop.example/a");
        assert_eq!(url_sha256("a").len(), 64);
        for bad in ["javascript:alert(1)", "file:///etc/passwd", "ftp://x", "not a url", ""] {
            assert!(canonical_url(bad).is_err(), "{bad}");
        }
        assert!(canonical_url(&format!("https://a.example/{}", "x".repeat(3000))).is_err());
    }

    #[test]
    fn valid_payload_is_parsed_and_normalized() {
        let p = parse_import_payload(&payload()).unwrap();
        assert_eq!((p.title.as_str(), p.variants.len(), p.variants[1].shipping_minor), ("Наушники", 2, 5000));
        assert_eq!(p.variants[0].attributes_json, r#"{"color":"Black"}"#);
        assert_eq!(p.store_id, None);
    }

    #[test]
    fn bad_payloads_are_rejected_at_the_boundary() {
        let cases: Vec<Box<dyn Fn(&mut Value)>> = vec![
            Box::new(|v| { v["title"] = json!(""); }),
            Box::new(|v| { v["title"] = json!(5); }),
            Box::new(|v| { v["title"] = json!("x".repeat(501)); }),
            Box::new(|v| { v["url"] = json!("javascript:1"); }),
            Box::new(|v| { v["variants"] = json!([]); }),
            Box::new(|v| { v["variants"] = json!("x"); }),
            Box::new(|v| { v["variants"][0]["price_cents"] = json!(0); }),
            Box::new(|v| { v["variants"][0]["price_cents"] = json!(-5); }),
            Box::new(|v| { v["variants"][0]["price_cents"] = json!(1.5); }),
            Box::new(|v| { v["variants"][0]["stock"] = json!(-1); }),
            Box::new(|v| { v["variants"][0]["sku"] = json!(""); }),
            Box::new(|v| { v["variants"][1]["sku"] = json!("A"); }),
            Box::new(|v| { v["variants"][0]["attributes"] = json!({"a": {"nested": 1}}); }),
            Box::new(|v| { v["images"] = json!(["javascript:1"]); }),
            Box::new(|v| { v["images"] = json!(vec!["https://a.example/i.jpg"; 31]); }),
            Box::new(|v| { v["store_id"] = json!(0); }),
        ];
        for mutate in cases {
            let mut v = payload();
            mutate(&mut v);
            assert!(parse_import_payload(&v).is_err(), "{v}");
        }
        assert!(parse_import_payload(&json!([])).is_err());
    }

    #[tokio::test]
    async fn new_product_is_saved_priced_and_queued_for_ai() {
        let (_d, pool) = pool().await;
        let store = catalog::ensure_default_store(&pool, 0).await.unwrap();
        sqlx::query("INSERT INTO pricing_rules(store_id, price_from_minor, price_to_minor, margin_percent, min_profit_minor) VALUES (?, 0, 9000000000, 50.0, 0)")
            .bind(store).execute(&pool).await.unwrap();
        let out = import_product(&pool, &parse_import_payload(&payload()).unwrap(), 100).await.unwrap();
        assert_eq!((out.duplicate, out.variants), (false, 2));
        let (title, sha, status): (String, String, String) =
            sqlx::query_as("SELECT raw_title, url_sha256, ai_status FROM products WHERE id = ?").bind(out.product_id).fetch_one(&pool).await.unwrap();
        assert_eq!((title.as_str(), sha.len(), status.as_str()), ("Наушники", 64, "PENDING"));
        let retail: i64 = sqlx::query_scalar("SELECT retail_price_minor FROM product_variants WHERE supplier_sku = 'A'").fetch_one(&pool).await.unwrap();
        assert!(retail > 100_000);
        let out_of_stock: bool = sqlx::query_scalar("SELECT NOT is_in_stock FROM product_variants WHERE supplier_sku = 'B'").fetch_one(&pool).await.unwrap();
        assert!(out_of_stock);
        let tasks: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue WHERE task_type = 'AI_REWRITE'").fetch_one(&pool).await.unwrap();
        assert_eq!(tasks, 1);
        let history: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM price_history").fetch_one(&pool).await.unwrap();
        assert_eq!(history, 2);
    }

    #[tokio::test]
    async fn importing_again_updates_instead_of_duplicating() {
        let (_d, pool) = pool().await;
        let first = import_product(&pool, &parse_import_payload(&payload()).unwrap(), 100).await.unwrap();
        let mut changed = payload();
        changed["url"] = json!("https://shop.example/item/1?color=red&utm_campaign=z"); // та же страница
        changed["variants"][0]["price_cents"] = json!(90000);
        changed["variants"].as_array_mut().unwrap().push(json!({"sku": "C", "price_cents": 50000, "stock": 2}));
        let second = import_product(&pool, &parse_import_payload(&changed).unwrap(), 200).await.unwrap();
        assert_eq!((second.product_id, second.duplicate), (first.product_id, true));
        let variants: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM product_variants").fetch_one(&pool).await.unwrap();
        let products: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM products").fetch_one(&pool).await.unwrap();
        assert_eq!((products, variants), (1, 3));
        let price: i64 = sqlx::query_scalar("SELECT supplier_price_minor FROM product_variants WHERE supplier_sku = 'A'").fetch_one(&pool).await.unwrap();
        assert_eq!(price, 90_000);
        let ai_tasks: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue").fetch_one(&pool).await.unwrap();
        assert_eq!(ai_tasks, 1); // повторный импорт не плодит задачи ИИ
    }

    #[tokio::test]
    async fn unknown_store_is_rejected() {
        let (_d, pool) = pool().await;
        let mut v = payload();
        v["store_id"] = json!(42);
        assert!(import_product(&pool, &parse_import_payload(&v).unwrap(), 1).await.is_err());
        let products: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM products").fetch_one(&pool).await.unwrap();
        assert_eq!(products, 0);
    }
}
