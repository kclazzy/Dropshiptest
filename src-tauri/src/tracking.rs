//! Защита трек-номеров (ТЗ 4.5): до отправки на площадку номер проверяется на формат и на «выдающие» префиксы
//! служб, которые маркетплейс не принимает или которые раскрывают прямую отгрузку со склада конкурента.
//! Агрегаторы (17TRACK, Cainiao, Почта России) требуют внешних API — здесь только локальная проверка.

use crate::error::{AppError, AppResult};

/// Префиксы, которые не передаём покупателю/площадке как есть (регистр не важен).
const BLOCKED_PREFIXES: &[(&str, &str)] = &[
    ("LP", "Cainiao/AliExpress Standard"),
    ("AE", "AliExpress"),
    ("YT", "Yanwen/YunExpress"),
    ("UB", "Cainiao Super Economy"),
    ("SF", "SF Express"),
    ("1Z", "UPS (трансграничная отгрузка)"),
];

/// Международный формат S10 (две буквы + 9 цифр + две буквы страны), например `RB123456785CN`.
fn is_s10(track: &str) -> bool {
    let b = track.as_bytes();
    b.len() == 13 && b[..2].iter().all(u8::is_ascii_uppercase) && b[2..11].iter().all(u8::is_ascii_digit) && b[11..].iter().all(u8::is_ascii_uppercase)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Checked {
    /// Нормализованный номер (верхний регистр, без пробелов и дефисов).
    pub track: String,
    /// Отправление из-за рубежа (код страны не RU): стоит предупредить пользователя.
    pub foreign_origin: bool,
}

pub fn check(raw: &str) -> AppResult<Checked> {
    let track: String = raw.chars().filter(|c| !c.is_whitespace() && *c != '-').collect::<String>().to_ascii_uppercase();
    if track.len() < 8 || track.len() > 40 || !track.chars().all(|c| c.is_ascii_alphanumeric()) {
        return Err(AppError::Validation("трек-номер должен состоять из 8–40 латинских букв и цифр".into()));
    }
    if let Some((_, service)) = BLOCKED_PREFIXES.iter().find(|(prefix, _)| track.starts_with(prefix)) {
        return Err(AppError::Validation(format!("трек-номер службы «{service}» площадка может отклонить. Дождитесь локального номера (Почта России, СДЭК и др.) и введите его")));
    }
    let foreign_origin = is_s10(&track) && !track.ends_with("RU");
    Ok(Checked { track, foreign_origin })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_and_accepts_domestic_numbers() {
        assert_eq!(check(" 1234-5678 9012 ").unwrap(), Checked { track: "123456789012".into(), foreign_origin: false });
        assert_eq!(check("rb123456785ru").unwrap(), Checked { track: "RB123456785RU".into(), foreign_origin: false });
    }

    #[test]
    fn foreign_s10_is_accepted_but_flagged() {
        assert_eq!(check("RB123456785CN").unwrap().foreign_origin, true);
    }

    #[test]
    fn suspicious_carriers_and_garbage_are_refused_with_a_human_reason() {
        for bad in ["LP00123456789012", "ae123456789", "YT2012345678901234", "1Z999AA10123456784"] {
            let msg = check(bad).unwrap_err().to_user_friendly_ru();
            assert!(msg.contains("локального номера"), "{bad}: {msg}");
        }
        for bad in ["", "123", "ABC DEF!", "привет12345", &"9".repeat(41)] {
            assert!(check(bad).is_err(), "{bad}");
        }
    }
}
