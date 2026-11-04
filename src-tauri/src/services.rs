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
use crate::{orders, stores, sync};

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
    /// Адрес API релизов (в тестах — локальный сервер).
    pub update_api: String,
}

fn order_id(params: &Value) -> Result<i64, RpcError> {
    params.get("order_id").and_then(Value::as_i64).filter(|id| *id > 0).ok_or_else(|| RpcError::new(INVALID_PARAMS, "нужен параметр order_id"))
}

fn opt_str<'a>(params: &'a Value, key: &str) -> Option<&'a str> {
    params.get(key).and_then(Value::as_str).filter(|s| !s.is_empty())
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
            "UPDATE_CHECK" => {
                let info = crate::update::check(&self.update_api, env!("CARGO_PKG_VERSION")).await?;
                // Тихая замена приложения не делается: can_apply всегда false, расширение подскажет открыть «Настройки → Обновление».
                let mut value = serde_json::to_value(&info).unwrap_or_default();
                if let Some(map) = value.as_object_mut() {
                    map.insert("can_apply".into(), json!(false));
                }
                Ok(value)
            }
            "LIST_ORDERS" => {
                let orders = orders::list_order_views(&self.pool, 200).await?;
                Ok(json!({ "orders": orders }))
            }
            "ORDER_FULFILLMENT_RESULT" => {
                let result = match params.get("status").and_then(Value::as_str) {
                    Some("ordered") => orders::PurchaseResult::Ordered,
                    Some("failed") => orders::PurchaseResult::Failed,
                    Some("needs_manual") => orders::PurchaseResult::NeedsManual,
                    _ => return Err(RpcError::new(INVALID_PARAMS, "status: ordered, failed или needs_manual")),
                };
                let status = orders::record_purchase_result(&self.pool, order_id(params)?, result, opt_str(params, "supplier_order_id"), opt_str(params, "message"), now).await?;
                Ok(json!({ "status": status }))
            }
            "ORDER_TRACKING_UPDATE" => {
                let Some(track) = opt_str(params, "tracking_number") else { return Err(RpcError::new(INVALID_PARAMS, "нужен параметр tracking_number")) };
                let status = orders::record_tracking(&self.pool, order_id(params)?, track, opt_str(params, "carrier"), now).await?;
                Ok(json!({ "status": status }))
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

/// Заказы: передать готовые расширению и отправить полученные трек-номера в магазины.
pub async fn orders_tick(pool: &SqlitePool, vault: &CredentialVault, notifier: &dyn orders::OrderNotifier, now: i64) -> AppResult<usize> {
    let mut handed = 0;
    for _ in 0..MAX_TASKS_PER_TICK {
        match orders::process_next_handoff(pool, notifier, now).await? {
            orders::HandoffOutcome::NoTask => break,
            orders::HandoffOutcome::HandedOff { .. } => handed += 1,
            orders::HandoffOutcome::Stale { .. } => {}
        }
    }
    let waiting: Vec<(i64, i64)> = sqlx::query_as("SELECT id, store_id FROM orders WHERE status = 'SHIPPED' AND tracking_sent_at IS NULL AND tracking_number IS NOT NULL LIMIT 100").fetch_all(pool).await?;
    for (order_id, store_id) in waiting {
        let store: Option<Store> = sqlx::query_as("SELECT * FROM stores WHERE id = ? AND is_active = 1 AND platform_type <> 'local'").bind(store_id).fetch_optional(pool).await?;
        if let Some(Ok(adapter)) = store.map(|s| stores::build_adapter(&s, vault)) {
            orders::push_tracking(pool, order_id, adapter.as_ref(), now).await?;
        }
    }
    Ok(handed)
}

async fn monitor_loop(pool: SqlitePool, vault: Arc<CredentialVault>, bridge: Arc<BridgeState>, cancel: CancellationToken) {
    let fetcher = ThrottledFetcher { inner: HttpFetcher { allow_private: false }, throttle: DomainThrottle::new(SUPPLIER_GAP_MS) };
    while !cancel.is_cancelled() {
        if let Err(e) = monitor_tick(&pool, &vault, &fetcher, now_secs()).await {
            tracing::error!(error = %e, "ошибка цикла мониторинга");
        }
        if let Err(e) = orders_tick(&pool, &vault, bridge.as_ref(), now_secs()).await {
            tracing::error!(error = %e, "ошибка цикла заказов");
        }
        tokio::select! {
            _ = cancel.cancelled() => break,
            _ = tokio::time::sleep(TICK) => {}
        }
    }
}

/// Запускает все службы. Возвращает состояние моста (для окна привязки расширения).
pub async fn start(pool: SqlitePool, vault: Arc<CredentialVault>, cancel: CancellationToken) -> Arc<BridgeState> {
    let extra: Arc<dyn ExtraMethods> = Arc::new(AppRpc { pool: pool.clone(), vault: vault.clone(), update_api: crate::update::GITHUB_API.to_string() });
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

    tokio::spawn(monitor_loop(pool, vault, state.clone(), cancel));
    state
}

/// (Пере)запускает обработчик ИИ-очереди с актуальными настройками. Старый останавливается.
pub async fn restart_ai(state: &crate::AppState) {
    let token = state.shutdown.child_token();
    let previous = state.ai_worker.lock().ok().and_then(|mut slot| slot.replace(token.clone()));
    if let Some(old) = previous {
        old.cancel();
    }
    match crate::ai::store::load_generator(&state.pool, &state.vault).await {
        Ok((generator, language)) => {
            let filter = Arc::new(crate::ai::stopwords::StopWordFilter::new(&[]));
            tokio::spawn(crate::ai::worker::run_ai_worker(state.pool.clone(), generator, filter, language, now_secs, token));
        }
        Err(e) => tracing::warn!(error = %e, "ИИ не настроен: генерация карточек отключена"),
    }
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
        let rpc = AppRpc { pool: pool.clone(), vault: Arc::new(CredentialVault::from_key([2; 32])), update_api: String::new() };
        assert_eq!(rpc.handle("SYNC_SUPPLIER_COOKIES", &json!({"host": "s.example", "cookies": "a=b"}), 5).await.unwrap(), json!({"ok": true}));
        assert!(crate::settings::get(&pool, "supplier_cookies:s.example").await.unwrap().is_some());
        assert_eq!(rpc.handle("SYNC_SUPPLIER_COOKIES", &json!({"host": "s.example"}), 5).await.unwrap_err().code, INVALID_PARAMS);
        assert_eq!(rpc.handle("X", &json!({}), 5).await.unwrap_err().code, METHOD_NOT_FOUND);
    }

    struct Count(Mutex<usize>);
    impl orders::OrderNotifier for Count {
        fn notify(&self, _m: &str, _p: Value) -> usize {
            *self.0.lock().unwrap() += 1;
            1
        }
    }

    #[tokio::test]
    async fn order_rpc_methods_drive_the_whole_flow_and_validate_input() {
        let (_d, pool) = pool().await;
        let (store, _p, _v) = crate::testutil::insert_product(&pool, "ozon", "r1").await;
        sqlx::query("UPDATE product_variants SET external_variant_id = 'OFFER-1'").execute(&pool).await.unwrap();
        sqlx::query("INSERT INTO pricing_rules(store_id, price_from_minor, price_to_minor, margin_percent, min_profit_minor, min_stock_threshold) VALUES (?, 0, 9000000000, 50.0, 1000, 1)").bind(store).execute(&pool).await.unwrap();
        let order = stores::StoreOrder { external_order_id: "E-1".into(), external_variant_id: "OFFER-1".into(), quantity: 1, total_minor: 200_000, buyer_address_json: "{}".into() };
        orders::ingest_orders(&pool, store, &[order], 1).await.unwrap();
        let vault = CredentialVault::from_key([3; 32]);
        let notifier = Count(Mutex::new(0));
        assert_eq!(orders_tick(&pool, &vault, &notifier, 2).await.unwrap(), 1);
        assert_eq!(orders_tick(&pool, &vault, &notifier, 3).await.unwrap(), 0);

        let rpc = AppRpc { pool: pool.clone(), vault: Arc::new(CredentialVault::from_key([3; 32])), update_api: String::new() };
        let listed = rpc.handle("LIST_ORDERS", &json!({}), 4).await.unwrap();
        assert_eq!(listed["orders"][0]["status"], "QUEUED_FOR_EXT");
        assert_eq!((listed["orders"][0]["retail_total_cents"].as_i64(), listed["orders"][0]["sku"].as_str()), (Some(200_000), Some("SKU-1")));
        assert!(listed["orders"][0]["address"].is_object() && listed["orders"][0].get("buyer_address_json").is_none());
        assert_eq!(rpc.handle("ORDER_FULFILLMENT_RESULT", &json!({"order_id": 1, "status": "bogus"}), 5).await.unwrap_err().code, INVALID_PARAMS);
        assert_eq!(rpc.handle("ORDER_FULFILLMENT_RESULT", &json!({"status": "ordered"}), 5).await.unwrap_err().code, INVALID_PARAMS);
        let ok = rpc.handle("ORDER_FULFILLMENT_RESULT", &json!({"order_id": 1, "status": "ordered", "supplier_order_id": "S-1", "message": ""}), 5).await.unwrap();
        assert_eq!(ok["status"], "ORDERED");
        assert_eq!(rpc.handle("ORDER_TRACKING_UPDATE", &json!({"order_id": 1}), 6).await.unwrap_err().code, INVALID_PARAMS);
        assert_eq!(rpc.handle("ORDER_TRACKING_UPDATE", &json!({"order_id": 1, "tracking_number": "RB1", "carrier": ""}), 6).await.unwrap()["status"], "SHIPPED");
        let err = rpc.handle("ORDER_FULFILLMENT_RESULT", &json!({"order_id": 99, "status": "ordered"}), 7).await.unwrap_err();
        assert_eq!(err.code, INVALID_PARAMS, "несуществующий заказ — ошибка пользователя, не сбой");
    }

    #[tokio::test]
    async fn update_check_rpc_reports_release_and_never_offers_silent_apply() {
        let (_d, pool) = pool().await;
        let server = crate::testutil::MockServer::start().await;
        server.reply(&format!("/repos/{}/releases/latest", crate::update::REPO), 200, r#"{"tag_name":"v99.0.0","html_url":"https://github.com/kclazzy/Dropshiptest/releases/tag/v99.0.0"}"#);
        let rpc = AppRpc { pool, vault: Arc::new(CredentialVault::from_key([4; 32])), update_api: server.base_url.clone() };
        let reply = rpc.handle("UPDATE_CHECK", &json!({}), 1).await.unwrap();
        assert_eq!((reply["update_available"].as_bool(), reply["can_apply"].as_bool()), (Some(true), Some(false)));
        assert_eq!(rpc.handle("UPDATE_APPLY", &json!({}), 1).await.unwrap_err().code, METHOD_NOT_FOUND);
    }

    #[tokio::test]
    async fn throttled_fetcher_passes_through_to_inner() {
        let f = ThrottledFetcher { inner: Fixed(Mutex::new(vec![])), throttle: DomainThrottle::new(0) };
        assert_eq!(f.fetch("https://a.example/x", None, None).await.unwrap().status, 200);
    }
}
