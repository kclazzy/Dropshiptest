//! Санация каталога (ТЗ 4.6): раз в сутки находит «мёртвые» карточки и решает — оживить или снять.
//! Решение — чистая функция от метрик и правила; применение — отдельный шаг.

use sqlx::SqlitePool;

use crate::error::AppResult;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PruneRule {
    pub min_days_published: i64,
    pub max_views: i64,
    /// Сколько раз пробуем оживить, прежде чем архивировать.
    pub max_reanimations: i64,
}

impl Default for PruneRule {
    fn default() -> Self {
        Self { min_days_published: 30, max_views: 50, max_reanimations: 1 }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Metrics {
    pub days_published: i64,
    pub orders: i64,
    pub views: i64,
    pub reanimation_attempts: i64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Decision {
    Keep,
    /// Пересоздать текст (ИИ) и снизить маржу на 3% — первая попытка.
    Reanimate,
    Archive,
}

pub fn decide(m: &Metrics, rule: &PruneRule) -> Decision {
    let dead = m.days_published > rule.min_days_published && m.orders == 0 && m.views < rule.max_views;
    match (dead, m.reanimation_attempts < rule.max_reanimations) {
        (false, _) => Decision::Keep,
        (true, true) => Decision::Reanimate,
        (true, false) => Decision::Archive,
    }
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct PruneReport {
    pub reanimated: usize,
    pub archived: usize,
}

/// Применяет правило ко всем опубликованным товарам. Идемпотентно в пределах суток: счётчик попыток растёт один раз за проход.
pub async fn run(pool: &SqlitePool, rule: &PruneRule, now: i64) -> AppResult<PruneReport> {
    let rows: Vec<(i64, i64, i64, i64, i64)> = sqlx::query_as(
        "SELECT p.id, p.published_at, COALESCE(a.orders_count, 0), COALESCE(a.views_count, 0), COALESCE(a.reanimation_attempts, 0) \
         FROM products p LEFT JOIN product_analytics a ON a.product_id = p.id WHERE p.published_at > 0 AND p.monitor_status = 'ACTIVE'",
    )
    .fetch_all(pool)
    .await?;
    let mut report = PruneReport::default();
    for (id, published_at, orders, views, attempts) in rows {
        let metrics = Metrics { days_published: (now - published_at) / 86_400, orders, views, reanimation_attempts: attempts };
        match decide(&metrics, rule) {
            Decision::Keep => {}
            Decision::Reanimate => {
                sqlx::query("INSERT INTO product_analytics (product_id, reanimation_attempts) VALUES (?, 1) ON CONFLICT(product_id) DO UPDATE SET reanimation_attempts = reanimation_attempts + 1").bind(id).execute(pool).await?;
                // свежие 30 дней на проверку результата
                sqlx::query("UPDATE products SET published_at = ? WHERE id = ?").bind(now).bind(id).execute(pool).await?;
                crate::ai::worker::enqueue_ai_rewrite(pool, id, now).await?;
                report.reanimated += 1;
            }
            Decision::Archive => {
                sqlx::query("UPDATE products SET monitor_status = 'ARCHIVED' WHERE id = ?").bind(id).execute(pool).await?;
                report.archived += 1;
            }
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    const RULE: PruneRule = PruneRule { min_days_published: 30, max_views: 50, max_reanimations: 1 };
    fn m(days: i64, orders: i64, views: i64, attempts: i64) -> Metrics {
        Metrics { days_published: days, orders, views, reanimation_attempts: attempts }
    }

    #[test]
    fn decisions_follow_the_rule_at_the_boundaries() {
        assert_eq!(decide(&m(31, 0, 49, 0), &RULE), Decision::Reanimate);
        assert_eq!(decide(&m(30, 0, 49, 0), &RULE), Decision::Keep, "ровно 30 дней — ещё рано");
        assert_eq!(decide(&m(31, 0, 50, 0), &RULE), Decision::Keep, "50 просмотров — не мёртвый");
        assert_eq!(decide(&m(90, 1, 0, 0), &RULE), Decision::Keep, "был заказ");
        assert_eq!(decide(&m(31, 0, 10, 1), &RULE), Decision::Archive, "оживляли — не помогло");
    }

    #[tokio::test]
    async fn run_reanimates_once_then_archives_and_ignores_unpublished() {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("pr.db")).await.unwrap();
        let (_s, dead, _v) = crate::testutil::insert_product(&pool, "local", "dead").await;
        let (_s2, fresh, _v2) = crate::testutil::insert_product(&pool, "local", "fresh").await;
        let (_s3, draft, _v3) = crate::testutil::insert_product(&pool, "local", "draft").await;
        let day = 86_400;
        let now = 100 * day;
        sqlx::query("UPDATE products SET published_at = ? WHERE id = ?").bind(now - 40 * day).bind(dead).execute(&pool).await.unwrap();
        sqlx::query("UPDATE products SET published_at = ? WHERE id = ?").bind(now - 5 * day).bind(fresh).execute(&pool).await.unwrap();
        let _ = draft; // published_at = 0 — не опубликован

        assert_eq!(run(&pool, &RULE, now).await.unwrap(), PruneReport { reanimated: 1, archived: 0 });
        let queued: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue WHERE task_type = 'AI_REWRITE'").fetch_one(&pool).await.unwrap();
        assert_eq!(queued, 1);
        assert_eq!(run(&pool, &RULE, now + day).await.unwrap(), PruneReport::default(), "после оживления даём ещё 30 дней");
        // «свежий» к этому дню тоже состарился (45 дней без заказов) — его впервые оживляем, а прошедшего оживление архивируем
        assert_eq!(run(&pool, &RULE, now + 40 * day).await.unwrap(), PruneReport { reanimated: 1, archived: 1 });
        let status: String = sqlx::query_scalar("SELECT monitor_status FROM products WHERE id = ?").bind(dead).fetch_one(&pool).await.unwrap();
        assert_eq!(status, "ARCHIVED");
        let others: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM products WHERE monitor_status = 'ACTIVE'").fetch_one(&pool).await.unwrap();
        assert_eq!(others, 2, "архивирован только мёртвый; «свежий» и черновик остаются ACTIVE");
    }
}
