//! Состояние вариантов товара: единственное место, где данные поставщика превращаются в цены витрины.
//! Импорт и мониторинг вызывают одну функцию `apply_variant_state`.

use sqlx::{Row, SqliteConnection, SqlitePool};
use xxhash_rust::xxh3::xxh3_64;

use crate::db::{PricingRuleRow, Store};
use crate::error::AppResult;
use crate::pricing::{self, PriceInput, RuleSet};

/// Хэш состояния `"{цена}:{доставка}:{остаток}"` (xxh3). Запрос к площадке нужен только при изменении.
pub fn state_hash(price_minor: i64, shipping_minor: i64, stock: i64) -> String {
    format!("{:016x}", xxh3_64(format!("{price_minor}:{shipping_minor}:{stock}").as_bytes()))
}

/// Правила магазина. Повреждённые или пересекающиеся — `None` (цены не рассчитываются, но работа не ломается).
pub async fn load_rule_set(conn: &mut SqliteConnection, store_id: i64) -> AppResult<Option<RuleSet>> {
    let rows: Vec<PricingRuleRow> = sqlx::query_as("SELECT * FROM pricing_rules WHERE store_id = ? ORDER BY price_from_minor")
        .bind(store_id)
        .fetch_all(&mut *conn)
        .await?;
    let rules: Result<Vec<_>, _> = rows.iter().map(PricingRuleRow::to_rule).collect();
    Ok(rules.ok().and_then(|r| RuleSet::new(r).ok()))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ApplyOutcome {
    Unchanged,
    /// Состояние изменилось; `priced` — цена витрины рассчитана по правилу.
    Updated { priced: bool },
}

/// Применяет новое состояние поставщика к варианту: пересчитывает цены по правилу магазина,
/// обновляет флаг наличия и пишет строку в `price_history`. Повторный вызов с теми же данными ничего не меняет.
pub async fn apply_variant_state(
    conn: &mut SqliteConnection,
    rules: Option<&RuleSet>,
    variant_id: i64,
    price_minor: i64,
    shipping_minor: i64,
    stock: i64,
    now: i64,
) -> AppResult<ApplyOutcome> {
    let current: Option<String> =
        sqlx::query_scalar("SELECT state_hash FROM product_variants WHERE id = ?").bind(variant_id).fetch_optional(&mut *conn).await?;
    let Some(current) = current else { return Ok(ApplyOutcome::Unchanged) };
    let hash = state_hash(price_minor, shipping_minor, stock);
    if current == hash {
        return Ok(ApplyOutcome::Unchanged);
    }

    let rule = rules.and_then(|set| set.find(price_minor));
    let breakdown = rule.and_then(|r| {
        pricing::calculate(r, &PriceInput { supplier_price_minor: price_minor, logistics_minor: shipping_minor, packaging_minor: 0 }).ok()
    });
    let (retail, min, old) = breakdown.as_ref().map(|b| (b.retail_price, b.min_price, b.old_price)).unwrap_or((0, 0, 0));
    let threshold = rule.map(|r| r.min_stock_threshold()).unwrap_or(1);
    let in_stock = stock >= threshold.max(1);

    sqlx::query(
        "UPDATE product_variants SET supplier_price_minor = ?, shipping_price_minor = ?, retail_price_minor = ?, \
         min_price_minor = ?, old_price_minor = ?, supplier_stock = ?, is_in_stock = ?, state_hash = ?, updated_at = ? WHERE id = ?",
    )
    .bind(price_minor)
    .bind(shipping_minor)
    .bind(retail)
    .bind(min)
    .bind(old)
    .bind(stock)
    .bind(in_stock)
    .bind(&hash)
    .bind(now)
    .bind(variant_id)
    .execute(&mut *conn)
    .await?;
    sqlx::query(
        "INSERT INTO price_history (variant_id, supplier_price_minor, shipping_price_minor, retail_price_minor, supplier_stock, recorded_at) \
         VALUES (?, ?, ?, ?, ?, ?)",
    )
    .bind(variant_id)
    .bind(price_minor)
    .bind(shipping_minor)
    .bind(retail)
    .bind(stock)
    .bind(now)
    .execute(&mut *conn)
    .await?;
    Ok(ApplyOutcome::Updated { priced: breakdown.is_some() })
}

/// Пересчитывает розничные цены всех вариантов магазина по текущим правилам (после смены правил).
pub async fn recalculate_store(pool: &SqlitePool, store_id: i64, now: i64) -> AppResult<usize> {
    let mut tx = pool.begin().await?;
    let rules = load_rule_set(&mut tx, store_id).await?;
    let variants: Vec<(i64, i64, i64, i64)> = sqlx::query_as(
        "SELECT v.id, v.supplier_price_minor, v.shipping_price_minor, v.supplier_stock FROM product_variants v JOIN products p ON p.id = v.product_id WHERE p.store_id = ?",
    )
    .bind(store_id)
    .fetch_all(&mut *tx)
    .await?;
    let mut changed = 0;
    for (id, price, shipping, stock) in variants {
        // Сбрасываем хэш, иначе «то же состояние» пропустит пересчёт.
        sqlx::query("UPDATE product_variants SET state_hash = '' WHERE id = ?").bind(id).execute(&mut *tx).await?;
        if !matches!(apply_variant_state(&mut tx, rules.as_ref(), id, price, shipping, stock, now).await?, ApplyOutcome::Unchanged) {
            changed += 1;
        }
    }
    tx.commit().await?;
    Ok(changed)
}

/// Магазин по умолчанию («Мой каталог», без площадки), чтобы импорт работал до подключения Ozon/Avito.
pub async fn ensure_default_store(pool: &SqlitePool, now: i64) -> AppResult<i64> {
    if let Some(id) = sqlx::query_scalar::<_, i64>("SELECT id FROM stores ORDER BY id LIMIT 1").fetch_optional(pool).await? {
        return Ok(id);
    }
    let id: i64 = sqlx::query_scalar(
        "INSERT INTO stores (name, platform_type, api_url, encrypted_credentials, created_at) VALUES ('Мой каталог', 'local', '', x'', ?) RETURNING id",
    )
    .bind(now)
    .fetch_one(pool)
    .await?;
    Ok(id)
}

pub async fn store_exists(pool: &SqlitePool, store_id: i64) -> AppResult<Option<Store>> {
    Ok(sqlx::query_as("SELECT * FROM stores WHERE id = ?").bind(store_id).fetch_optional(pool).await?)
}

/// Количество строк истории варианта (для тестов и интерфейса).
pub async fn history_len(pool: &SqlitePool, variant_id: i64) -> AppResult<i64> {
    Ok(sqlx::query("SELECT COUNT(*) AS n FROM price_history WHERE variant_id = ?").bind(variant_id).fetch_one(pool).await?.get("n"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::insert_product;

    async fn setup() -> (tempfile::TempDir, SqlitePool, i64, i64) {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("c.db")).await.unwrap();
        let (store, _, variant) = insert_product(&pool, "ozon", "sha").await;
        sqlx::query("UPDATE product_variants SET state_hash = ''").execute(&pool).await.unwrap();
        (dir, pool, store, variant)
    }

    async fn add_rule(pool: &SqlitePool, store: i64, margin: f64, min_stock: i64) {
        sqlx::query("INSERT INTO pricing_rules(store_id, price_from_minor, price_to_minor, margin_percent, min_stock_threshold, min_profit_minor, discount_striken_percent) VALUES (?, 0, 9000000000, ?, ?, 0, 30.0)")
            .bind(store).bind(margin).bind(min_stock).execute(pool).await.unwrap();
    }

    async fn apply(pool: &SqlitePool, store: i64, variant: i64, price: i64, stock: i64, now: i64) -> ApplyOutcome {
        let mut conn = pool.acquire().await.unwrap();
        let rules = load_rule_set(&mut conn, store).await.unwrap();
        apply_variant_state(&mut conn, rules.as_ref(), variant, price, 0, stock, now).await.unwrap()
    }

    #[test]
    fn hash_is_stable_and_sensitive() {
        assert_eq!(state_hash(1, 2, 3), state_hash(1, 2, 3));
        assert_ne!(state_hash(1, 2, 3), state_hash(1, 3, 2));
        assert_eq!(state_hash(1, 2, 3).len(), 16);
    }

    #[tokio::test]
    async fn change_prices_by_rule_records_history_and_is_idempotent() {
        let (_d, pool, store, variant) = setup().await;
        add_rule(&pool, store, 50.0, 1).await;
        assert_eq!(apply(&pool, store, variant, 100_000, 8, 10).await, ApplyOutcome::Updated { priced: true });
        let (retail, min, old, in_stock): (i64, i64, i64, bool) =
            sqlx::query_as("SELECT retail_price_minor, min_price_minor, old_price_minor, is_in_stock FROM product_variants WHERE id = ?")
                .bind(variant).fetch_one(&pool).await.unwrap();
        assert!(retail > 100_000 && min > 100_000 && min < retail && old > retail && in_stock);
        assert_eq!(apply(&pool, store, variant, 100_000, 8, 20).await, ApplyOutcome::Unchanged);
        assert_eq!(history_len(&pool, variant).await.unwrap(), 1);
        apply(&pool, store, variant, 90_000, 8, 30).await;
        assert_eq!(history_len(&pool, variant).await.unwrap(), 2);
    }

    #[tokio::test]
    async fn low_stock_marks_the_variant_out_of_stock_and_no_rule_means_no_price() {
        let (_d, pool, store, variant) = setup().await;
        assert_eq!(apply(&pool, store, variant, 100_000, 3, 10).await, ApplyOutcome::Updated { priced: false });
        let retail: i64 = sqlx::query_scalar("SELECT retail_price_minor FROM product_variants").fetch_one(&pool).await.unwrap();
        assert_eq!(retail, 0);
        add_rule(&pool, store, 50.0, 5).await;
        apply(&pool, store, variant, 100_000, 4, 20).await;
        let in_stock: bool = sqlx::query_scalar("SELECT is_in_stock FROM product_variants").fetch_one(&pool).await.unwrap();
        assert!(!in_stock);
        apply(&pool, store, variant, 100_000, 5, 30).await;
        let in_stock: bool = sqlx::query_scalar("SELECT is_in_stock FROM product_variants").fetch_one(&pool).await.unwrap();
        assert!(in_stock);
    }

    #[tokio::test]
    async fn broken_rules_do_not_break_updates_and_unknown_variants_are_ignored() {
        let (_d, pool, store, variant) = setup().await;
        add_rule(&pool, store, 50.0, 1).await;
        sqlx::query("UPDATE pricing_rules SET marketplace_fee_percent = 99").execute(&pool).await.unwrap();
        assert_eq!(apply(&pool, store, variant, 100_000, 8, 10).await, ApplyOutcome::Updated { priced: false });
        assert_eq!(apply(&pool, store, 9_999, 100_000, 8, 10).await, ApplyOutcome::Unchanged);
    }

    #[tokio::test]
    async fn default_store_is_created_once() {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("d.db")).await.unwrap();
        let a = ensure_default_store(&pool, 1).await.unwrap();
        assert_eq!(ensure_default_store(&pool, 2).await.unwrap(), a);
        let store = store_exists(&pool, a).await.unwrap().unwrap();
        assert_eq!((store.name.as_str(), store.platform_type), ("Мой каталог", crate::db::PlatformType::Local));
    }
}
