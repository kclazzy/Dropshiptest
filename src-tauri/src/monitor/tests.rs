use std::sync::Mutex;

use async_trait::async_trait;

use super::fetch::{FetchError, Page};
use super::*;

type Seen = (String, Option<String>, Option<String>);

struct FakeFetcher {
    reply: Mutex<Result<Page, FetchError>>,
    seen: Mutex<Vec<Seen>>,
}

impl FakeFetcher {
    fn page(status: u16, body: &str) -> Self {
        Self { reply: Mutex::new(Ok(Page { status, body: body.into() })), seen: Mutex::new(vec![]) }
    }
    fn error(e: FetchError) -> Self {
        Self { reply: Mutex::new(Err(e)), seen: Mutex::new(vec![]) }
    }
}

#[async_trait]
impl PageFetcher for FakeFetcher {
    async fn fetch(&self, url: &str, cookies: Option<&str>, proxy: Option<&str>) -> Result<Page, FetchError> {
        self.seen.lock().unwrap().push((url.into(), cookies.map(str::to_string), proxy.map(str::to_string)));
        self.reply.lock().unwrap().clone()
    }
}

fn ld(price: &str, availability: &str) -> String {
    format!(r#"<script type="application/ld+json">{{"@type":"Product","offers":{{"@type":"Offer","price":"{price}","availability":"{availability}"}}}}</script>"#)
}

async fn setup() -> (tempfile::TempDir, SqlitePool, i64, i64) {
    let dir = tempfile::tempdir().unwrap();
    let pool = crate::db::init_pool(&dir.path().join("m.db")).await.unwrap();
    let (store, product, variant) = crate::testutil::insert_product(&pool, "local", "m1").await;
    sqlx::query("INSERT INTO pricing_rules(store_id, price_from_minor, price_to_minor, margin_percent, min_profit_minor, min_stock_threshold) VALUES (?, 0, 9000000000, 40.0, 0, 2)")
        .bind(store)
        .execute(&pool)
        .await
        .unwrap();
    (dir, pool, product, variant)
}

#[tokio::test]
async fn due_products_are_scheduled_once_and_pushed_forward() {
    let (_d, pool, product, _v) = setup().await;
    assert_eq!(schedule_due(&pool, 1_000).await.unwrap(), 1);
    assert_eq!(schedule_due(&pool, 1_000).await.unwrap(), 0, "ещё не пора");
    sqlx::query("UPDATE products SET next_check_at = 0 WHERE id = ?").bind(product).execute(&pool).await.unwrap();
    assert_eq!(schedule_due(&pool, 1_001).await.unwrap(), 0, "открытая задача не дублируется");
    let tasks: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue WHERE task_type = 'PRICE_SYNC'").fetch_one(&pool).await.unwrap();
    assert_eq!(tasks, 1);
}

#[tokio::test]
async fn paused_products_are_not_scheduled() {
    let (_d, pool, product, _v) = setup().await;
    sqlx::query("UPDATE products SET monitor_status = 'PAUSED' WHERE id = ?").bind(product).execute(&pool).await.unwrap();
    assert_eq!(schedule_due(&pool, 10).await.unwrap(), 0);
}

#[tokio::test]
async fn new_price_is_applied_recalculated_and_recorded() {
    let (_d, pool, product, variant) = setup().await;
    schedule_due(&pool, 10).await.unwrap();
    let fetcher = FakeFetcher::page(200, &ld("150.00", "InStock"));
    let out = process_next_price_sync(&pool, &fetcher, None, 20).await.unwrap();
    assert_eq!(out, SyncOutcome::Updated { product_id: product, changed: 1 });
    let (price, retail, stock, in_stock): (i64, i64, i64, bool) =
        sqlx::query_as("SELECT supplier_price_minor, retail_price_minor, supplier_stock, is_in_stock FROM product_variants WHERE id = ?").bind(variant).fetch_one(&pool).await.unwrap();
    assert_eq!((price, stock, in_stock), (15_000, 99, true));
    assert!(retail > 15_000);
    assert_eq!(catalog::history_len(&pool, variant).await.unwrap(), 1);
    let status: String = sqlx::query_scalar("SELECT status FROM task_queue").fetch_one(&pool).await.unwrap();
    assert_eq!(status, "COMPLETED");
    let checked: i64 = sqlx::query_scalar("SELECT last_checked_at FROM products WHERE id = ?").bind(product).fetch_one(&pool).await.unwrap();
    assert_eq!(checked, 20);
}

#[tokio::test]
async fn unchanged_page_writes_no_history() {
    let (_d, pool, _p, variant) = setup().await;
    for now in [10, 5_000] {
        sqlx::query("UPDATE products SET next_check_at = 0").execute(&pool).await.unwrap();
        schedule_due(&pool, now).await.unwrap();
        process_next_price_sync(&pool, &FakeFetcher::page(200, &ld("150", "InStock")), None, now).await.unwrap();
    }
    assert_eq!(catalog::history_len(&pool, variant).await.unwrap(), 1);
}

#[tokio::test]
async fn out_of_stock_page_zeroes_stock() {
    let (_d, pool, _p, variant) = setup().await;
    schedule_due(&pool, 10).await.unwrap();
    process_next_price_sync(&pool, &FakeFetcher::page(200, &ld("100", "OutOfStock")), None, 20).await.unwrap();
    let in_stock: bool = sqlx::query_scalar("SELECT is_in_stock FROM product_variants WHERE id = ?").bind(variant).fetch_one(&pool).await.unwrap();
    assert!(!in_stock);
}

#[tokio::test]
async fn removed_product_is_archived_and_stock_zeroed() {
    let (_d, pool, product, variant) = setup().await;
    schedule_due(&pool, 10).await.unwrap();
    let out = process_next_price_sync(&pool, &FakeFetcher::page(404, "not found"), None, 20).await.unwrap();
    assert_eq!(out, SyncOutcome::Gone { product_id: product });
    let (stock, status): (i64, String) = sqlx::query_as("SELECT v.supplier_stock, p.monitor_status FROM product_variants v JOIN products p ON p.id = v.product_id WHERE v.id = ?").bind(variant).fetch_one(&pool).await.unwrap();
    assert_eq!((stock, status.as_str()), (0, "ARCHIVED"));
}

#[tokio::test]
async fn blocked_site_defers_without_touching_data() {
    let (_d, pool, _p, variant) = setup().await;
    schedule_due(&pool, 10).await.unwrap();
    let out = process_next_price_sync(&pool, &FakeFetcher::page(429, ""), None, 20).await.unwrap();
    assert!(matches!(out, SyncOutcome::Deferred { .. }));
    let stock: i64 = sqlx::query_scalar("SELECT supplier_stock FROM product_variants WHERE id = ?").bind(variant).fetch_one(&pool).await.unwrap();
    assert_eq!(stock, 10, "блокировка не обнуляет остаток");
    assert!(process_next_price_sync(&pool, &FakeFetcher::page(200, &ld("1", "InStock")), None, 30).await.unwrap() == SyncOutcome::NoTask, "ещё рано повторять");
    let again = process_next_price_sync(&pool, &FakeFetcher::page(200, &ld("100", "InStock")), None, 20 + BLOCKED_RETRY_SECS + 1).await.unwrap();
    assert!(matches!(again, SyncOutcome::Updated { .. }));
}

#[tokio::test]
async fn network_errors_retry_then_fail_without_losing_data() {
    let (_d, pool, _p, variant) = setup().await;
    schedule_due(&pool, 10).await.unwrap();
    let fetcher = FakeFetcher::error(FetchError::Transport("таймаут".into()));
    let mut now = 20;
    for _ in 0..3 {
        process_next_price_sync(&pool, &fetcher, None, now).await.unwrap();
        now += 10_000;
    }
    let status: String = sqlx::query_scalar("SELECT status FROM task_queue").fetch_one(&pool).await.unwrap();
    assert_eq!(status, "FAILED");
    let stock: i64 = sqlx::query_scalar("SELECT supplier_stock FROM product_variants WHERE id = ?").bind(variant).fetch_one(&pool).await.unwrap();
    assert_eq!(stock, 10);
}

#[tokio::test]
async fn page_without_markup_is_reported_not_guessed() {
    let (_d, pool, product, _v) = setup().await;
    schedule_due(&pool, 10).await.unwrap();
    let out = process_next_price_sync(&pool, &FakeFetcher::page(200, "<html>пусто</html>"), None, 20).await.unwrap();
    assert!(matches!(out, SyncOutcome::Unreadable { product_id, .. } if product_id == product));
}

#[tokio::test]
async fn forbidden_address_stops_monitoring_of_that_product() {
    let (_d, pool, product, _v) = setup().await;
    schedule_due(&pool, 10).await.unwrap();
    let out = process_next_price_sync(&pool, &FakeFetcher::error(FetchError::Forbidden("внутренняя сеть".into())), None, 20).await.unwrap();
    assert_eq!(out, SyncOutcome::Forbidden { product_id: product });
    let status: String = sqlx::query_scalar("SELECT monitor_status FROM products WHERE id = ?").bind(product).fetch_one(&pool).await.unwrap();
    assert_eq!(status, "ERROR");
}

#[tokio::test]
async fn multiple_variants_are_matched_by_sku_and_ambiguity_is_refused() {
    let (_d, pool, product, _v) = setup().await;
    sqlx::query("INSERT INTO product_variants(product_id, supplier_sku, attributes_json, supplier_price_minor, retail_price_minor, min_price_minor, old_price_minor, supplier_stock, state_hash, updated_at) VALUES (?, 'SKU-2', '{}', 1, 0, 0, 0, 5, 'h', 0)").bind(product).execute(&pool).await.unwrap();
    schedule_due(&pool, 10).await.unwrap();
    let ambiguous = FakeFetcher::page(200, &ld("100", "InStock"));
    assert!(matches!(process_next_price_sync(&pool, &ambiguous, None, 20).await.unwrap(), SyncOutcome::Unreadable { .. }));

    sqlx::query("UPDATE products SET next_check_at = 0").execute(&pool).await.unwrap();
    schedule_due(&pool, 30).await.unwrap();
    let html = r#"<script type="application/ld+json">{"@type":"Product","offers":[{"@type":"Offer","sku":"SKU-1","price":"100"},{"@type":"Offer","sku":"SKU-2","price":"200","availability":"OutOfStock"}]}</script>"#;
    let out = process_next_price_sync(&pool, &FakeFetcher::page(200, html), None, 40).await.unwrap();
    assert_eq!(out, SyncOutcome::Updated { product_id: product, changed: 2 });
}

#[tokio::test]
async fn cookies_and_proxy_are_passed_to_the_fetcher_and_cookies_are_encrypted_at_rest() {
    let (_d, pool, _p, _v) = setup().await;
    let vault = crate::security::CredentialVault::from_key([7; 32]);
    save_supplier_cookies(&pool, &vault, "Supplier.Example", "sid=abc", 1).await.unwrap();
    let raw = crate::settings::get(&pool, "supplier_cookies:supplier.example").await.unwrap().unwrap();
    assert!(!raw.contains("abc") && !raw.contains("sid"));
    assert!(save_supplier_cookies(&pool, &vault, "bad host!", "a=b", 1).await.is_err());
    assert!(save_supplier_cookies(&pool, &vault, "x.example", "a=b\r\nX: y", 1).await.is_err());
    fetch::add_proxy(&pool, "http://proxy.example:3128").await.unwrap();
    schedule_due(&pool, 10).await.unwrap();
    let fetcher = FakeFetcher::page(200, &ld("100", "InStock"));
    process_next_price_sync(&pool, &fetcher, Some(&vault), 20).await.unwrap();
    let seen = fetcher.seen.lock().unwrap();
    assert_eq!(seen[0], ("https://supplier.example/item".into(), Some("sid=abc".into()), Some("http://proxy.example:3128".into())));
}
