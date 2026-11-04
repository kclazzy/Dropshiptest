use super::*;
use crate::pricing::{PricingRule, RuleSpec};

fn rules(min_stock: i64, min_profit: i64) -> RuleSet {
    RuleSet::new(vec![PricingRule::new(RuleSpec {
        price_from_minor: 0, price_to_minor: 100_000_000, margin_percent: 50.0, fixed_markup_minor: 0, marketplace_fee_percent: 15.0,
        acquiring_and_tax_percent: 6.0, discount_striken_percent: 30.0, rounding_minor: 0, min_stock_threshold: min_stock, min_profit_minor: min_profit,
    }).unwrap()]).unwrap()
}

fn cand(id: i64, priority: i64, price: i64, stock: i64) -> Candidate {
    Candidate { id, priority, price_minor: price, shipping_minor: 0, stock, available: true }
}

#[test]
fn primary_wins_when_healthy_regardless_of_input_order() {
    let r = rules(5, 0);
    assert_eq!(select(&[cand(2, 2, 50_000, 50), cand(1, 1, 100_000, 10)], Some(&r), 0), Some(1));
}

#[test]
fn low_stock_switches_to_backup_and_recovery_switches_back() {
    let r = rules(5, 0);
    assert_eq!(select(&[cand(1, 1, 100_000, 4), cand(2, 2, 110_000, 20)], Some(&r), 0), Some(2));
    assert_eq!(select(&[cand(1, 1, 100_000, 5), cand(2, 2, 110_000, 20)], Some(&r), 0), Some(1), "ровно порог — достаточно");
}

#[test]
fn price_spike_switches_to_backup_only_if_it_keeps_the_margin() {
    let r = rules(1, 30_000);
    let published = 189_900; // цена на витрине для закупки 1000 ₽
    // основной подорожал до 1500 ₽: при опубликованной цене прибыль отрицательная, резервный по 1050 ₽ подходит
    assert_eq!(select(&[cand(1, 1, 150_000, 50), cand(2, 2, 105_000, 50)], Some(&r), published), Some(2));
    // резервный тоже слишком дорогой — остаёмся на основном (цена на витрине пересчитается), а не на худшем
    assert_eq!(select(&[cand(1, 1, 150_000, 50), cand(2, 2, 160_000, 50)], Some(&r), published), Some(1));
}

#[test]
fn nobody_in_stock_or_unavailable_means_none() {
    let r = rules(5, 0);
    let mut gone = cand(2, 2, 90_000, 99);
    gone.available = false;
    assert_eq!(select(&[cand(1, 1, 100_000, 0), gone], Some(&r), 0), None);
    assert_eq!(select(&[], Some(&r), 0), None);
}

#[test]
fn works_without_rules_and_with_overflowing_costs() {
    assert_eq!(select(&[cand(1, 1, 100, 1)], None, 500), Some(1));
    let r = rules(1, 0);
    let mut huge = cand(1, 1, i64::MAX, 10);
    huge.shipping_minor = i64::MAX;
    assert_eq!(select(&[huge, cand(2, 2, 100_000, 10)], Some(&r), 189_900), Some(2), "переполнение — не паника, а «не подходит»");
}

async fn setup() -> (tempfile::TempDir, SqlitePool, i64, i64) {
    let dir = tempfile::tempdir().unwrap();
    let pool = crate::db::init_pool(&dir.path().join("f.db")).await.unwrap();
    let (store, _p, variant) = crate::testutil::insert_product(&pool, "local", "f1").await;
    sqlx::query("INSERT INTO pricing_rules(store_id, price_from_minor, price_to_minor, margin_percent, min_profit_minor, min_stock_threshold) VALUES (?, 0, 9000000000, 50.0, 0, 5)").bind(store).execute(&pool).await.unwrap();
    (dir, pool, store, variant)
}

fn backup(price: i64, stock: i64) -> NewSupplier<'static> {
    NewSupplier { priority: 2, url: "https://backup.example/item", sku: "B-1", price_minor: price, shipping_minor: 0, stock }
}

async fn variant_state(pool: &SqlitePool, id: i64) -> (i64, i64, bool, Option<i64>) {
    sqlx::query_as("SELECT supplier_price_minor, supplier_stock, is_in_stock, active_supplier_id FROM product_variants WHERE id = ?").bind(id).fetch_one(pool).await.unwrap()
}

