//! Единый интерфейс витрин (Ozon, Avito, …). Бизнес-логика знает только `StoreAdapter`.
//! Операции, которые площадка/адаптер не поддерживает, возвращают `Unsupported` с понятным текстом.
//!
//! ⚠ Адреса, заголовки и тела запросов Ozon/Avito написаны по памяти и вторичным источникам: официальная
//! документация в среде разработки была недоступна. Проверены только на тестовом сервере.

pub mod avito;
pub mod ozon;

use std::num::NonZeroU32;
use std::sync::Arc;
use std::time::Duration;

use governor::{DefaultDirectRateLimiter, Quota, RateLimiter};
use reqwest::header::HeaderMap;
use reqwest::{redirect, StatusCode, Url};
use serde::Deserialize;
use sqlx::SqlitePool;

use crate::db::{PlatformType, Store};
use crate::error::{AppError, AppResult};
use crate::security::CredentialVault;

pub const HTTP_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StoreErrorKind {
    /// Неверный/просроченный ключ: повтор бессмыслен, нужен пользователь.
    Auth,
    /// Слишком часто: повторить позже.
    RateLimited,
    /// Площадка отклонила запрос целиком (неверные данные).
    Rejected,
    /// Сеть/5xx/непонятный ответ: повторить.
    Transport,
    /// Операция недоступна для этой площадки.
    Unsupported,
    /// Не хватает настроек магазина.
    Config,
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("{message}")]
pub struct StoreError {
    pub kind: StoreErrorKind,
    pub http_status: u16,
    pub message: String,
    pub retry_after_secs: u64,
}

impl StoreError {
    pub fn new(kind: StoreErrorKind, message: impl Into<String>) -> Self {
        Self { kind, http_status: 0, message: message.into(), retry_after_secs: 0 }
    }
    pub fn retryable(&self) -> bool {
        matches!(self.kind, StoreErrorKind::Transport | StoreErrorKind::RateLimited)
    }
    pub fn to_user_friendly_ru(&self) -> String {
        match self.kind {
            StoreErrorKind::Auth => "Площадка не приняла ключи доступа. Проверьте их в настройках магазина.".into(),
            StoreErrorKind::RateLimited => "Площадка просит подождать (лимит запросов). Приложение повторит позже.".into(),
            StoreErrorKind::Rejected => format!("Площадка отклонила запрос: {}", self.message),
            StoreErrorKind::Transport => "Нет связи с площадкой. Приложение повторит попытку.".into(),
            StoreErrorKind::Unsupported => format!("Для этой площадки пока недоступно: {}", self.message),
            StoreErrorKind::Config => format!("Проверьте настройки магазина: {}", self.message),
        }
    }
}

pub type StoreResult<T> = Result<T, StoreError>;

