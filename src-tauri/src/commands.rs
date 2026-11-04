//! Команды Tauri: тонкие обёртки над `ui`, `orders`, `ai::store`. Логика и тесты — в обёрнутых модулях.
//! Ошибки уходят в интерфейс уже понятным русским текстом (`AppError::to_user_friendly_ru`).

use serde::Deserialize;
use serde_json::{json, Value};
use tauri::{AppHandle, State};

use crate::ai::providers::AiSettings;
use crate::error::{AppError, AppResult};
use crate::orders::{self, Event};
use crate::services::now_secs;
use crate::ui::{self, NewStoreInput, ProductFilter, RuleInput};
use crate::{ai, bridge, monitor, update, AppState};

type S<'a> = State<'a, AppState>;

#[tauri::command]
pub async fn list_products(state: S<'_>, filter: ProductFilter, search: String, limit: i64, offset: i64) -> AppResult<Vec<ui::ProductRow>> {
    ui::list_products(&state.pool, filter, &search, limit, offset).await
}

#[tauri::command]
pub async fn product_detail(state: S<'_>, product_id: i64) -> AppResult<Value> {
    ui::product_detail(&state.pool, product_id).await
}

#[tauri::command]
pub async fn update_product_text(state: S<'_>, product_id: i64, title: String, description: String) -> AppResult<()> {
    ui::update_product_text(&state.pool, product_id, &title, &description).await
}

#[tauri::command]
pub async fn retry_ai(state: S<'_>, product_id: i64) -> AppResult<bool> {
    ui::retry_ai(&state.pool, product_id, now_secs()).await
}

#[tauri::command]
pub async fn link_variant(state: S<'_>, variant_id: i64, external_id: String) -> AppResult<()> {
    ui::link_variant(&state.pool, variant_id, &external_id).await
}

#[tauri::command]
pub async fn set_monitoring(state: S<'_>, product_id: i64, active: bool) -> AppResult<()> {
    ui::set_monitoring(&state.pool, product_id, active).await
}

#[tauri::command]
pub async fn delete_product(state: S<'_>, product_id: i64) -> AppResult<()> {
    ui::delete_product(&state.pool, product_id).await
}

#[tauri::command]
pub async fn list_stores(state: S<'_>) -> AppResult<Vec<Value>> {
    ui::list_stores(&state.pool).await
}

#[tauri::command]
pub async fn add_store(state: S<'_>, input: NewStoreInput) -> AppResult<i64> {
    ui::add_store(&state.pool, &state.vault, &input, now_secs()).await
}

#[tauri::command]
pub async fn set_store_active(state: S<'_>, store_id: i64, active: bool) -> AppResult<()> {
    ui::set_store_active(&state.pool, store_id, active).await
}

#[tauri::command]
pub async fn delete_store(state: S<'_>, store_id: i64) -> AppResult<()> {
    ui::delete_store(&state.pool, store_id).await
}

#[tauri::command]
pub async fn list_rules(state: S<'_>, store_id: i64) -> AppResult<Vec<crate::db::PricingRuleRow>> {
    ui::list_rules(&state.pool, store_id).await
}

#[tauri::command]
pub async fn save_rules(state: S<'_>, store_id: i64, rules: Vec<RuleInput>) -> AppResult<usize> {
    ui::save_rules(&state.pool, store_id, &rules, now_secs()).await
}

#[tauri::command]
pub fn standard_rule() -> RuleInput {
    RuleInput::standard()
}

#[tauri::command]
pub async fn list_proxies(state: S<'_>) -> AppResult<Vec<Value>> {
    ui::list_proxies(&state.pool).await
}

#[tauri::command]
pub async fn add_proxy(state: S<'_>, proxy_url: String) -> AppResult<i64> {
    monitor::fetch::add_proxy(&state.pool, proxy_url.trim()).await
}

#[tauri::command]
pub async fn remove_proxy(state: S<'_>, proxy_id: i64) -> AppResult<()> {
    ui::remove_proxy(&state.pool, proxy_id).await
}

#[tauri::command]
pub async fn onboarding_state(state: S<'_>) -> AppResult<Value> {
    ui::onboarding_state(&state.pool).await
}

#[tauri::command]
pub async fn setup_default_catalog(state: S<'_>) -> AppResult<i64> {
    ui::setup_default_catalog(&state.pool, now_secs()).await
}

#[tauri::command]
pub async fn finish_onboarding(state: S<'_>) -> AppResult<()> {
    ui::finish_onboarding(&state.pool, now_secs()).await
}

#[tauri::command]
pub async fn get_ai_settings(state: S<'_>) -> AppResult<Value> {
    let settings = ai::store::load_settings(&state.pool).await?;
    Ok(json!({ "settings": settings, "has_key": ai::store::has_key(&state.pool).await? }))
}

#[tauri::command]
pub async fn save_ai_settings(state: S<'_>, settings: AiSettings, api_key: Option<String>) -> AppResult<()> {
    ai::store::save_settings(&state.pool, &state.vault, &settings, api_key.as_deref(), now_secs()).await?;
    crate::services::restart_ai(&state).await;
    Ok(())
}

// ---- заказы ----

