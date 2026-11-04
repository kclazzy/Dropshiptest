use std::collections::VecDeque;
use std::sync::Mutex;

use super::*;
use crate::ai::{AiError, GeneratedContent};
use crate::testutil::insert_product;

struct Fake {
    replies: Mutex<VecDeque<Result<GeneratedContent, AiError>>>,
    seen: Mutex<Vec<ContentRequest>>,
}

impl Fake {
    fn new(replies: Vec<Result<GeneratedContent, AiError>>) -> Self {
        Self { replies: Mutex::new(replies.into()), seen: Mutex::new(vec![]) }
    }
    fn calls(&self) -> usize {
        self.seen.lock().unwrap().len()
    }
}

#[async_trait::async_trait]
impl ContentGenerator for Fake {
    async fn generate(&self, request: &ContentRequest) -> Result<GeneratedContent, AiError> {
        self.seen.lock().unwrap().push(request.clone());
        self.replies.lock().unwrap().pop_front().unwrap_or_else(|| Ok(content("AI title", "AI description")))
    }
    fn name(&self) -> String {
        "fake".into()
    }
}

fn content(title: &str, description: &str) -> GeneratedContent {
    GeneratedContent {
        seo_title: title.into(),
        avito_title: "Avito title".into(),
        description: description.into(),
        bullet_points: vec!["b1".into()],
        extracted_attributes: [("Цвет".to_string(), "Чёрный".to_string())].into(),
        search_tags: vec!["t1".into()],
    }
}

async fn setup() -> (tempfile::TempDir, SqlitePool, i64) {
    let dir = tempfile::tempdir().unwrap();
    let pool = db::init_pool(&dir.path().join("w.db")).await.unwrap();
    let (_, product, _) = insert_product(&pool, "avito", "sha-1").await;
    (dir, pool, product)
}

async fn run(pool: &SqlitePool, g: &Fake, now: i64) -> AiTaskOutcome {
    process_next_ai_task(pool, g, &StopWordFilter::new(&[]), "ru", now, 60).await.unwrap()
}

async fn ai_status(pool: &SqlitePool, product: i64) -> String {
    sqlx::query_scalar("SELECT ai_status FROM products WHERE id = ?").bind(product).fetch_one(pool).await.unwrap()
}

#[tokio::test]
async fn empty_queue_does_not_call_the_model() {
    let (_d, pool, _) = setup().await;
    let g = Fake::new(vec![]);
    assert_eq!(run(&pool, &g, 100).await, AiTaskOutcome::NoTask);
    assert_eq!(g.calls(), 0);
}

#[tokio::test]
async fn generates_and_saves_the_listing_for_the_stores_platform() {
    let (_d, pool, product) = setup().await;
    sqlx::query("INSERT INTO product_variants(product_id, supplier_sku, attributes_json, supplier_price_minor, retail_price_minor, min_price_minor, old_price_minor, supplier_stock, state_hash, updated_at) VALUES (?, 'SKU-2', '{\"color\":\"White\"}', 1, 1, 1, 1, 1, 'h', 0)")
        .bind(product).execute(&pool).await.unwrap();
    assert!(enqueue_ai_rewrite(&pool, product, 0).await.unwrap());
    let g = Fake::new(vec![]);
    assert_eq!(run(&pool, &g, 100).await, AiTaskOutcome::Completed);

    let seen = g.seen.lock().unwrap()[0].clone();
    assert_eq!(seen.target, Target::Avito);
    assert_eq!(seen.attributes, vec![("color".to_string(), "Black, White".to_string())]);
    assert_eq!(seen.description.as_deref(), Some("Bluetooth 5.3"));

    let (seo, avito, desc, attrs, tags, status): (String, String, String, String, String, String) = sqlx::query_as(
        "SELECT ai_seo_title, ai_avito_title, ai_description, ai_attributes_json, ai_tags_json, ai_status FROM products WHERE id = ?",
    ).bind(product).fetch_one(&pool).await.unwrap();
    assert_eq!((seo.as_str(), avito.as_str(), desc.as_str(), status.as_str()), ("AI title", "Avito title", "AI description", "READY"));
    assert!(attrs.contains("Чёрный") && tags.contains("b1") && tags.contains("t1"));
    let task: String = sqlx::query_scalar("SELECT status FROM task_queue").fetch_one(&pool).await.unwrap();
    assert_eq!(task, "COMPLETED");
}

#[tokio::test]
async fn enqueue_is_idempotent_while_a_task_is_waiting() {
    let (_d, pool, product) = setup().await;
    assert!(enqueue_ai_rewrite(&pool, product, 0).await.unwrap());
    assert!(!enqueue_ai_rewrite(&pool, product, 0).await.unwrap());
    let count: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue").fetch_one(&pool).await.unwrap();
    assert_eq!(count, 1);
}