/// Что отправляем на витрину по одному варианту (суммы в копейках).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VariantUpdate {
    pub variant_id: i64,
    /// `offer_id` (Ozon) / `item_id` (Avito): связь карточки с нашим вариантом. Пусто — вариант не привязан.
    pub external_id: String,
    /// 0 — цена не рассчитана: цену не отправляем, только остаток.
    pub retail_minor: i64,
    pub min_price_minor: i64,
    pub old_price_minor: i64,
    /// Уже с защитой остатков (0, если ниже порога).
    pub stock: i64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ItemResult {
    pub variant_id: i64,
    pub ok: bool,
    pub price_sent: bool,
    pub stock_sent: bool,
    pub error: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StoreOrder {
    pub external_order_id: String,
    pub external_variant_id: String,
    pub quantity: i64,
    pub total_minor: i64,
    pub buyer_address_json: String,
}

#[async_trait::async_trait]
pub trait StoreAdapter: Send + Sync {
    fn name(&self) -> String;

    /// Результат по каждому варианту; ошибка всего вызова — если запрос не удался целиком.
    async fn update_variants(&self, updates: &[VariantUpdate]) -> StoreResult<Vec<ItemResult>>;

    async fn fetch_new_orders(&self, _since: i64) -> StoreResult<Vec<StoreOrder>> {
        Err(StoreError::new(StoreErrorKind::Unsupported, format!("{}: получение заказов пока не поддерживается", self.name())))
    }

    async fn submit_tracking(&self, _external_order_id: &str, _tracking_number: &str) -> StoreResult<()> {
        Err(StoreError::new(StoreErrorKind::Unsupported, format!("{}: передача трек-номера пока не поддерживается", self.name())))
    }
}

/// Сопоставление ответа HTTP с ошибкой. Тело ответа в сообщение не попадает целиком (может содержать лишнее).
pub(crate) fn classify(status: StatusCode, headers: &HeaderMap, body: &str) -> StoreError {
    let code = status.as_u16();
    let retry_after = headers.get("retry-after").and_then(|v| v.to_str().ok()).and_then(|v| v.trim().parse::<u64>().ok()).unwrap_or(0);
    let snippet: String = body.chars().take(200).collect();
    let (kind, message) = match code {
        401 | 403 => (StoreErrorKind::Auth, "ключи не приняты".to_string()),
        429 => (StoreErrorKind::RateLimited, "лимит запросов".to_string()),
        408 | 500..=599 => (StoreErrorKind::Transport, format!("HTTP {code}")),
        _ => (StoreErrorKind::Rejected, format!("HTTP {code}: {snippet}")),
    };
    StoreError { kind, http_status: code, message, retry_after_secs: retry_after }
}

pub(crate) fn http_client() -> StoreResult<reqwest::Client> {
    // Без редиректов: заголовки с ключами не должны уходить на другой адрес.
    reqwest::Client::builder()
        .timeout(HTTP_TIMEOUT)
        .redirect(redirect::Policy::none())
        .build()
        .map_err(|e| StoreError::new(StoreErrorKind::Config, format!("HTTP-клиент: {e}")))
}

pub(crate) fn limiter(rps: f64) -> StoreResult<DefaultDirectRateLimiter> {
    if !rps.is_finite() || rps <= 0.0 || rps > 1000.0 {
        return Err(StoreError::new(StoreErrorKind::Config, "лимит запросов в секунду должен быть больше 0"));
    }
    let period = Duration::from_secs_f64(1.0 / rps);
    let quota = Quota::with_period(period)
        .ok_or_else(|| StoreError::new(StoreErrorKind::Config, "неверный лимит запросов"))?
        .allow_burst(NonZeroU32::MIN);
    Ok(RateLimiter::direct(quota))
}

/// Рубли с округлением вверх (цена не должна оказаться ниже рассчитанной).
pub(crate) fn rubles_ceil(minor: i64) -> i64 {
    minor.saturating_add(99).div_euclid(100)
}

/// Адрес API: https, либо http на этот компьютер (тестовый сервер, прокси).
pub(crate) fn validate_base_url(url: &str) -> StoreResult<Url> {
    let parsed = Url::parse(url.trim()).map_err(|_| StoreError::new(StoreErrorKind::Config, "адрес API указан неверно"))?;
    let local = match parsed.host_str() {
        Some("localhost") => true,
        Some(host) => host.parse::<std::net::IpAddr>().map(|ip| ip.is_loopback()).unwrap_or(false),
        None => false,
    };
    match parsed.scheme() {
        "https" => Ok(parsed),
        "http" if local => Ok(parsed),
        _ => Err(StoreError::new(StoreErrorKind::Config, "адрес API должен начинаться с https://")),
    }
}

pub(crate) fn join(base: &Url, path: &str) -> String {
    format!("{}{}", base.as_str().trim_end_matches('/'), path)
}

#[derive(Debug, Deserialize)]
struct OzonCredentials {
    client_id: String,
    api_key: String,
}

#[derive(Debug, Deserialize)]
struct AvitoCredentials {
    client_id: String,
    client_secret: String,
}

/// Добавляемый магазин. `credentials_json` — как ввёл пользователь; в базе окажется только шифротекст.
pub struct NewStore<'a> {
    pub name: &'a str,
    pub platform: PlatformType,
    pub credentials_json: &'a str,
    /// Склад Ozon FBS/rFBS (обязателен для Ozon).
    pub warehouse_id: Option<&'a str>,
    /// Только для тестов и прокси; по умолчанию — боевой адрес площадки.
    pub api_url: Option<&'a str>,
    pub now: i64,
}

fn default_api_url(platform: PlatformType) -> Option<&'static str> {
    match platform {
        PlatformType::Ozon => Some("https://api-seller.ozon.ru"),
        PlatformType::Avito => Some("https://api.avito.ru"),
        _ => None,
    }
}