#[tokio::test]
async fn primary_going_out_of_stock_hot_swaps_to_backup_and_back() {
    let (_d, pool, _s, v) = setup().await;
    upsert_backup(&pool, v, &backup(12_000, 30), 10).await.unwrap();
    let (price, stock, in_stock, active) = variant_state(&pool, v).await;
    assert_eq!((price, stock, in_stock), (10_000, 10, true), "основной (остаток 10 ≥ 5) остаётся");
    assert!(active.is_some());

    let mut conn = pool.acquire().await.unwrap();
    apply_observed(&mut conn, None, v, 10_000, 0, 1, 20).await.unwrap(); // у основного остался 1
    drop(conn);
    let (price, stock, in_stock, _) = variant_state(&pool, v).await;
    assert_eq!((price, stock, in_stock), (12_000, 30, true), "витрина переключена на резервного, не обнулена");

    let mut conn = pool.acquire().await.unwrap();
    apply_observed(&mut conn, None, v, 10_000, 0, 40, 30).await.unwrap();
    drop(conn);
    assert_eq!(variant_state(&pool, v).await.0, 10_000, "основной вернулся");
}

#[tokio::test]
async fn everyone_out_of_stock_zeroes_the_showcase_but_keeps_primary_price() {
    let (_d, pool, _s, v) = setup().await;
    upsert_backup(&pool, v, &backup(12_000, 30), 10).await.unwrap();
    record_backup_observation(&pool, 2, None, Some(0), true, 20).await.unwrap();
    let mut conn = pool.acquire().await.unwrap();
    apply_observed(&mut conn, None, v, 10_000, 0, 0, 30).await.unwrap();
    drop(conn);
    let (price, stock, in_stock, active) = variant_state(&pool, v).await;
    assert_eq!((price, stock, in_stock, active), (10_000, 0, false, None));
}

#[tokio::test]
async fn variants_without_backups_use_the_legacy_path_and_removal_restores_it() {
    let (_d, pool, _s, v) = setup().await;
    let mut conn = pool.acquire().await.unwrap();
    assert!(!has_suppliers(&mut conn, v).await.unwrap());
    apply_observed(&mut conn, None, v, 9_000, 0, 20, 5).await.unwrap();
    assert_eq!(refresh_variant(&mut conn, v, 6).await.unwrap(), None);
    drop(conn);
    assert_eq!(variant_state(&pool, v).await.0, 9_000);
    upsert_backup(&pool, v, &backup(8_000, 50), 10).await.unwrap();
    remove_backup(&pool, v, 2, 11).await.unwrap();
    assert_eq!(variant_state(&pool, v).await.0, 9_000, "после удаления резерва снова основной");
}

#[tokio::test]
async fn backup_input_is_validated() {
    let (_d, pool, _s, v) = setup().await;
    for bad in [
        NewSupplier { priority: 1, ..backup(1, 1) },
        NewSupplier { priority: 4, ..backup(1, 1) },
        NewSupplier { url: "javascript:1", ..backup(1, 1) },
        NewSupplier { sku: " ", ..backup(1, 1) },
        NewSupplier { price_minor: 0, ..backup(1, 1) },
        NewSupplier { stock: -1, ..backup(1, 1) },
    ] {
        assert!(upsert_backup(&pool, v, &bad, 1).await.is_err());
    }
    assert!(upsert_backup(&pool, 999, &backup(1, 1), 1).await.is_err());
    assert!(remove_backup(&pool, v, 1, 1).await.is_err());
    let rows: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM variant_suppliers").fetch_one(&pool).await.unwrap();
    assert_eq!(rows, 0, "ошибка не оставляет следов");
}

#[tokio::test]
async fn upserting_the_same_priority_replaces_the_supplier() {
    let (_d, pool, _s, v) = setup().await;
    upsert_backup(&pool, v, &backup(12_000, 30), 10).await.unwrap();
    upsert_backup(&pool, v, &backup(11_000, 40), 11).await.unwrap();
    let (n, price): (i64, i64) = sqlx::query_as("SELECT COUNT(*), MAX(CASE WHEN priority = 2 THEN supplier_price_minor END) FROM variant_suppliers").fetch_one(&pool).await.unwrap();
    assert_eq!((n, price), (2, 11_000));
}