#[tokio::test]
async fn temporary_errors_retry_then_give_up() {
    let (_d, pool, product) = setup().await;
    enqueue_ai_rewrite(&pool, product, 0).await.unwrap();
    sqlx::query("UPDATE task_queue SET max_attempts = 2").execute(&pool).await.unwrap();
    let g = Fake::new(vec![Err(AiError::Transport("down".into())), Err(AiError::Transport("down".into()))]);
    assert_eq!(run(&pool, &g, 100).await, AiTaskOutcome::Retrying);
    assert_eq!(ai_status(&pool, product).await, "PENDING");
    assert_eq!(run(&pool, &g, 159).await, AiTaskOutcome::NoTask);
    assert_eq!(run(&pool, &g, 160).await, AiTaskOutcome::Failed);
    assert_eq!(ai_status(&pool, product).await, "ERROR");
    let log: String = sqlx::query_scalar("SELECT error_log FROM task_queue").fetch_one(&pool).await.unwrap();
    assert!(log.contains("down"));
}

#[tokio::test]
async fn wrong_key_is_final_immediately() {
    let (_d, pool, product) = setup().await;
    enqueue_ai_rewrite(&pool, product, 0).await.unwrap();
    let g = Fake::new(vec![Err(AiError::Auth(401))]);
    assert_eq!(run(&pool, &g, 100).await, AiTaskOutcome::Failed);
    assert_eq!(ai_status(&pool, product).await, "ERROR");
    assert_eq!(g.calls(), 1);
}

#[tokio::test]
async fn stop_words_in_the_source_block_the_model_call() {
    let (_d, pool, product) = setup().await;
    sqlx::query("UPDATE products SET raw_title = 'Часы Rolex реплика'").execute(&pool).await.unwrap();
    enqueue_ai_rewrite(&pool, product, 0).await.unwrap();
    let g = Fake::new(vec![]);
    assert_eq!(run(&pool, &g, 100).await, AiTaskOutcome::Failed);
    assert_eq!(g.calls(), 0);
    let log: String = sqlx::query_scalar("SELECT error_log FROM task_queue").fetch_one(&pool).await.unwrap();
    assert!(log.contains("реплика"));
}

#[tokio::test]
async fn stop_words_in_the_answer_are_not_saved_and_the_model_is_asked_again() {
    let (_d, pool, product) = setup().await;
    enqueue_ai_rewrite(&pool, product, 0).await.unwrap();
    let g = Fake::new(vec![Ok(content("Копия часов", "текст"))]);
    assert_eq!(run(&pool, &g, 100).await, AiTaskOutcome::Retrying);
    let title: Option<String> = sqlx::query_scalar("SELECT ai_seo_title FROM products").fetch_one(&pool).await.unwrap();
    assert!(title.is_none());
    assert_eq!(run(&pool, &g, 200).await, AiTaskOutcome::Completed); // вторая попытка чистая
}

#[tokio::test]
async fn broken_payload_or_missing_product_fail_without_calling_the_model() {
    let (_d, pool, _) = setup().await;
    let g = Fake::new(vec![]);
    db::enqueue_task(&pool, TaskType::AiRewrite, 5, "{}", 0, 3).await.unwrap();
    db::enqueue_task(&pool, TaskType::AiRewrite, 5, r#"{"product_id":9999}"#, 0, 3).await.unwrap();
    assert_eq!(run(&pool, &g, 100).await, AiTaskOutcome::Failed);
    assert_eq!(run(&pool, &g, 100).await, AiTaskOutcome::Failed);
    assert_eq!(g.calls(), 0);
}

#[tokio::test]
async fn other_task_types_are_left_alone() {
    let (_d, pool, _) = setup().await;
    db::enqueue_task(&pool, TaskType::PriceSync, 1, "{}", 0, 3).await.unwrap();
    assert_eq!(run(&pool, &Fake::new(vec![]), 100).await, AiTaskOutcome::NoTask);
    let pending: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue WHERE status = 'PENDING'").fetch_one(&pool).await.unwrap();
    assert_eq!(pending, 1);
}

#[tokio::test]
async fn background_worker_drains_the_queue_and_stops_on_cancel() {
    let (_d, pool, product) = setup().await;
    enqueue_ai_rewrite(&pool, product, 0).await.unwrap();
    let cancel = CancellationToken::new();
    let handle = tokio::spawn(run_ai_worker(
        pool.clone(),
        std::sync::Arc::new(Fake::new(vec![])),
        std::sync::Arc::new(StopWordFilter::new(&[])),
        "ru".into(),
        || 100,
        cancel.clone(),
    ));
    for _ in 0..100 {
        if ai_status(&pool, product).await == "READY" {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    assert_eq!(ai_status(&pool, product).await, "READY");
    cancel.cancel();
    tokio::time::timeout(Duration::from_secs(3), handle).await.unwrap().unwrap();
}
