//! Отправка изменений на витрины. Критерий «что слать» — хэш состояния: отправляем только то, что
//! отличается от уже опубликованного (`published_hash`) и не отклонялось площадкой в этом же виде (`rejected_hash`).
//! Повторный запуск без изменений не делает ни одного запроса.

use sqlx::SqlitePool;

use crate::catalog;
use crate::error::AppResult;
use crate::pricing::stock_to_publish;
use crate::stores::{StoreAdapter, StoreErrorKind, VariantUpdate};

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SyncReport {
    pub sent: usize,
    pub accepted: usize,
    pub rejected: usize,
    /// Ошибка всего обмена (ключи, сеть) — показывается пользователю по-русски.
    pub store_error: Option<String>,
}

struct Pending {
    update: VariantUpdate,
    hash: String,
}

#[derive(sqlx::FromRow)]
struct Row {
    id: i64,
    external_id: String,
    retail: i64,
    min: i64,
    old: i64,
    stock: i64,
    supplier_price: i64,
    in_stock: bool,
    hash: String,
}

async fn pending_updates(pool: &SqlitePool, store_id: i64) -> AppResult<Vec<Pending>> {
    let rows: Vec<Row> = sqlx::query_as(
        "SELECT v.id AS id, COALESCE(v.external_variant_id, '') AS external_id, v.retail_price_minor AS retail, v.min_price_minor AS min, \
                v.old_price_minor AS old, v.supplier_stock AS stock, v.supplier_price_minor AS supplier_price, v.is_in_stock AS in_stock, v.state_hash AS hash \
         FROM product_variants v JOIN products p ON p.id = v.product_id \
         WHERE p.store_id = ? AND p.monitor_status IN ('ACTIVE','ARCHIVED') AND v.external_variant_id IS NOT NULL AND v.external_variant_id <> '' \
           AND v.state_hash <> COALESCE(v.published_hash, '') AND v.state_hash <> COALESCE(v.rejected_hash, '') \
         ORDER BY v.id LIMIT 1000",
    )
    .bind(store_id)
    .fetch_all(pool)
    .await?;
    let mut conn = pool.acquire().await?;
    let rules = catalog::load_rule_set(&mut conn, store_id).await?;
    Ok(rows
        .into_iter()
        .map(|Row { id: variant_id, external_id, retail, min, old, stock, supplier_price, in_stock, hash }| {
            let published_stock = match rules.as_ref().and_then(|r| r.find(supplier_price)) {
                Some(rule) => stock_to_publish(rule, stock, in_stock),
                None if in_stock => stock.max(0),
                None => 0,
            };
            Pending { update: VariantUpdate { variant_id, external_id, retail_minor: retail, min_price_minor: min, old_price_minor: old, stock: published_stock }, hash }
        })
        .collect())
}

async fn set_last_error(pool: &SqlitePool, store_id: i64, error: Option<&str>) -> AppResult<()> {
    sqlx::query("UPDATE stores SET last_error = ? WHERE id = ?").bind(error).bind(store_id).execute(pool).await?;
    Ok(())
}

pub async fn sync_store(pool: &SqlitePool, store_id: i64, adapter: &dyn StoreAdapter) -> AppResult<SyncReport> {
    let pending = pending_updates(pool, store_id).await?;
    if pending.is_empty() {
        return Ok(SyncReport::default());
    }
    let updates: Vec<VariantUpdate> = pending.iter().map(|p| p.update.clone()).collect();
    let results = match adapter.update_variants(&updates).await {
        Ok(r) => r,
        Err(e) => {
            let message = e.to_user_friendly_ru();
            set_last_error(pool, store_id, Some(&message)).await?;
            // Отклонение всего запроса площадкой — не повторять то же самое; сетевые сбои остаются в очереди.
            if e.kind == StoreErrorKind::Rejected {
                for p in &pending {
                    sqlx::query("UPDATE product_variants SET rejected_hash = ? WHERE id = ?").bind(&p.hash).bind(p.update.variant_id).execute(pool).await?;
                }
            }
            return Ok(SyncReport { sent: updates.len(), store_error: Some(message), ..Default::default() });
        }
    };
    let mut report = SyncReport { sent: updates.len(), ..Default::default() };
    let mut last_item_error = None;
    for p in &pending {
        match results.iter().find(|r| r.variant_id == p.update.variant_id) {
            Some(r) if r.ok => {
                sqlx::query("UPDATE product_variants SET published_hash = ?, rejected_hash = NULL WHERE id = ?").bind(&p.hash).bind(p.update.variant_id).execute(pool).await?;
                report.accepted += 1;
            }
            Some(r) => {
                sqlx::query("UPDATE product_variants SET rejected_hash = ? WHERE id = ?").bind(&p.hash).bind(p.update.variant_id).execute(pool).await?;
                last_item_error = Some(r.error.clone());
                report.rejected += 1;
            }
            None => {} // площадка не вернула ответ по варианту — останется в очереди
        }
    }
    set_last_error(pool, store_id, last_item_error.as_deref()).await?;
    Ok(report)
}