#[tauri::command]
pub async fn list_orders(state: S<'_>) -> AppResult<Vec<Value>> {
    orders::list_order_views(&state.pool, 200).await
}

#[derive(Debug, Deserialize)]
pub struct ManualOrder {
    pub store_id: i64,
    pub variant_id: i64,
    pub external_order_id: String,
    pub quantity: i64,
    pub total_minor: i64,
    pub buyer: Value,
}

#[tauri::command]
pub async fn add_manual_order(state: S<'_>, order: ManualOrder) -> AppResult<orders::IngestReport> {
    let store_order = crate::stores::StoreOrder {
        external_order_id: order.external_order_id,
        external_variant_id: format!("#{}", order.variant_id),
        quantity: order.quantity,
        total_minor: order.total_minor,
        buyer_address_json: order.buyer.to_string(),
    };
    orders::ingest_orders(&state.pool, order.store_id, &[store_order], now_secs()).await
}

#[tauri::command]
pub async fn order_action(state: S<'_>, order_id: i64, action: String) -> AppResult<Value> {
    let event = match action.as_str() {
        "retry" => Event::UserRetry,
        "cancel" => Event::Cancel,
        _ => return Err(AppError::Validation("неизвестное действие".into())),
    };
    let status = orders::apply_event(&state.pool, order_id, event, None, None, now_secs()).await?;
    Ok(json!({ "status": status }))
}

#[tauri::command]
pub async fn order_mark_ordered(state: S<'_>, order_id: i64, supplier_order_id: String) -> AppResult<Value> {
    let id = Some(supplier_order_id.trim()).filter(|s| !s.is_empty());
    let status = orders::record_purchase_result(&state.pool, order_id, orders::PurchaseResult::Ordered, id, None, now_secs()).await?;
    Ok(json!({ "status": status }))
}

#[tauri::command]
pub async fn order_set_tracking(state: S<'_>, order_id: i64, tracking_number: String, carrier: Option<String>) -> AppResult<Value> {
    let status = orders::record_tracking(&state.pool, order_id, &tracking_number, carrier.as_deref(), now_secs()).await?;
    Ok(json!({ "status": status }))
}

// ---- резервные поставщики ----

#[derive(serde::Deserialize)]
pub struct BackupInput {
    pub variant_id: i64,
    pub priority: i64,
    pub url: String,
    pub sku: String,
    pub price_minor: i64,
    pub shipping_minor: i64,
    pub stock: i64,
}

#[tauri::command]
pub async fn add_backup_supplier(state: S<'_>, input: BackupInput) -> AppResult<()> {
    let supplier = crate::failover::NewSupplier { priority: input.priority, url: &input.url, sku: &input.sku, price_minor: input.price_minor, shipping_minor: input.shipping_minor, stock: input.stock };
    crate::failover::upsert_backup(&state.pool, input.variant_id, &supplier, now_secs()).await
}

#[tauri::command]
pub async fn remove_backup_supplier(state: S<'_>, variant_id: i64, priority: i64) -> AppResult<()> {
    crate::failover::remove_backup(&state.pool, variant_id, priority, now_secs()).await
}

// ---- расширение ----

#[tauri::command]
pub fn start_pairing(state: S<'_>) -> String {
    state.bridge.pairing.start(now_secs())
}

#[tauri::command]
pub async fn extension_status(state: S<'_>) -> AppResult<Value> {
    let paired = crate::settings::get(&state.pool, "bridge_token_sha256").await?.is_some();
    Ok(json!({ "paired": paired, "address": bridge::DEFAULT_ADDR }))
}

#[tauri::command]
pub async fn revoke_extension(state: S<'_>) -> AppResult<bool> {
    bridge::pairing::revoke_token(&state.pool).await
}

// ---- обновление ----

#[tauri::command]
pub async fn check_update() -> AppResult<update::UpdateInfo> {
    update::check(update::GITHUB_API, env!("CARGO_PKG_VERSION")).await
}

/// Открывает только страницы нашего репозитория на GitHub.
pub fn release_url_allowed(url: &str) -> bool {
    url.starts_with(&format!("https://github.com/{}/", update::REPO)) || url == format!("https://github.com/{}", update::REPO)
}

#[tauri::command]
pub fn open_release_page(app: AppHandle, url: String) -> AppResult<()> {
    use tauri_plugin_opener::OpenerExt;
    if !release_url_allowed(&url) {
        return Err(AppError::Validation("можно открывать только страницу релизов проекта".into()));
    }
    app.opener().open_url(url, None::<&str>).map_err(|_| AppError::Validation("не удалось открыть браузер".into()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_project_release_pages_may_be_opened() {
        assert!(release_url_allowed("https://github.com/kclazzy/Dropshiptest/releases/tag/v1"));
        assert!(release_url_allowed("https://github.com/kclazzy/Dropshiptest"));
        for bad in ["https://github.com/kclazzy/Dropshiptest-evil/x", "https://github.com.evil.example/kclazzy/Dropshiptest/", "file:///etc/passwd", "javascript:alert(1)", "http://github.com/kclazzy/Dropshiptest/"] {
            assert!(!release_url_allowed(bad), "{bad}");
        }
    }
}
