//! Локальная база данных: пул SQLite в режиме WAL, миграции, атомарная очередь задач.
//!
//! Время всегда передаётся параметром (`now`, секунды Unix) — так очередь детерминированно тестируется.

pub mod models;

use std::path::Path;
use std::time::Duration;

use sqlx::sqlite::{SqliteConnectOptions, SqliteJournalMode, SqlitePoolOptions, SqliteSynchronous};
use sqlx::SqlitePool;

use crate::error::{AppError, AppResult};
pub use models::*;

const MAX_CONNECTIONS: u32 = 8;

/// Открывает (или создаёт) базу, включает WAL и накатывает миграции.
pub async fn init_pool(path: &Path) -> AppResult<SqlitePool> {
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        std::fs::create_dir_all(parent)?;
    }
    let options = SqliteConnectOptions::new()
        .filename(path)
        .create_if_missing(true)
        .journal_mode(SqliteJournalMode::Wal)
        .synchronous(SqliteSynchronous::Normal)
        .foreign_keys(true)
        .busy_timeout(Duration::from_secs(5));
    let pool = SqlitePoolOptions::new().max_connections(MAX_CONNECTIONS).connect_with(options).await?;
    sqlx::migrate!("./migrations").run(&pool).await?;
    Ok(pool)
}

// ---------------------------------------------------------------------------------------------
// Очередь задач
// ---------------------------------------------------------------------------------------------

/// Чем закончился вызов `fail_task`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FailOutcome {
    /// Задача вернётся в очередь после паузы.
    WillRetry,
    /// Попытки закончились (или ошибка окончательная) — `FAILED`.
    Exhausted,
    /// Задача не в работе (уже завершена или сброшена): ничего не изменено.
    NotInProgress,
}

/// Ставит задачу в очередь. `priority`: 1 (высший) … 10 (низший).
pub async fn enqueue_task(
    pool: &SqlitePool,
    task_type: TaskType,
    priority: i64,
    payload_json: &str,
    scheduled_at: i64,
    max_attempts: i64,
) -> AppResult<i64> {
    if !(1..=10).contains(&priority) {
        return Err(AppError::Validation("приоритет задачи должен быть от 1 до 10".into()));
    }
    if max_attempts < 1 {
        return Err(AppError::Validation("число попыток должно быть не меньше 1".into()));
    }
    if serde_json::from_str::<serde_json::Value>(payload_json).is_err() {
        return Err(AppError::Validation("данные задачи не являются корректным JSON".into()));
    }
    let id: i64 = sqlx::query_scalar(
        "INSERT INTO task_queue (task_type, priority, payload_json, scheduled_at, max_attempts) \
         VALUES (?, ?, ?, ?, ?) RETURNING id",
    )
    .bind(task_type)
    .bind(priority)
    .bind(payload_json)
    .bind(scheduled_at)
    .bind(max_attempts)
    .fetch_one(pool)
    .await?;
    Ok(id)
}

/// Атомарно забирает самую приоритетную готовую задачу любого типа (см. `dequeue_next_task_of`).
pub async fn dequeue_next_task(pool: &SqlitePool, now: i64, lock_timeout_secs: i64) -> AppResult<Option<Task>> {
    dequeue_next_task_of(pool, now, lock_timeout_secs, &[]).await
}

