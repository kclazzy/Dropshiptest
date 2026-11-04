//! Привязка расширения в один клик: приложение показывает одноразовый код (живёт 2 минуты),
//! расширение меняет его на постоянный токен. В базе хранится только SHA-256 токена.

use std::sync::Mutex;

use sqlx::SqlitePool;

use crate::error::AppResult;
use crate::security::{constant_time_eq, random_token, sha256_hex};
use crate::settings;

const CODE_TTL_SECS: i64 = 120;
const MAX_WRONG_ATTEMPTS: u32 = 5;
const TOKEN_KEY: &str = "bridge_token_sha256";

struct PendingCode {
    code: String,
    expires_at: i64,
    wrong_attempts: u32,
}

#[derive(Default)]
pub struct Pairing {
    pending: Mutex<Option<PendingCode>>,
}

impl Pairing {
    /// Новый код заменяет предыдущий. Показывается пользователю в приложении.
    pub fn start(&self, now: i64) -> String {
        let code = random_token()[..8].to_uppercase();
        if let Ok(mut slot) = self.pending.lock() {
            *slot = Some(PendingCode { code: code.clone(), expires_at: now + CODE_TTL_SECS, wrong_attempts: 0 });
        }
        code
    }

    /// Код одноразовый: после успеха или `MAX_WRONG_ATTEMPTS` ошибок он сгорает.
    pub fn redeem(&self, candidate: &str, now: i64) -> bool {
        let Ok(mut slot) = self.pending.lock() else { return false };
        let Some(pending) = slot.as_mut() else { return false };
        if now > pending.expires_at {
            *slot = None;
            return false;
        }
        if constant_time_eq(&pending.code, &candidate.trim().to_uppercase()) {
            *slot = None;
            return true;
        }
        pending.wrong_attempts += 1;
        if pending.wrong_attempts >= MAX_WRONG_ATTEMPTS {
            *slot = None;
        }
        false
    }
}

/// Выдаёт новый токен; прежний перестаёт действовать (одно расширение — один токен).
pub async fn issue_token(pool: &SqlitePool, now: i64) -> AppResult<String> {
    let token = random_token();
    settings::set(pool, TOKEN_KEY, &sha256_hex(&token), now).await?;
    Ok(token)
}

pub async fn verify_token(pool: &SqlitePool, token: &str) -> AppResult<bool> {
    Ok(match settings::get(pool, TOKEN_KEY).await? {
        Some(stored) => constant_time_eq(&stored, &sha256_hex(token)),
        None => false,
    })
}

pub async fn revoke_token(pool: &SqlitePool) -> AppResult<bool> {
    settings::delete(pool, TOKEN_KEY).await
}