fn check_credentials(platform: PlatformType, json: &str, warehouse: Option<&str>) -> Result<(), String> {
    let nonempty = |s: &str| !s.trim().is_empty();
    match platform {
        PlatformType::Ozon => {
            let c: OzonCredentials = serde_json::from_str(json).map_err(|_| "для Ozon нужны client_id и api_key".to_string())?;
            if !nonempty(&c.client_id) || !nonempty(&c.api_key) {
                return Err("для Ozon нужны client_id и api_key".into());
            }
            if !warehouse.is_some_and(|w| w.trim().parse::<i64>().is_ok_and(|n| n > 0)) {
                return Err("для Ozon укажите номер склада (положительное число)".into());
            }
        }
        PlatformType::Avito => {
            let c: AvitoCredentials = serde_json::from_str(json).map_err(|_| "для Avito нужны client_id и client_secret".to_string())?;
            if !nonempty(&c.client_id) || !nonempty(&c.client_secret) {
                return Err("для Avito нужны client_id и client_secret".into());
            }
        }
        _ => return Err("эта площадка пока не поддерживается".into()),
    }
    Ok(())
}

/// Проверяет ввод, шифрует ключи и сохраняет магазин. Возвращает id.
pub async fn add_store(pool: &SqlitePool, vault: &CredentialVault, new: NewStore<'_>) -> AppResult<i64> {
    if new.name.trim().is_empty() || new.name.chars().count() > 100 {
        return Err(AppError::Validation("название магазина — от 1 до 100 символов".into()));
    }
    check_credentials(new.platform, new.credentials_json, new.warehouse_id).map_err(AppError::Validation)?;
    let api_url = new.api_url.or(default_api_url(new.platform)).unwrap_or_default();
    validate_base_url(api_url).map_err(|e| AppError::Validation(e.message))?;
    let blob = vault.encrypt(new.credentials_json.as_bytes())?;
    let id: i64 = sqlx::query_scalar(
        "INSERT INTO stores (name, platform_type, api_url, encrypted_credentials, warehouse_id, created_at) \
         VALUES (?, ?, ?, ?, ?, ?) RETURNING id",
    )
    .bind(new.name.trim())
    .bind(new.platform)
    .bind(api_url.trim())
    .bind(blob)
    .bind(new.warehouse_id.map(str::trim))
    .bind(new.now)
    .fetch_one(pool)
    .await?;
    Ok(id)
}

/// Собирает адаптер магазина: расшифровывает ключи и настраивает лимитер.
pub fn build_adapter(store: &Store, vault: &CredentialVault) -> StoreResult<Arc<dyn StoreAdapter>> {
    let plain = vault
        .decrypt(&store.encrypted_credentials)
        .map_err(|_| StoreError::new(StoreErrorKind::Config, "не удалось расшифровать ключи магазина"))?;
    let text = String::from_utf8(plain).map_err(|_| StoreError::new(StoreErrorKind::Config, "ключи магазина повреждены"))?;
    let base = validate_base_url(&store.api_url)?;
    let broken = || StoreError::new(StoreErrorKind::Config, "ключи магазина повреждены");
    match store.platform_type {
        PlatformType::Ozon => {
            let c: OzonCredentials = serde_json::from_str(&text).map_err(|_| broken())?;
            let warehouse = store
                .warehouse_id
                .as_deref()
                .and_then(|w| w.trim().parse::<i64>().ok())
                .filter(|w| *w > 0)
                .ok_or_else(|| StoreError::new(StoreErrorKind::Config, "не указан склад Ozon"))?;
            Ok(Arc::new(ozon::OzonAdapter::new(base, c.client_id, c.api_key, warehouse, store.rate_limit_rps)?))
        }
        PlatformType::Avito => {
            let c: AvitoCredentials = serde_json::from_str(&text).map_err(|_| broken())?;
            Ok(Arc::new(avito::AvitoAdapter::new(base, c.client_id, c.client_secret, store.rate_limit_rps)?))
        }
        _ => Err(StoreError::new(StoreErrorKind::Unsupported, "эта площадка пока не поддерживается")),
    }
}

#[cfg(test)]
mod tests;
