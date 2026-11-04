//! Dropship: ядро локальной системы дропшиппинга. GUI — Tauri v2, данные — SQLite (WAL).

pub mod ai;
pub mod bridge;
pub mod catalog;
pub mod commands;
pub mod db;
pub mod error;
pub mod failover;
pub mod fx;
pub mod import;
pub mod monitor;
pub mod orders;
pub mod pricing;
pub mod pruner;
pub mod security;
pub mod services;
pub mod settings;
pub mod stores;
pub mod tracking;
pub mod tray;
pub mod ui;
pub mod update;
pub mod sync;
#[cfg(test)]
pub mod testutil;

use serde::Deserialize;
use sqlx::SqlitePool;
use tauri::Manager;

use crate::error::AppError;
use crate::pricing::{PriceBreakdown, PriceInput, PricingRule, RuleSpec};

/// Состояние приложения, доступное командам интерфейса.
pub struct AppState {
    pub pool: SqlitePool,
    pub vault: std::sync::Arc<security::CredentialVault>,
    pub bridge: std::sync::Arc<bridge::BridgeState>,
    pub shutdown: tokio_util::sync::CancellationToken,
    /// Токен отмены текущего ИИ-обработчика: при смене настроек ИИ он перезапускается.
    pub ai_worker: std::sync::Mutex<Option<tokio_util::sync::CancellationToken>>,
}

/// Параметры визуального калькулятора прибыли (суммы в копейках, проценты как на ползунках).
#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BreakdownRequest {
    pub supplier_price_minor: i64,
    pub logistics_minor: i64,
    pub packaging_minor: i64,
    pub margin_percent: f64,
    pub marketplace_fee_percent: f64,
    pub acquiring_and_tax_percent: f64,
    pub discount_striken_percent: f64,
    pub rounding_minor: i64,
    pub min_profit_minor: i64,
}

/// Чистая функция команды: проще тестировать, чем сам обработчик Tauri.
pub fn breakdown_for(request: &BreakdownRequest) -> Result<PriceBreakdown, AppError> {
    let rule = PricingRule::new(RuleSpec {
        price_from_minor: 0,
        price_to_minor: i64::MAX,
        margin_percent: request.margin_percent,
        fixed_markup_minor: 0,
        marketplace_fee_percent: request.marketplace_fee_percent,
        acquiring_and_tax_percent: request.acquiring_and_tax_percent,
        discount_striken_percent: request.discount_striken_percent,
        rounding_minor: request.rounding_minor,
        min_stock_threshold: 0,
        min_profit_minor: request.min_profit_minor,
    })?;
    let input = PriceInput {
        supplier_price_minor: request.supplier_price_minor,
        logistics_minor: request.logistics_minor,
        packaging_minor: request.packaging_minor,
    };
    Ok(pricing::calculate(&rule, &input)?)
}

#[tauri::command]
fn price_breakdown(request: BreakdownRequest) -> Result<PriceBreakdown, AppError> {
    breakdown_for(&request).inspect_err(|e| tracing::warn!("price_breakdown: {e}"))
}

/// Запускает приложение. Ошибки старта возвращаются вызывающему, а не паникуют.
pub fn run() -> Result<(), Box<dyn std::error::Error>> {
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()))
        .init();

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let data_dir = app.path().app_data_dir()?;
            let pool = tauri::async_runtime::block_on(db::init_pool(&data_dir.join("dropship.db")))?;
            tracing::info!("database ready in {}", data_dir.display());
            let vault = std::sync::Arc::new(security::CredentialVault::open_or_create(&data_dir.join("secret.key"))?);
            let shutdown = tokio_util::sync::CancellationToken::new();
            let bridge = tauri::async_runtime::block_on(services::start(pool.clone(), vault.clone(), shutdown.clone()));
            let ai_worker = std::sync::Mutex::new(None);
            app.manage(AppState { pool, vault, bridge, shutdown, ai_worker });
            let state = app.state::<AppState>();
            tauri::async_runtime::block_on(services::restart_ai(&state));
            tray::install(app)?;
            Ok(())
        })
        .on_window_event(tray::keep_in_tray)
        .invoke_handler(tauri::generate_handler![
            price_breakdown,
            commands::list_products,
            commands::product_detail,
            commands::update_product_text,
            commands::retry_ai,
            commands::link_variant,
            commands::set_monitoring,
            commands::delete_product,
            commands::list_stores,
            commands::add_store,
            commands::set_store_active,
            commands::delete_store,
            commands::list_rules,
            commands::save_rules,
            commands::standard_rule,
            commands::list_proxies,
            commands::add_proxy,
            commands::remove_proxy,
            commands::onboarding_state,
            commands::setup_default_catalog,
            commands::finish_onboarding,
            commands::get_ai_settings,
            commands::save_ai_settings,
            commands::list_orders,
            commands::add_manual_order,
            commands::order_action,
            commands::order_mark_ordered,
            commands::order_set_tracking,
            commands::start_pairing,
            commands::extension_status,
            commands::revoke_extension,
            commands::check_update,
            commands::open_release_page,
        ])
        .run(tauri::generate_context!())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> BreakdownRequest {
        BreakdownRequest {
            supplier_price_minor: 100_000,
            logistics_minor: 0,
            packaging_minor: 0,
            margin_percent: 50.0,
            marketplace_fee_percent: 15.0,
            acquiring_and_tax_percent: 6.0,
            discount_striken_percent: 30.0,
            rounding_minor: 0,
            min_profit_minor: 0,
        }
    }

    #[test]
    fn calculator_command_uses_the_pricing_engine() {
        let b = breakdown_for(&request()).unwrap();
        assert_eq!((b.retail_price, b.net_profit), (189_875, 50_000));
    }

    #[test]
    fn calculator_errors_are_friendly_russian_text() {
        let mut bad = request();
        bad.marketplace_fee_percent = 99.0;
        let text = breakdown_for(&bad).unwrap_err().to_user_friendly_ru();
        assert!(text.starts_with("Не удалось рассчитать цену"), "{text}");
        assert!(!text.contains("PricingError"));
    }

    #[test]
    fn request_uses_camel_case_from_the_frontend() {
        let json = r#"{"supplierPriceMinor":1000,"logisticsMinor":0,"packagingMinor":0,"marginPercent":10,
            "marketplaceFeePercent":10,"acquiringAndTaxPercent":5,"discountStrikenPercent":0,"roundingMinor":0,"minProfitMinor":0}"#;
        let parsed: BreakdownRequest = serde_json::from_str(json).unwrap();
        assert_eq!(parsed.supplier_price_minor, 1000);
    }
}