/// Атомарно забирает самую приоритетную готовую задачу (`priority`, `scheduled_at`, `id`) одним
/// запросом `UPDATE … RETURNING`. Задачи `IN_PROGRESS` с истёкшей блокировкой (`lock_timeout_secs`)
/// берутся повторно, пока остались попытки; исчерпавшие попытки помечаются `FAILED`.
/// Пустой `types` — любой тип; иначе воркер не трогает чужие задачи.
pub async fn dequeue_next_task_of(
    pool: &SqlitePool,
    now: i64,
    lock_timeout_secs: i64,
    types: &[TaskType],
) -> AppResult<Option<Task>> {
    let lock_expired_before = now.saturating_sub(lock_timeout_secs);
    // Номера параметров: ?1 — now, ?2 — граница блокировки, далее типы. Сами значения всегда передаются через bind.
    let type_clause = if types.is_empty() {
        String::new()
    } else {
        let list = (0..types.len()).map(|i| format!("?{}", i + 3)).collect::<Vec<_>>().join(", ");
        format!(" AND task_type IN ({list})")
    };

    let fail_sql = format!(
        "UPDATE task_queue SET status = 'FAILED', locked_at = 0, \
         error_log = COALESCE(error_log, 'worker stopped responding') \
         WHERE status = 'IN_PROGRESS' AND locked_at <= ?2 AND attempts >= max_attempts{type_clause}"
    );
    let mut fail = sqlx::query(&fail_sql).bind(now).bind(lock_expired_before);
    for t in types {
        fail = fail.bind(*t);
    }
    fail.execute(pool).await?;

    // Два запроса вместо одного с OR: с OR SQLite не может читать индекс по порядку и сортирует всю очередь
    // (на 11 000 ожидающих задач — 2,2 мс против ~0,1 мс; профиль, шаг 8). Сначала готовые, потом зависшие.
    let ready = "status = 'PENDING' AND scheduled_at <= ?1";
    let stale = "status = 'IN_PROGRESS' AND locked_at <= ?2 AND attempts < max_attempts";
    for condition in [ready, stale] {
        let take_sql = format!(
            "UPDATE task_queue SET status = 'IN_PROGRESS', attempts = attempts + 1, locked_at = ?1 \
             WHERE id = ( \
                 SELECT id FROM task_queue WHERE {condition}{type_clause} \
                 ORDER BY priority ASC, scheduled_at ASC, id ASC LIMIT 1) \
             RETURNING *"
        );
        let mut take = sqlx::query_as::<_, Task>(&take_sql).bind(now).bind(lock_expired_before);
        for t in types {
            take = take.bind(*t);
        }
        if let Some(task) = take.fetch_optional(pool).await? {
            return Ok(Some(task));
        }
    }
    Ok(None)
}

/// `true`, если именно этот вызов перевёл задачу из `IN_PROGRESS` в `COMPLETED`.
pub async fn complete_task(pool: &SqlitePool, task_id: i64) -> AppResult<bool> {
    let result = sqlx::query(
        "UPDATE task_queue SET status = 'COMPLETED', locked_at = 0 WHERE id = ? AND status = 'IN_PROGRESS'",
    )
    .bind(task_id)
    .execute(pool)
    .await?;
    Ok(result.rows_affected() == 1)
}

/// Возвращает захваченную задачу в очередь до `until`, не расходуя попытку
/// (например, сработал лимит запросов). `true` — задача была в работе.
pub async fn defer_task(pool: &SqlitePool, task_id: i64, until: i64) -> AppResult<bool> {
    let result = sqlx::query(
        "UPDATE task_queue SET status = 'PENDING', locked_at = 0, scheduled_at = ?, \
         attempts = CASE WHEN attempts > 0 THEN attempts - 1 ELSE 0 END \
         WHERE id = ? AND status = 'IN_PROGRESS'",
    )
    .bind(until)
    .bind(task_id)
    .execute(pool)
    .await?;
    Ok(result.rows_affected() == 1)
}

/// Фиксирует неудачу. `allow_retry = false` — ошибка окончательная (неверный ключ, битые данные).
pub async fn fail_task(
    pool: &SqlitePool,
    task_id: i64,
    error_message: &str,
    now: i64,
    retry_delay_secs: i64,
    allow_retry: bool,
) -> AppResult<FailOutcome> {
    let status: Option<TaskStatus> = sqlx::query_scalar(
        "UPDATE task_queue SET error_log = ?1, locked_at = 0, \
           status = CASE WHEN ?2 = 1 AND attempts < max_attempts THEN 'PENDING' ELSE 'FAILED' END, \
           scheduled_at = CASE WHEN ?2 = 1 AND attempts < max_attempts THEN ?3 + ?4 ELSE scheduled_at END \
         WHERE id = ?5 AND status = 'IN_PROGRESS' RETURNING status",
    )
    .bind(error_message)
    .bind(allow_retry)
    .bind(now)
    .bind(retry_delay_secs.max(0))
    .bind(task_id)
    .fetch_optional(pool)
    .await?;
    Ok(match status {
        Some(TaskStatus::Pending) => FailOutcome::WillRetry,
        Some(_) => FailOutcome::Exhausted,
        None => FailOutcome::NotInProgress,
    })
}

#[cfg(test)]
mod queue_tests;
