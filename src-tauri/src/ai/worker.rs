//! Конвейер «товар -> текст карточки»: берёт задачи `AI_REWRITE` из очереди, вызывает модель и сохраняет результат.
//! Источник работы один — очередь; `products.ai_status` только отражает итог.

use std::collections::BTreeMap;
use std::time::Duration;

use serde_json::json;
use sqlx::SqlitePool;
use tokio_util::sync::CancellationToken;

use super::content::{ContentRequest, Target};
use super::stopwords::StopWordFilter;
use super::ContentGenerator;
use crate::db::{self, FailOutcome, PlatformType, Task, TaskType};
use crate::error::AppResult;

const LOCK_TIMEOUT_SECS: i64 = 600;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AiTaskOutcome {
    NoTask,
    Completed,
    /// Временная ошибка: задача вернётся в очередь позже.
    Retrying,
    /// Окончательная ошибка (`ai_status = ERROR`).
    Failed,
}

/// Значения одного атрибута у разных вариантов склеиваются без повторов: «Black, White».
fn collect_attributes(rows: &[String]) -> Vec<(String, String)> {
    let mut by_name: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for raw in rows {
        let Ok(serde_json::Value::Object(map)) = serde_json::from_str::<serde_json::Value>(raw) else { continue };
        for (name, value) in map {
            let text = value.as_str().map(str::to_string).unwrap_or_else(|| value.to_string());
            if text.is_empty() {
                continue;
            }
            let values = by_name.entry(name).or_default();
            if !values.contains(&text) {
                values.push(text);
            }
        }
    }
    by_name.into_iter().map(|(name, values)| (name, values.join(", "))).collect()
}

/// Единственное место, где неудача превращается в состояние очереди и товара.
async fn record_failure(
    pool: &SqlitePool,
    task: &Task,
    product_id: Option<i64>,
    message: &str,
    retryable: bool,
    now: i64,
    retry_delay: i64,
) -> AppResult<AiTaskOutcome> {
    let outcome = db::fail_task(pool, task.id, message, now, retry_delay, retryable).await?;
    let final_failure = outcome != FailOutcome::WillRetry;
    if let Some(id) = product_id {
        let status = if final_failure { "ERROR" } else { "PENDING" };
        sqlx::query("UPDATE products SET ai_status = ? WHERE id = ?").bind(status).bind(id).execute(pool).await?;
    }
    Ok(if final_failure { AiTaskOutcome::Failed } else { AiTaskOutcome::Retrying })
}