/// Кнопка «Повторить отклонённые»: снимает отметки отклонения, следующая синхронизация отправит их снова.
pub async fn retry_rejected(pool: &SqlitePool, store_id: i64) -> AppResult<u64> {
    let done = sqlx::query("UPDATE product_variants SET rejected_hash = NULL WHERE rejected_hash IS NOT NULL AND product_id IN (SELECT id FROM products WHERE store_id = ?)")
        .bind(store_id)
        .execute(pool)
        .await?;
    Ok(done.rows_affected())
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use async_trait::async_trait;

    use super::*;
    use crate::stores::{ItemResult, StoreError, StoreResult};

    struct Fake {
        calls: Mutex<Vec<Vec<VariantUpdate>>>,
        reply: Mutex<Result<bool, StoreErrorKind>>,
    }

    impl Fake {
        fn new(reply: Result<bool, StoreErrorKind>) -> Self {
            Self { calls: Mutex::new(vec![]), reply: Mutex::new(reply) }
        }
    }

    #[async_trait]
    impl StoreAdapter for Fake {
        fn name(&self) -> String {
            "Fake".into()
        }
        async fn update_variants(&self, updates: &[VariantUpdate]) -> StoreResult<Vec<ItemResult>> {
            self.calls.lock().unwrap().push(updates.to_vec());
            match *self.reply.lock().unwrap() {
                Ok(ok) => Ok(updates.iter().map(|u| ItemResult { variant_id: u.variant_id, ok, price_sent: ok, stock_sent: ok, error: if ok { String::new() } else { "цена ниже допустимой".into() } }).collect()),
                Err(kind) => Err(StoreError::new(kind, "сбой")),
            }
        }
    }

    async fn setup() -> (tempfile::TempDir, SqlitePool, i64, i64) {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("s.db")).await.unwrap();
        let (store, _p, variant) = crate::testutil::insert_product(&pool, "ozon", "s1").await;
        sqlx::query("UPDATE product_variants SET external_variant_id = 'OFFER-1', state_hash = 'h1' WHERE id = ?").bind(variant).execute(&pool).await.unwrap();
        (dir, pool, store, variant)
    }

    #[tokio::test]
    async fn accepted_update_is_not_sent_again_until_state_changes() {
        let (_d, pool, store, variant) = setup().await;
        let fake = Fake::new(Ok(true));
        let first = sync_store(&pool, store, &fake).await.unwrap();
        assert_eq!((first.sent, first.accepted), (1, 1));
        assert_eq!(fake.calls.lock().unwrap()[0][0].external_id, "OFFER-1");
        assert_eq!(sync_store(&pool, store, &fake).await.unwrap(), SyncReport::default());
        assert_eq!(fake.calls.lock().unwrap().len(), 1, "второй запуск не обращается к площадке");
        sqlx::query("UPDATE product_variants SET state_hash = 'h2' WHERE id = ?").bind(variant).execute(&pool).await.unwrap();
        assert_eq!(sync_store(&pool, store, &fake).await.unwrap().accepted, 1);
        assert_eq!(fake.calls.lock().unwrap().len(), 2);
    }

    #[tokio::test]
    async fn rejected_state_is_not_hammered_and_can_be_retried() {
        let (_d, pool, store, _v) = setup().await;
        let fake = Fake::new(Ok(false));
        let r = sync_store(&pool, store, &fake).await.unwrap();
        assert_eq!((r.rejected, r.accepted), (1, 0));
        let err: Option<String> = sqlx::query_scalar("SELECT last_error FROM stores WHERE id = ?").bind(store).fetch_one(&pool).await.unwrap();
        assert_eq!(err.as_deref(), Some("цена ниже допустимой"));
        sync_store(&pool, store, &fake).await.unwrap();
        assert_eq!(fake.calls.lock().unwrap().len(), 1, "отклонённое не шлём повторно");
        assert_eq!(retry_rejected(&pool, store).await.unwrap(), 1);
        *fake.reply.lock().unwrap() = Ok(true);
        assert_eq!(sync_store(&pool, store, &fake).await.unwrap().accepted, 1);
        let err: Option<String> = sqlx::query_scalar("SELECT last_error FROM stores WHERE id = ?").bind(store).fetch_one(&pool).await.unwrap();
        assert_eq!(err, None, "успех очищает ошибку");
    }

    #[tokio::test]
    async fn network_failure_keeps_changes_pending_and_reports_in_russian() {
        let (_d, pool, store, _v) = setup().await;
        let fake = Fake::new(Err(StoreErrorKind::Transport));
        let r = sync_store(&pool, store, &fake).await.unwrap();
        assert!(r.store_error.is_some());
        *fake.reply.lock().unwrap() = Ok(true);
        assert_eq!(sync_store(&pool, store, &fake).await.unwrap().accepted, 1, "сетевой сбой не теряет изменения");
    }

    #[tokio::test]
    async fn unlinked_and_archived_handling() {
        let (_d, pool, store, variant) = setup().await;
        sqlx::query("UPDATE product_variants SET external_variant_id = NULL WHERE id = ?").bind(variant).execute(&pool).await.unwrap();
        let fake = Fake::new(Ok(true));
        assert_eq!(sync_store(&pool, store, &fake).await.unwrap(), SyncReport::default());
        assert!(fake.calls.lock().unwrap().is_empty(), "несвязанные варианты не отправляются");
        sqlx::query("UPDATE product_variants SET external_variant_id = 'O', supplier_stock = 0, is_in_stock = 0 WHERE id = ?").bind(variant).execute(&pool).await.unwrap();
        sync_store(&pool, store, &fake).await.unwrap();
        assert_eq!(fake.calls.lock().unwrap()[0][0].stock, 0, "нет в наличии — публикуем остаток 0");
    }
}
