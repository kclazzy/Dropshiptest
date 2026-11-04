use super::*;
use std::collections::HashSet;
use tempfile::TempDir;

async fn pool() -> (TempDir, SqlitePool) {
    let dir = tempfile::tempdir().unwrap();
    let pool = init_pool(&dir.path().join("test.db")).await.unwrap();
    (dir, pool)
}

async fn enqueue(pool: &SqlitePool, priority: i64, at: i64) -> i64 {
    enqueue_task(pool, TaskType::PriceSync, priority, "{}", at, 3).await.unwrap()
}

#[tokio::test]
async fn pool_uses_wal_and_foreign_keys() {
    let (_dir, pool) = pool().await;
    let mode: String = sqlx::query_scalar("PRAGMA journal_mode").fetch_one(&pool).await.unwrap();
    assert_eq!(mode, "wal");
    let fk: i64 = sqlx::query_scalar("PRAGMA foreign_keys").fetch_one(&pool).await.unwrap();
    assert_eq!(fk, 1);
    let sync: i64 = sqlx::query_scalar("PRAGMA synchronous").fetch_one(&pool).await.unwrap();
    assert_eq!(sync, 1); // NORMAL
    let tables: i64 = sqlx::query_scalar(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('stores','pricing_rules','proxies','products','product_variants','orders','task_queue')",
    )
    .fetch_one(&pool)
    .await
    .unwrap();
    assert_eq!(tables, 7);
}

#[tokio::test]
async fn reopening_the_database_keeps_data_and_does_not_remigrate() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("again.db");
    let first = init_pool(&path).await.unwrap();
    enqueue(&first, 5, 0).await;
    first.close().await;
    let second = init_pool(&path).await.unwrap();
    let count: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue").fetch_one(&second).await.unwrap();
    assert_eq!(count, 1);
}

#[tokio::test]
async fn dequeue_orders_by_priority_then_schedule_then_id() {
    let (_dir, pool) = pool().await;
    let low = enqueue(&pool, 5, 100).await;
    let late_high = enqueue(&pool, 1, 200).await;
    let early_high = enqueue(&pool, 1, 100).await;
    for expected in [early_high, late_high, low] {
        assert_eq!(dequeue_next_task(&pool, 500, 300).await.unwrap().unwrap().id, expected);
    }
    assert!(dequeue_next_task(&pool, 500, 300).await.unwrap().is_none());
}