/// Обрабатывает не более одной задачи. Ошибки базы возвращаются как ошибка;
/// ошибки провайдера — обычные исходы (`Retrying`/`Failed`), а не сбой.
pub async fn process_next_ai_task(
    pool: &SqlitePool,
    generator: &dyn ContentGenerator,
    filter: &StopWordFilter,
    language: &str,
    now: i64,
    retry_delay_secs: i64,
) -> AppResult<AiTaskOutcome> {
    let Some(task) = db::dequeue_next_task_of(pool, now, LOCK_TIMEOUT_SECS, &[TaskType::AiRewrite]).await? else {
        return Ok(AiTaskOutcome::NoTask);
    };
    let fail = |product: Option<i64>, message: String, retryable: bool| {
        let task = task.clone();
        async move { record_failure(pool, &task, product, &message, retryable, now, retry_delay_secs).await }
    };

    let product_id = serde_json::from_str::<serde_json::Value>(&task.payload_json)
        .ok()
        .and_then(|v| v.get("product_id").and_then(serde_json::Value::as_i64));
    let Some(product_id) = product_id else {
        return fail(None, "invalid AI_REWRITE payload".into(), false).await;
    };

    let row: Option<(String, Option<String>, PlatformType)> = sqlx::query_as(
        "SELECT p.raw_title, p.raw_description, s.platform_type FROM products p JOIN stores s ON s.id = p.store_id WHERE p.id = ?",
    )
    .bind(product_id)
    .fetch_optional(pool)
    .await?;
    let Some((title, description, platform)) = row else {
        return fail(None, format!("product {product_id} not found"), false).await;
    };
    let attributes: Vec<String> = sqlx::query_scalar("SELECT attributes_json FROM product_variants WHERE product_id = ? ORDER BY id")
        .bind(product_id)
        .fetch_all(pool)
        .await?;

    let request = ContentRequest {
        target: Target::from_platform(platform),
        title,
        description,
        attributes: collect_attributes(&attributes),
        language: language.to_string(),
    };

    let source_text = format!("{} {}", request.title, request.description.as_deref().unwrap_or(""));
    let banned = filter.find(&source_text);
    if !banned.is_empty() {
        return fail(Some(product_id), format!("в описании товара запрещённые слова: {}", banned.join(", ")), false).await;
    }

    sqlx::query("UPDATE products SET ai_status = 'PROCESSING' WHERE id = ?").bind(product_id).execute(pool).await?;
    // Сетевой вызов — вне транзакции: писатель БД не блокируется на время генерации.
    let generated = match generator.generate(&request).await {
        Ok(content) => content,
        Err(error) => return fail(Some(product_id), error.to_string(), error.retryable()).await,
    };

    let generated_text = format!("{} {} {}", generated.seo_title, generated.avito_title, generated.description);
    let banned = filter.find(&generated_text);
    if !banned.is_empty() {
        return fail(Some(product_id), format!("модель употребила запрещённые слова: {}", banned.join(", ")), true).await;
    }

    let extras = json!({"bullet_points": generated.bullet_points, "search_tags": generated.search_tags});
    let attributes_json = serde_json::to_string(&generated.extracted_attributes).unwrap_or_else(|_| "{}".into());
    let mut tx = pool.begin().await?;
    sqlx::query(
        "UPDATE products SET ai_seo_title = ?, ai_avito_title = ?, ai_description = ?, ai_attributes_json = ?, \
         ai_tags_json = ?, ai_status = 'READY' WHERE id = ?",
    )
    .bind(&generated.seo_title)
    .bind(&generated.avito_title)
    .bind(&generated.description)
    .bind(attributes_json)
    .bind(extras.to_string())
    .bind(product_id)
    .execute(&mut *tx)
    .await?;
    sqlx::query("UPDATE task_queue SET status = 'COMPLETED', locked_at = 0 WHERE id = ? AND status = 'IN_PROGRESS'")
        .bind(task.id)
        .execute(&mut *tx)
        .await?;
    tx.commit().await?;
    Ok(AiTaskOutcome::Completed)
}

/// Ставит задачу генерации (идемпотентно: если уже ждёт — вторая не создаётся).
pub async fn enqueue_ai_rewrite(pool: &SqlitePool, product_id: i64, now: i64) -> AppResult<bool> {
    let payload = json!({ "product_id": product_id }).to_string();
    let existing: Option<i64> = sqlx::query_scalar(
        "SELECT id FROM task_queue WHERE task_type = 'AI_REWRITE' AND status IN ('PENDING','IN_PROGRESS') AND payload_json = ?",
    )
    .bind(&payload)
    .fetch_optional(pool)
    .await?;
    if existing.is_some() {
        return Ok(false);
    }
    sqlx::query("UPDATE products SET ai_status = 'PENDING' WHERE id = ?").bind(product_id).execute(pool).await?;
    db::enqueue_task(pool, TaskType::AiRewrite, 5, &payload, now, 3).await?;
    Ok(true)
}

/// Фоновая обработка очереди, пока не отменён токен.
pub async fn run_ai_worker(
    pool: SqlitePool,
    generator: std::sync::Arc<dyn ContentGenerator>,
    filter: std::sync::Arc<StopWordFilter>,
    language: String,
    clock: impl Fn() -> i64 + Send + Sync + 'static,
    cancel: CancellationToken,
) {
    while !cancel.is_cancelled() {
        match process_next_ai_task(&pool, generator.as_ref(), &filter, &language, clock(), 60).await {
            Ok(AiTaskOutcome::NoTask) => {
                tokio::select! {
                    _ = cancel.cancelled() => break,
                    _ = tokio::time::sleep(Duration::from_secs(1)) => {}
                }
            }
            Ok(outcome) => tracing::info!("ai: {outcome:?} via {}", generator.name()),
            Err(error) => {
                tracing::error!("ai worker: {error}");
                tokio::select! {
                    _ = cancel.cancelled() => break,
                    _ = tokio::time::sleep(Duration::from_secs(5)) => {}
                }
            }
        }
    }
}

#[cfg(test)]
mod tests;
