//! Фоновые службы: мост для расширения, обработка ИИ-очереди, мониторинг поставщика и отправка изменений в магазины.

use std::sync::Arc;
use std::time::Duration;

use async_trait::async_trait;
use serde_json::{json, Value};
use sqlx::SqlitePool;
use tokio_util::sync::CancellationToken;

use crate::bridge::rpc::{RpcError, INVALID_PARAMS, METHOD_NOT_FOUND};
use crate::bridge::{self, BridgeState, ExtraMethods};
use crate::db::Store;
use crate::error::AppResult;
use crate::monitor::fetch::{DomainThrottle, FetchError, HttpFetcher, Page, PageFetcher};
use crate::monitor::{self, SyncOutcome};
use crate::security::CredentialVault;
use crate::{stores, sync};

const TICK: Duration = Duration::from_secs(30);
const MAX_TASKS_PER_TICK: usize = 50;
const SUPPLIER_GAP_MS: i64 = 3_000;

pub fn now_secs() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

/// Методы моста, которым нужен доступ к ключам (сессии поставщиков).
pub struct AppRpc {
    pub pool: SqlitePool,
    pub vault: Arc<CredentialVault>,
}

#[async_trait]
impl ExtraMethods for AppRpc {
    async fn handle(&self, method: &str, params: &Value, now: i64) -> Result<Value, RpcError> {
        match method {
            "SYNC_SUPPLIER_COOKIES" => {
                let host = params.get("host").and_then(Value::as_str);
                let cookies = params.get("cookies").and_then(Value::as_str);
                let (Some(host), Some(cookies)) = (host, cookies) else { return Err(RpcError::new(INVALID_PARAMS, "нужны параметры host и cookies")) };
                monitor::save_supplier_cookies(&self.pool, &self.vault, host, cookies, now).await?;
                Ok(json!({"ok": true}))
            }
            other => Err(RpcError::new(METHOD_NOT_FOUND, format!("метод {other} не поддерживается"))),
        }
    }
}

/// Обёртка загрузчика: выдерживает паузу между запросами к одному сайту.
pub struct ThrottledFetcher<F: PageFetcher> {
    pub inner: F,
    pub throttle: DomainThrottle,
}

