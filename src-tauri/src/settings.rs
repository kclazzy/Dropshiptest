//! Настройки приложения в таблице `settings` (ключ -> текст). Запись идемпотентна (upsert).

use sqlx::SqlitePool;

use crate::error::AppResult;

pub async fn get(pool: &SqlitePool, key: &str) -> AppResult<Option<String>> {
    Ok(sqlx::query_scalar("SELECT value FROM settings WHERE key = ?").bind(key).fetch_optional(pool).await?)
}

pub async fn set(pool: &SqlitePool, key: &str, value: &str, now: i64) -> AppResult<()> {
    sqlx::query(
        "INSERT INTO settings(key, value, updated_at) VALUES (?, ?, ?) \
         ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
    )
    .bind(key)
    .bind(value)
    .bind(now)
    .execute(pool)
    .await?;
    Ok(())
}

pub async fn delete(pool: &SqlitePool, key: &str) -> AppResult<bool> {
    Ok(sqlx::query("DELETE FROM settings WHERE key = ?").bind(key).execute(pool).await?.rows_affected() == 1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn set_get_overwrite_delete() {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("s.db")).await.unwrap();
        assert_eq!(get(&pool, "a").await.unwrap(), None);
        set(&pool, "a", "1", 10).await.unwrap();
        set(&pool, "a", "2", 20).await.unwrap();
        assert_eq!(get(&pool, "a").await.unwrap().as_deref(), Some("2"));
        assert!(delete(&pool, "a").await.unwrap());
        assert!(!delete(&pool, "a").await.unwrap());
    }
}
