//! Модели таблиц. Перечисления хранятся в базе текстом (как в DDL); неизвестное значение при чтении — ошибка.

use serde::Serialize;
use sqlx::FromRow;

use crate::pricing::{PricingError, PricingRule, RuleSpec};

macro_rules! text_enum {
    ($(#[$meta:meta])* $name:ident, $rename:literal, { $($variant:ident),+ $(,)? }) => {
        $(#[$meta])*
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, sqlx::Type)]
        #[sqlx(type_name = "TEXT", rename_all = $rename)]
        #[serde(rename_all = $rename)]
        pub enum $name { $($variant),+ }
    };
}

text_enum!(PlatformType, "lowercase", { Ozon, Avito, Shopify, Woocommerce, Ebay, Local });
text_enum!(AiStatus, "SCREAMING_SNAKE_CASE", { Pending, Processing, Ready, Error });
text_enum!(MonitorStatus, "SCREAMING_SNAKE_CASE", { Active, Paused, CaptchaBlocked, Error });
text_enum!(TaskStatus, "SCREAMING_SNAKE_CASE", { Pending, InProgress, Completed, Failed });
text_enum!(TaskType, "SCREAMING_SNAKE_CASE", { AiRewrite, OzonPublish, AvitoFeedSync, PriceSync, AutoOrder });
text_enum!(OrderStatus, "SCREAMING_SNAKE_CASE", {
    New, ReadyToBuy, QueuedForExt, Ordered, Shipped, ManualReview, Error
});

/// Схема доставки заказа ('rFBS', 'FBS', 'AVITO_DELIVERY', 'DIRECT').
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, sqlx::Type)]
#[sqlx(type_name = "TEXT")]
pub enum DeliveryScheme {
    #[sqlx(rename = "rFBS")]
    #[serde(rename = "rFBS")]
    RFbs,
    #[sqlx(rename = "FBS")]
    #[serde(rename = "FBS")]
    Fbs,
    #[sqlx(rename = "AVITO_DELIVERY")]
    #[serde(rename = "AVITO_DELIVERY")]
    AvitoDelivery,
    #[sqlx(rename = "DIRECT")]
    #[serde(rename = "DIRECT")]
    Direct,
}

#[derive(Debug, Clone, FromRow, Serialize)]
pub struct Store {
    pub id: i64,
    pub name: String,
    pub platform_type: PlatformType,
    pub api_url: String,
    /// Зашифрованные ключи никогда не отправляются в интерфейс.
    #[serde(skip)]
    pub encrypted_credentials: Vec<u8>,
    pub warehouse_id: Option<String>,
    pub currency_code: String,
    pub rate_limit_rps: f64,
    pub is_active: bool,
    pub created_at: i64,
}

#[derive(Debug, Clone, FromRow, Serialize)]
pub struct PricingRuleRow {
    pub id: i64,
    pub store_id: i64,
    pub rule_name: String,
    pub price_from_minor: i64,
    pub price_to_minor: i64,
    pub margin_percent: f64,
    pub fixed_markup_minor: i64,
    pub marketplace_fee_percent: f64,
    pub acquiring_and_tax_percent: f64,
    pub discount_striken_percent: f64,
    pub rounding_minor: i64,
    pub min_stock_threshold: i64,
    pub min_profit_minor: i64,
}

impl PricingRuleRow {
    /// Проверенное правило для `PricingEngine`; повреждённая строка — ошибка, а не тихая подмена.
    pub fn to_rule(&self) -> Result<PricingRule, PricingError> {
        PricingRule::new(RuleSpec {
            price_from_minor: self.price_from_minor,
            price_to_minor: self.price_to_minor,
            margin_percent: self.margin_percent,
            fixed_markup_minor: self.fixed_markup_minor,
            marketplace_fee_percent: self.marketplace_fee_percent,
            acquiring_and_tax_percent: self.acquiring_and_tax_percent,
            discount_striken_percent: self.discount_striken_percent,
            rounding_minor: self.rounding_minor,
            min_stock_threshold: self.min_stock_threshold,
            min_profit_minor: self.min_profit_minor,
        })
    }
}

#[derive(Debug, Clone, FromRow, Serialize)]
pub struct Proxy {
    pub id: i64,
    /// Содержит логин и пароль — в интерфейс не отправляется.
    #[serde(skip)]
    pub proxy_url: String,
    pub fail_count: i64,
    pub last_used_at: i64,
    pub cooldown_until: i64,
    pub is_active: bool,
}

#[derive(Debug, Clone, FromRow, Serialize)]
pub struct Product {
    pub id: i64,
    pub store_id: i64,
    pub external_store_id: Option<String>,
    pub external_offer_id: String,
    pub supplier_url: String,
    pub supplier_product_id: String,
    pub url_sha256: String,
    pub raw_title: String,
    pub raw_description: Option<String>,
    pub ai_seo_title: Option<String>,
    pub ai_avito_title: Option<String>,
    pub ai_description: Option<String>,
    pub ai_attributes_json: Option<String>,
    pub ai_tags_json: Option<String>,
    pub images_json: String,
    pub marketplace_category_id: Option<String>,
    pub package_dimensions_json: String,
    pub ai_status: AiStatus,
    pub monitor_status: MonitorStatus,
    pub check_interval_sec: i64,
    pub last_checked_at: i64,
    pub next_check_at: i64,
}

#[derive(Debug, Clone, FromRow, Serialize)]
pub struct ProductVariant {
    pub id: i64,
    pub product_id: i64,
    pub external_variant_id: Option<String>,
    pub supplier_sku: String,
    pub attributes_json: String,
    pub supplier_price_minor: i64,
    pub shipping_price_minor: i64,
    pub retail_price_minor: i64,
    pub min_price_minor: i64,
    pub old_price_minor: i64,
    pub supplier_stock: i64,
    pub is_in_stock: bool,
    pub state_hash: String,
    pub updated_at: i64,
    pub published_hash: Option<String>,
    pub rejected_hash: Option<String>,
}

#[derive(Debug, Clone, FromRow, Serialize)]
pub struct Order {
    pub id: i64,
    pub store_id: i64,
    pub external_order_id: String,
    pub variant_id: Option<i64>,
    pub quantity: i64,
    pub delivery_scheme: DeliveryScheme,
    pub buyer_address_json: String,
    pub retail_total_minor: i64,
    pub supplier_cost_minor: Option<i64>,
    pub net_profit_minor: Option<i64>,
    pub status: OrderStatus,
    pub supplier_order_id: Option<String>,
    pub tracking_number: Option<String>,
    pub tracking_carrier: Option<String>,
    pub human_readable_hint: Option<String>,
    pub error_message: Option<String>,
    pub created_at: i64,
    pub updated_at: i64,
}

#[derive(Debug, Clone, FromRow, Serialize)]
pub struct Task {
    pub id: i64,
    pub task_type: TaskType,
    pub priority: i64,
    pub payload_json: String,
    pub status: TaskStatus,
    pub attempts: i64,
    pub max_attempts: i64,
    pub scheduled_at: i64,
    pub locked_at: i64,
    pub error_log: Option<String>,
}
