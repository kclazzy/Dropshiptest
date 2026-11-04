//! Валюты поставщика (ТЗ, модуль 3): конвертация CNY/USD/EUR → валюта витрины по настраиваемому курсу
//! с запасом на волатильность. Всё в целых числах: курс хранится в «микро-единицах» (1 CNY = 12.5 RUB → 12_500_000).

use sqlx::SqlitePool;

use crate::error::{AppError, AppResult};
use crate::settings;

pub const RATE_SCALE: i128 = 1_000_000;
const MAX_BUFFER_BP: i64 = 5_000;
const BP: i128 = 10_000;

fn code_ok(code: &str) -> bool {
    code.len() == 3 && code.chars().all(|c| c.is_ascii_uppercase())
}

/// «12,5» / «12.5» / «0.0845» → микро-единицы (без float). Не более 6 знаков после запятой.
pub fn parse_rate(text: &str) -> Option<i64> {
    let t = text.trim().replace(',', ".");
    let (whole, frac) = t.split_once('.').unwrap_or((t.as_str(), ""));
    if whole.is_empty() && frac.is_empty() || frac.len() > 6 || !whole.chars().chain(frac.chars()).all(|c| c.is_ascii_digit()) {
        return None;
    }
    let whole: i64 = if whole.is_empty() { 0 } else { whole.parse().ok()? };
    let frac_scaled: i64 = if frac.is_empty() { 0 } else { format!("{frac:0<6}").parse().ok()? };
    let micro = whole.checked_mul(RATE_SCALE as i64)?.checked_add(frac_scaled)?;
    (micro > 0).then_some(micro)
}

/// Сумма в минорных единицах исходной валюты → минорные единицы валюты витрины, округление вверх
/// (выгоднее продавцу). `buffer_bp` — запас на волатильность курса в базисных пунктах.
pub fn convert_minor(amount_minor: i64, rate_micro: i64, buffer_bp: i64) -> AppResult<i64> {
    if amount_minor < 0 || rate_micro <= 0 || !(0..=MAX_BUFFER_BP).contains(&buffer_bp) {
        return Err(AppError::Validation("некорректная сумма, курс или запас на колебания".into()));
    }
    let too_big = || AppError::Validation("сумма после конвертации слишком велика".into());
    let scaled = (amount_minor as i128)
        .checked_mul(rate_micro as i128)
        .and_then(|v| v.checked_mul(BP + buffer_bp as i128))
        .ok_or_else(too_big)?;
    let denom = RATE_SCALE * BP;
    let result = (scaled + denom - 1) / denom;
    i64::try_from(result).map_err(|_| too_big())
}

pub async fn set_rate(pool: &SqlitePool, from: &str, rate_text: &str, now: i64) -> AppResult<()> {
    let from = from.trim().to_ascii_uppercase();
    let micro = parse_rate(rate_text).filter(|_| code_ok(&from)).ok_or_else(|| AppError::Validation("курс: код валюты из 3 букв и положительное число, например 12,5".into()))?;
    settings::set(pool, &format!("fx:{from}"), &micro.to_string(), now).await
}

pub async fn set_buffer_bp(pool: &SqlitePool, buffer_bp: i64, now: i64) -> AppResult<()> {
    if !(0..=MAX_BUFFER_BP).contains(&buffer_bp) {
        return Err(AppError::Validation("запас на колебания — от 0% до 50%".into()));
    }
    settings::set(pool, "fx_buffer_bp", &buffer_bp.to_string(), now).await
}

/// Переводит сумму из `from` в валюту витрины. Одна и та же валюта — без изменений; курс не задан — понятная ошибка.
pub async fn to_store_currency(pool: &SqlitePool, amount_minor: i64, from: &str, store_currency: &str) -> AppResult<i64> {
    let (from, to) = (from.trim().to_ascii_uppercase(), store_currency.trim().to_ascii_uppercase());
    if from == to {
        return Ok(amount_minor);
    }
    let rate = settings::get(pool, &format!("fx:{from}")).await?.and_then(|v| v.parse::<i64>().ok());
    let Some(rate) = rate else { return Err(AppError::Validation(format!("не задан курс {from} → {to}: укажите его в настройках"))) };
    if to != "RUB" {
        return Err(AppError::Validation("курсы сейчас задаются только к рублю".into()));
    }
    let buffer = settings::get(pool, "fx_buffer_bp").await?.and_then(|v| v.parse::<i64>().ok()).unwrap_or(300);
    convert_minor(amount_minor, rate, buffer)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rates_parse_without_floats() {
        for (text, want) in [("12,5", 12_500_000), ("12.5", 12_500_000), ("0.0845", 84_500), ("90", 90_000_000), (" .5 ", 500_000), ("0.000001", 1)] {
            assert_eq!(parse_rate(text), Some(want), "{text}");
        }
        for bad in ["", "abc", "-1", "0", "0.0000001", "1.2.3", "1e3", ".", "12,5 ₽"] {
            assert_eq!(parse_rate(bad), None, "{bad}");
        }
    }

    #[test]
    fn conversion_rounds_up_and_applies_the_buffer() {
        // 10.00 CNY × 12.5 = 125.00 ₽; +3% = 128.75 ₽
        assert_eq!(convert_minor(1_000, 12_500_000, 0).unwrap(), 12_500);
        assert_eq!(convert_minor(1_000, 12_500_000, 300).unwrap(), 12_875);
        assert_eq!(convert_minor(1, 1, 0).unwrap(), 1, "доли копейки округляются вверх");
        assert_eq!(convert_minor(0, 12_500_000, 300).unwrap(), 0);
    }

    #[test]
    fn conversion_rejects_nonsense_and_overflow() {
        assert!(convert_minor(-1, 1, 0).is_err());
        assert!(convert_minor(1, 0, 0).is_err());
        assert!(convert_minor(1, 1, 5_001).is_err());
        assert!(convert_minor(i64::MAX, i64::MAX, 0).is_err());
    }

    #[tokio::test]
    async fn store_currency_conversion_uses_saved_rate_and_buffer() {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("fx.db")).await.unwrap();
        assert_eq!(to_store_currency(&pool, 500, "rub", "RUB").await.unwrap(), 500);
        let missing = to_store_currency(&pool, 1_000, "CNY", "RUB").await.unwrap_err().to_user_friendly_ru();
        assert!(missing.contains("курс") && missing.contains("CNY"), "{missing}");
        set_rate(&pool, "cny", "12,5", 1).await.unwrap();
        assert_eq!(to_store_currency(&pool, 1_000, "CNY", "RUB").await.unwrap(), 12_875, "запас по умолчанию 3%");
        set_buffer_bp(&pool, 0, 2).await.unwrap();
        assert_eq!(to_store_currency(&pool, 1_000, "CNY", "RUB").await.unwrap(), 12_500);
        assert!(set_buffer_bp(&pool, 9_999, 3).await.is_err());
        assert!(set_rate(&pool, "yuan", "1", 1).await.is_err());
        assert!(set_rate(&pool, "CNY", "0", 1).await.is_err());
        assert!(to_store_currency(&pool, 1, "CNY", "USD").await.is_err());
    }
}