#[async_trait]
impl<F: PageFetcher> PageFetcher for ThrottledFetcher<F> {
    async fn fetch(&self, url: &str, cookies: Option<&str>, proxy: Option<&str>) -> Result<Page, FetchError> {
        if let Some(host) = url::Url::parse(url).ok().and_then(|u| u.host_str().map(str::to_string)) {
            let now_ms = now_secs() * 1000;
            let wait = self.throttle.reserve(&host, now_ms);
            if wait > 0 {
                tokio::time::sleep(Duration::from_millis(wait as u64)).await;
            }
        }
        self.inner.fetch(url, cookies, proxy).await
    }
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct TickReport {
    pub scheduled: usize,
    pub processed: usize,
    pub updated: usize,
    pub stores_synced: usize,
}

/// Один проход: расписание → задачи мониторинга → отправка изменений в магазины. Время и загрузчик передаются снаружи.
pub async fn monitor_tick(pool: &SqlitePool, vault: &CredentialVault, fetcher: &dyn PageFetcher, now: i64) -> AppResult<TickReport> {
    let mut report = TickReport { scheduled: monitor::schedule_due(pool, now).await?, ..Default::default() };
    for _ in 0..MAX_TASKS_PER_TICK {
        match monitor::process_next_price_sync(pool, fetcher, Some(vault), now).await? {
            SyncOutcome::NoTask => break,
            SyncOutcome::Updated { changed, .. } => {
                report.processed += 1;
                report.updated += changed;
            }
            _ => report.processed += 1,
        }
    }
    let stores: Vec<Store> = sqlx::query_as("SELECT * FROM stores WHERE is_active = 1 AND platform_type <> 'local'").fetch_all(pool).await?;
    for store in stores {
        match stores::build_adapter(&store, vault) {
            Ok(adapter) => {
                sync::sync_store(pool, store.id, adapter.as_ref()).await?;
                report.stores_synced += 1;
            }
            Err(e) => {
                sqlx::query("UPDATE stores SET last_error = ? WHERE id = ?").bind(e.to_user_friendly_ru()).bind(store.id).execute(pool).await?;
            }
        }
    }
    Ok(report)
}

async fn monitor_loop(pool: SqlitePool, vault: Arc<CredentialVault>, cancel: CancellationToken) {
    let fetcher = ThrottledFetcher { inner: HttpFetcher { allow_private: false }, throttle: DomainThrottle::new(SUPPLIER_GAP_MS) };
    while !cancel.is_cancelled() {
        if let Err(e) = monitor_tick(&pool, &vault, &fetcher, now_secs()).await {
            tracing::error!(error = %e, "ошибка цикла мониторинга");
        }
        tokio::select! {
            _ = cancel.cancelled() => break,
            _ = tokio::time::sleep(TICK) => {}
        }
    }
}

/// Запускает все службы. Возвращает состояние моста (для окна привязки расширения).
pub async fn start(pool: SqlitePool, vault: Arc<CredentialVault>, cancel: CancellationToken) -> Arc<BridgeState> {
    let extra: Arc<dyn ExtraMethods> = Arc::new(AppRpc { pool: pool.clone(), vault: vault.clone() });
    let state = BridgeState::new(pool.clone(), Some(extra));

    match bridge::bind_loopback(bridge::DEFAULT_ADDR).await {
        Ok(listener) => {
            let (state, cancel) = (state.clone(), cancel.clone());
            tokio::spawn(async move {
                if let Err(e) = bridge::serve(listener, state, cancel).await {
                    tracing::error!(error = %e, "мост остановился");
                }
            });
        }
        Err(e) => tracing::error!(error = %e, "не удалось запустить мост (порт 8765 занят?)"),
    }

    match crate::ai::store::load_generator(&pool, &vault).await {
        Ok((generator, language)) => {
            let filter = Arc::new(crate::ai::stopwords::StopWordFilter::new(&[]));
            tokio::spawn(crate::ai::worker::run_ai_worker(pool.clone(), generator, filter, language, now_secs, cancel.clone()));
        }
        Err(e) => tracing::warn!(error = %e, "ИИ не настроен: генерация карточек отключена"),
    }

    tokio::spawn(monitor_loop(pool, vault, cancel));
    state
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use super::*;

    struct Fixed(Mutex<Vec<String>>);

    #[async_trait]
    impl PageFetcher for Fixed {
        async fn fetch(&self, url: &str, _c: Option<&str>, _p: Option<&str>) -> Result<Page, FetchError> {
            self.0.lock().unwrap().push(url.to_string());
            Ok(Page { status: 200, body: r#"<script type="application/ld+json">{"@type":"Product","offers":{"@type":"Offer","price":"250"}}</script>"#.into() })
        }
    }

    async fn pool() -> (tempfile::TempDir, SqlitePool) {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("sv.db")).await.unwrap();
        (dir, pool)
    }

    #[tokio::test]
    async fn tick_schedules_fetches_and_updates_then_goes_quiet() {
        let (_d, pool) = pool().await;
        let (_s, _p, variant) = crate::testutil::insert_product(&pool, "local", "t1").await;
        let vault = CredentialVault::from_key([1; 32]);
        let fetcher = Fixed(Mutex::new(vec![]));
        let first = monitor_tick(&pool, &vault, &fetcher, 100).await.unwrap();
        assert_eq!((first.scheduled, first.processed, first.updated), (1, 1, 1));
        let price: i64 = sqlx::query_scalar("SELECT supplier_price_minor FROM product_variants WHERE id = ?").bind(variant).fetch_one(&pool).await.unwrap();
        assert_eq!(price, 25_000);
        let second = monitor_tick(&pool, &vault, &fetcher, 101).await.unwrap();
        assert_eq!((second.scheduled, second.processed), (0, 0));
        assert_eq!(fetcher.0.lock().unwrap().len(), 1);
    }

    #[tokio::test]
    async fn tick_reports_broken_store_credentials_in_russian_instead_of_failing() {
        let (_d, pool) = pool().await;
        crate::testutil::insert_product(&pool, "ozon", "t2").await; // ключи — мусор x'00'
        let vault = CredentialVault::from_key([1; 32]);
        let report = monitor_tick(&pool, &vault, &Fixed(Mutex::new(vec![])), 100).await.unwrap();
        assert_eq!(report.stores_synced, 0);
        let err: Option<String> = sqlx::query_scalar("SELECT last_error FROM stores WHERE platform_type = 'ozon'").fetch_one(&pool).await.unwrap();
        assert!(err.unwrap().contains("ключ"));
    }

    #[tokio::test]
    async fn bridge_rpc_stores_supplier_cookies_and_rejects_bad_input() {
        let (_d, pool) = pool().await;
        let rpc = AppRpc { pool: pool.clone(), vault: Arc::new(CredentialVault::from_key([2; 32])) };
        assert_eq!(rpc.handle("SYNC_SUPPLIER_COOKIES", &json!({"host": "s.example", "cookies": "a=b"}), 5).await.unwrap(), json!({"ok": true}));
        assert!(crate::settings::get(&pool, "supplier_cookies:s.example").await.unwrap().is_some());
        assert_eq!(rpc.handle("SYNC_SUPPLIER_COOKIES", &json!({"host": "s.example"}), 5).await.unwrap_err().code, INVALID_PARAMS);
        assert_eq!(rpc.handle("X", &json!({}), 5).await.unwrap_err().code, METHOD_NOT_FOUND);
    }

    #[tokio::test]
    async fn throttled_fetcher_passes_through_to_inner() {
        let f = ThrottledFetcher { inner: Fixed(Mutex::new(vec![])), throttle: DomainThrottle::new(0) };
        assert_eq!(f.fetch("https://a.example/x", None, None).await.unwrap().status, 200);
    }
}
