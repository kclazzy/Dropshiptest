//! Профилирование горячих путей на 10 000 товаров. Запуск (в release, чтобы цифры были честными):
//! `cargo test --release --lib bench:: -- --ignored --nocapture`

use std::time::Instant;

use async_trait::async_trait;
use serde_json::json;
use sqlx::SqlitePool;

use crate::monitor::fetch::{FetchError, Page, PageFetcher};
use crate::ui::{list_products, ProductFilter};

const PRODUCTS: usize = 10_000;

struct Instant200;

#[async_trait]
impl PageFetcher for Instant200 {
    async fn fetch(&self, _url: &str, _c: Option<&str>, _p: Option<&str>) -> Result<Page, FetchError> {
        let body = r#"<script type="application/ld+json">{"@type":"Product","offers":{"@type":"Offer","price":"11.50","availability":"InStock"}}</script>"#;
        Ok(Page { status: 200, body: body.into() })
    }
}

fn time<T>(label: &str, started: Instant, out: T) -> T {
    println!("BENCH {label:<46} {:>9.1} ms", started.elapsed().as_secs_f64() * 1000.0);
    out
}

async fn plan(pool: &SqlitePool, label: &str, sql: &str) {
    let rows: Vec<(i64, i64, i64, String)> = sqlx::query_as(&format!("EXPLAIN QUERY PLAN {sql}")).fetch_all(pool).await.unwrap();
    for (_, _, _, detail) in rows {
        println!("PLAN  {label:<24} {detail}");
    }
}

#[tokio::test]
#[ignore]
async fn profile_hot_paths() {
    let dir = tempfile::tempdir().unwrap();
    let pool = crate::db::init_pool(&dir.path().join("bench.db")).await.unwrap();
    let store = crate::catalog::ensure_default_store(&pool, 1).await.unwrap();
    sqlx::query("INSERT INTO pricing_rules(store_id, price_from_minor, price_to_minor, margin_percent, min_profit_minor, min_stock_threshold) VALUES (?, 0, 9000000000, 40.0, 0, 2)")
        .bind(store).execute(&pool).await.unwrap();

    let t = Instant::now();
    for i in 0..PRODUCTS {
        let payload = json!({
            "url": format!("https://shop.example.com/item/{i}"), "title": format!("Беспроводные наушники модель {i}"),
            "description": "Описание товара ".repeat(40), "images": ["https://img.example.com/a.jpg"],
            "variants": [
                {"sku": format!("s{i}-1"), "price_cents": 1000 + i as i64, "shipping_cents": 100, "stock": 10},
                {"sku": format!("s{i}-2"), "price_cents": 1200 + i as i64, "shipping_cents": 100, "stock": 5},
                {"sku": format!("s{i}-3"), "price_cents": 1500 + i as i64, "shipping_cents": 100, "stock": 0}]
        });
        let payload = crate::import::parse_import_payload(&payload).unwrap();
        crate::import::import_product(&pool, &payload, 1_000).await.unwrap();
    }
    time(&format!("import {PRODUCTS} products x3 variants (1 by 1)"), t, ());

    let t = Instant::now();
    let _ = list_products(&pool, ProductFilter::All, "", 50, 0).await.unwrap();
    time("list_products All, page 1", t, ());
    let t = Instant::now();
    let _ = list_products(&pool, ProductFilter::All, "", 50, 9_000).await.unwrap();
    time("list_products All, offset 9000", t, ());
    let t = Instant::now();
    let _ = list_products(&pool, ProductFilter::Drafts, "", 50, 0).await.unwrap();
    time("list_products Drafts (HAVING)", t, ());
    let t = Instant::now();
    let r = list_products(&pool, ProductFilter::All, "НАУШНИКИ модель 77", 50, 0).await.unwrap();
    time(&format!("list_products search ({} hits)", r.len()), t, ());

    let t = Instant::now();
    let n = crate::monitor::schedule_due(&pool, 10_000_000).await.unwrap();
    time(&format!("schedule_due (500 of {PRODUCTS} due) -> {n}"), t, ());

    let t = Instant::now();
    for _ in 0..500 {
        crate::monitor::process_next_price_sync(&pool, &Instant200, None, 10_000_000).await.unwrap();
    }
    time("process_next_price_sync x500 (instant fetch)", t, ());

    let t = Instant::now();
    let changed = crate::catalog::recalculate_store(&pool, store, 20_000_000).await.unwrap();
    time(&format!("recalculate_store ({} variants changed)", changed), t, ());

    let t = Instant::now();
    for i in 0..1000 {
        crate::db::enqueue_task(&pool, crate::db::TaskType::AiRewrite, 5, &format!("{{\"product_id\":{i}}}"), 0, 3).await.unwrap();
    }
    time("enqueue_task x1000", t, ());
    let t = Instant::now();
    for _ in 0..1000 {
        crate::db::dequeue_next_task_of(&pool, 30_000_000, 600, &[crate::db::TaskType::AiRewrite]).await.unwrap();
    }
    time("dequeue_next_task_of(AI_REWRITE) x1000", t, ());

    plan(&pool, "list_products", "SELECT p.id, COUNT(v.id) FROM products p LEFT JOIN product_variants v ON v.product_id = p.id GROUP BY p.id ORDER BY p.id DESC LIMIT 50 OFFSET 0").await;
    plan(&pool, "schedule_due", "SELECT id, check_interval_sec FROM products WHERE monitor_status = 'ACTIVE' AND next_check_at <= 5 ORDER BY next_check_at LIMIT 500").await;
    plan(&pool, "open-task dedupe", "SELECT id FROM task_queue WHERE task_type = 'PRICE_SYNC' AND status IN ('PENDING','IN_PROGRESS') AND payload_json = '{}'").await;
    plan(&pool, "dequeue", "SELECT id FROM task_queue WHERE status = 'PENDING' AND scheduled_at <= 5 AND task_type IN ('PRICE_SYNC') ORDER BY priority, scheduled_at, id LIMIT 1").await;
    plan(&pool, "variant by sku", "SELECT id FROM product_variants WHERE product_id = 5 AND supplier_sku = 'x'").await;
    plan(&pool, "recalc join", "SELECT v.id FROM product_variants v JOIN products p ON p.id = v.product_id WHERE p.store_id = 1").await;
    plan(&pool, "orders poll", "SELECT id FROM orders WHERE status = 'NEW' ORDER BY id LIMIT 1").await;
}