#[tokio::test]
async fn dequeue_returns_the_task_marked_in_progress() {
    let (_dir, pool) = pool().await;
    let id = enqueue_task(&pool, TaskType::AutoOrder, 2, r#"{"order":42}"#, 100, 4).await.unwrap();
    let task = dequeue_next_task(&pool, 100, 300).await.unwrap().unwrap();
    assert_eq!(task.id, id);
    assert_eq!(task.task_type, TaskType::AutoOrder);
    assert_eq!(task.status, TaskStatus::InProgress);
    assert_eq!((task.priority, task.attempts, task.max_attempts, task.locked_at), (2, 1, 4, 100));
    assert_eq!(task.payload_json, r#"{"order":42}"#);
}

#[tokio::test]
async fn future_tasks_wait_for_their_time() {
    let (_dir, pool) = pool().await;
    enqueue(&pool, 5, 1_000).await;
    assert!(dequeue_next_task(&pool, 999, 300).await.unwrap().is_none());
    assert!(dequeue_next_task(&pool, 1_000, 300).await.unwrap().is_some());
}

#[tokio::test]
async fn enqueue_validates_priority_and_attempts() {
    let (_dir, pool) = pool().await;
    for (priority, attempts) in [(0, 3), (11, 3), (5, 0)] {
        let r = enqueue_task(&pool, TaskType::AiRewrite, priority, "{}", 0, attempts).await;
        assert!(matches!(r, Err(AppError::Validation(_))), "{priority} {attempts}");
    }
    assert!(enqueue_task(&pool, TaskType::AiRewrite, 10, "{}", 0, 1).await.is_ok());
}

#[tokio::test]
async fn complete_is_idempotent() {
    let (_dir, pool) = pool().await;
    let id = enqueue(&pool, 5, 0).await;
    assert!(!complete_task(&pool, id).await.unwrap()); // ещё не взята
    dequeue_next_task(&pool, 10, 300).await.unwrap().unwrap();
    assert!(complete_task(&pool, id).await.unwrap());
    assert!(!complete_task(&pool, id).await.unwrap());
    assert!(dequeue_next_task(&pool, 100_000, 300).await.unwrap().is_none());
}

#[tokio::test]
async fn failure_retries_with_delay_then_exhausts() {
    let (_dir, pool) = pool().await;
    let id = enqueue_task(&pool, TaskType::OzonPublish, 5, "{}", 0, 2).await.unwrap();
    dequeue_next_task(&pool, 100, 300).await.unwrap().unwrap();
    assert_eq!(fail_task(&pool, id, "network down", 100, 60, true).await.unwrap(), FailOutcome::WillRetry);
    assert!(dequeue_next_task(&pool, 159, 300).await.unwrap().is_none());
    let second = dequeue_next_task(&pool, 160, 300).await.unwrap().unwrap();
    assert_eq!(second.attempts, 2);
    assert_eq!(fail_task(&pool, id, "still down", 160, 60, true).await.unwrap(), FailOutcome::Exhausted);
    let (status, log): (String, String) =
        sqlx::query_as("SELECT status, error_log FROM task_queue WHERE id = ?").bind(id).fetch_one(&pool).await.unwrap();
    assert_eq!((status.as_str(), log.as_str()), ("FAILED", "still down"));
    assert!(dequeue_next_task(&pool, 100_000, 300).await.unwrap().is_none());
}

#[tokio::test]
async fn permanent_failure_skips_remaining_attempts() {
    let (_dir, pool) = pool().await;
    let id = enqueue_task(&pool, TaskType::AiRewrite, 5, "{}", 0, 5).await.unwrap();
    dequeue_next_task(&pool, 10, 300).await.unwrap().unwrap();
    assert_eq!(fail_task(&pool, id, "invalid api key", 10, 60, false).await.unwrap(), FailOutcome::Exhausted);
    assert!(dequeue_next_task(&pool, 100_000, 300).await.unwrap().is_none());
}

#[tokio::test]
async fn failing_a_task_that_is_not_running_changes_nothing() {
    let (_dir, pool) = pool().await;
    let id = enqueue(&pool, 5, 0).await;
    assert_eq!(fail_task(&pool, id, "x", 0, 1, true).await.unwrap(), FailOutcome::NotInProgress);
    assert_eq!(fail_task(&pool, 9_999, "x", 0, 1, true).await.unwrap(), FailOutcome::NotInProgress);
}

#[tokio::test]
async fn defer_returns_the_task_later_without_spending_an_attempt() {
    let (_dir, pool) = pool().await;
    let id = enqueue_task(&pool, TaskType::PriceSync, 5, "{}", 0, 1).await.unwrap();
    dequeue_next_task(&pool, 100, 300).await.unwrap().unwrap();
    assert!(defer_task(&pool, id, 500).await.unwrap());
    assert!(dequeue_next_task(&pool, 499, 300).await.unwrap().is_none());
    assert_eq!(dequeue_next_task(&pool, 500, 300).await.unwrap().unwrap().attempts, 1);
    complete_task(&pool, id).await.unwrap();
    assert!(!defer_task(&pool, id, 1).await.unwrap());
}

#[tokio::test]
async fn abandoned_tasks_are_reclaimed_and_eventually_failed() {
    let (_dir, pool) = pool().await;
    let id = enqueue_task(&pool, TaskType::AiRewrite, 5, "{}", 0, 2).await.unwrap();
    assert_eq!(dequeue_next_task(&pool, 1_000, 300).await.unwrap().unwrap().attempts, 1); // воркер «умер»
    assert!(dequeue_next_task(&pool, 1_299, 300).await.unwrap().is_none()); // блокировка действует
    assert_eq!(dequeue_next_task(&pool, 1_300, 300).await.unwrap().unwrap().attempts, 2);
    // Попытки исчерпаны: брошенная задача не берётся снова, а помечается FAILED.
    assert!(dequeue_next_task(&pool, 5_000, 300).await.unwrap().is_none());
    let status: String = sqlx::query_scalar("SELECT status FROM task_queue WHERE id = ?").bind(id).fetch_one(&pool).await.unwrap();
    assert_eq!(status, "FAILED");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn every_task_goes_to_exactly_one_of_many_workers() {
    let (_dir, pool) = pool().await;
    const TASKS: i64 = 200;
    for i in 0..TASKS {
        enqueue(&pool, 1 + i % 10, 0).await;
    }
    let mut workers = Vec::new();
    for _ in 0..8 {
        let pool = pool.clone();
        workers.push(tokio::spawn(async move {
            let mut got = Vec::new();
            while let Some(task) = dequeue_next_task(&pool, 10, 300).await.unwrap() {
                got.push(task.id);
                assert!(complete_task(&pool, task.id).await.unwrap());
            }
            got
        }));
    }
    let mut all = Vec::new();
    for w in workers {
        all.extend(w.await.unwrap());
    }
    assert_eq!(all.len() as i64, TASKS);
    assert_eq!(all.iter().collect::<HashSet<_>>().len() as i64, TASKS);
}

#[tokio::test]
async fn unknown_enum_text_in_the_database_is_an_error_not_a_panic() {
    let (_dir, pool) = pool().await;
    sqlx::query("INSERT INTO task_queue(task_type, payload_json, scheduled_at) VALUES ('NOPE', '{}', 0)")
        .execute(&pool)
        .await
        .unwrap();
    assert!(dequeue_next_task(&pool, 10, 300).await.is_err());
}

#[tokio::test]
async fn pricing_rule_row_converts_to_a_checked_rule() {
    let (_dir, pool) = pool().await;
    sqlx::query("INSERT INTO stores(name, platform_type, api_url, encrypted_credentials, created_at) VALUES ('s','ozon','u',x'00',0)")
        .execute(&pool).await.unwrap();
    sqlx::query("INSERT INTO pricing_rules(store_id, price_from_minor, price_to_minor, margin_percent) VALUES (1, 0, 9000000, 50.0)")
        .execute(&pool).await.unwrap();
    let row: PricingRuleRow = sqlx::query_as("SELECT * FROM pricing_rules").fetch_one(&pool).await.unwrap();
    assert_eq!((row.marketplace_fee_percent, row.min_profit_minor), (15.0, 30_000)); // значения по умолчанию из DDL
    assert!(row.to_rule().is_ok());
    sqlx::query("UPDATE pricing_rules SET marketplace_fee_percent = 99").execute(&pool).await.unwrap();
    let broken: PricingRuleRow = sqlx::query_as("SELECT * FROM pricing_rules").fetch_one(&pool).await.unwrap();
    assert!(broken.to_rule().is_err());
    let store: Store = sqlx::query_as("SELECT * FROM stores").fetch_one(&pool).await.unwrap();
    assert!(!serde_json::to_string(&store).unwrap().contains("encrypted_credentials"));
}
