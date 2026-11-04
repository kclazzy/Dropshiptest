//! Подбор категории площадки (ТЗ 4.4): локальный поиск BM25 по кэшу дерева категорий (FTS5), без обращения к ИИ.
//! Кэш наполняется из API площадки; здесь — хранение, нормализация запроса и ранжирование.

use serde::Serialize;
use sqlx::SqlitePool;

use crate::error::{AppError, AppResult};

const MAX_TOKENS: usize = 12;
const MAX_LIMIT: i64 = 20;

#[derive(Debug, Clone, Serialize, sqlx::FromRow, PartialEq)]
pub struct CategoryHit {
    pub category_id: String,
    pub type_id: String,
    pub full_path_name: String,
    pub required_attributes_json: String,
}

#[derive(Debug, Clone)]
pub struct NewCategory<'a> {
    pub category_id: &'a str,
    pub type_id: &'a str,
    pub full_path_name: &'a str,
    pub required_attributes_json: &'a str,
}

/// Идемпотентное обновление кэша (по ключу платформа+категория+тип). Возвращает число записанных строк.
pub async fn upsert_all(pool: &SqlitePool, platform: &str, items: &[NewCategory<'_>], now: i64) -> AppResult<usize> {
    let mut tx = pool.begin().await?;
    for c in items {
        if c.category_id.is_empty() || c.full_path_name.trim().is_empty() {
            return Err(AppError::Validation("у категории должны быть id и название".into()));
        }
        sqlx::query(
            "INSERT INTO marketplace_categories_cache (platform_type, category_id, type_id, full_path_name, required_attributes_json, updated_at) \
             VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(platform_type, category_id, type_id) DO UPDATE SET \
             full_path_name = excluded.full_path_name, required_attributes_json = excluded.required_attributes_json, updated_at = excluded.updated_at",
        )
        .bind(platform).bind(c.category_id).bind(c.type_id).bind(c.full_path_name.trim()).bind(c.required_attributes_json).bind(now)
        .execute(&mut *tx)
        .await?;
    }
    tx.commit().await?;
    Ok(items.len())
}

/// Слова запроса → безопасное выражение MATCH: каждое слово в кавычках, длинные — по префиксу (грубый «стемминг» для русского).
pub fn match_expression(query: &str) -> Option<String> {
    let tokens: Vec<String> = query
        .split(|c: char| !c.is_alphanumeric())
        .filter(|w| w.chars().count() >= 2)
        .take(MAX_TOKENS)
        .map(|w| {
            let w = w.to_lowercase();
            let n = w.chars().count();
            if n >= 6 {
                let stem: String = w.chars().take(n - 2).collect();
                format!("\"{stem}\"*")
            } else {
                format!("\"{w}\"")
            }
        })
        .collect();
    (!tokens.is_empty()).then(|| tokens.join(" OR "))
}

/// Лучшие категории для названия товара (BM25, самые релевантные первыми).
pub async fn best_matches(pool: &SqlitePool, platform: &str, query: &str, limit: i64) -> AppResult<Vec<CategoryHit>> {
    let Some(expr) = match_expression(query) else { return Ok(Vec::new()) };
    let hits = sqlx::query_as::<_, CategoryHit>(
        "SELECT c.category_id, c.type_id, c.full_path_name, c.required_attributes_json \
         FROM marketplace_categories_fts f JOIN marketplace_categories_cache c ON c.id = f.rowid \
         WHERE marketplace_categories_fts MATCH ? AND c.platform_type = ? ORDER BY bm25(marketplace_categories_fts), c.id LIMIT ?",
    )
    .bind(expr).bind(platform).bind(limit.clamp(1, MAX_LIMIT))
    .fetch_all(pool)
    .await?;
    Ok(hits)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cat<'a>(id: &'a str, path: &'a str) -> NewCategory<'a> {
        NewCategory { category_id: id, type_id: "", full_path_name: path, required_attributes_json: "[\"brand\"]" }
    }

    async fn seeded() -> (tempfile::TempDir, SqlitePool) {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("c.db")).await.unwrap();
        let items = [
            cat("1", "Электроника / Аксессуары для телефонов / Чехлы для смартфонов"),
            cat("2", "Электроника / Наушники и гарнитуры / Беспроводные наушники"),
            cat("3", "Дом и сад / Кухня / Кружки и стаканы"),
            cat("4", "Одежда / Мужская / Футболки"),
        ];
        upsert_all(&pool, "ozon", &items, 1).await.unwrap();
        (dir, pool)
    }

    #[test]
    fn query_is_sanitised_into_a_safe_match_expression() {
        assert_eq!(match_expression("Чехол для iPhone 15").unwrap(), "\"чехол\" OR \"для\" OR \"ipho\"* OR \"15\"");
        assert_eq!(match_expression("a ! \" ) NEAR( -").unwrap(), "\"near\"", "знаки отброшены, слово NEAR стало обычным словом в кавычках");
        assert_eq!(match_expression("   "), None);
    }

    #[tokio::test]
    async fn ranks_the_relevant_category_first_and_matches_word_forms() {
        let (_d, pool) = seeded().await;
        let top = best_matches(&pool, "ozon", "Беспроводные наушники Bluetooth TWS", 5).await.unwrap();
        assert_eq!(top[0].category_id, "2");
        let case = best_matches(&pool, "ozon", "чехлы для смартфона", 5).await.unwrap();
        assert_eq!(case[0].category_id, "1", "словоформы «чехлы/смартфона» находят «Чехлы/смартфонов»");
        assert_eq!(top[0].required_attributes_json, "[\"brand\"]");
    }

    #[tokio::test]
    async fn is_scoped_by_platform_and_survives_hostile_input() {
        let (_d, pool) = seeded().await;
        assert!(best_matches(&pool, "avito", "наушники", 5).await.unwrap().is_empty());
        for q in ["\"", "NEAR(", "наушники OR", "*", "a AND b NOT", ""] {
            best_matches(&pool, "ozon", q, 5).await.unwrap_or_else(|e| panic!("{q}: {e}"));
        }
    }

    #[tokio::test]
    async fn upsert_is_idempotent_and_keeps_the_index_in_sync() {
        let (_d, pool) = seeded().await;
        upsert_all(&pool, "ozon", &[cat("3", "Дом и сад / Посуда / Термокружки")], 2).await.unwrap();
        upsert_all(&pool, "ozon", &[cat("3", "Дом и сад / Посуда / Термокружки")], 3).await.unwrap();
        let n: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM marketplace_categories_cache").fetch_one(&pool).await.unwrap();
        assert_eq!(n, 4);
        assert!(best_matches(&pool, "ozon", "кружки", 5).await.unwrap().is_empty(), "старое название ушло из индекса");
        assert_eq!(best_matches(&pool, "ozon", "термокружка", 5).await.unwrap()[0].category_id, "3");
        assert!(upsert_all(&pool, "ozon", &[cat("", "x")], 4).await.is_err());
    }
}
