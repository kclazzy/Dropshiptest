//! Dropship: ядро локальной системы дропшиппинга. GUI — Tauri v2, данные — SQLite (WAL).

pub mod ai;
pub mod db;
pub mod error;
pub mod pricing;
pub mod security;
pub mod settings;
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
        .setup(|app| {
            let data_dir = app.path().app_data_dir()?;
            let pool = tauri::async_runtime::block_on(db::init_pool(&data_dir.join("dropship.db")))?;
            tracing::info!("database ready in {}", data_dir.display());
            app.manage(AppState { pool });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![price_breakdown])
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
