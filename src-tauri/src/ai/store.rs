//! Хранение настроек ИИ: параметры — в `settings` открытым JSON, ключ API — зашифрованным (AES-256-GCM).

use std::sync::Arc;

use sqlx::SqlitePool;

use super::providers::{AiSettings, HttpGenerator, ProviderConfig};
use super::ContentGenerator;
use crate::error::{AppError, AppResult};
use crate::security::{hex_decode, hex_encode, CredentialVault};
use crate::settings;

const SETTINGS_KEY: &str = "ai";
const KEY_KEY: &str = "ai_api_key_enc";

pub async fn load_settings(pool: &SqlitePool) -> AppResult<AiSettings> {
    match settings::get(pool, SETTINGS_KEY).await? {
        Some(json) => serde_json::from_str(&json).map_err(|_| AppError::Validation("сохранённые настройки ИИ повреждены".into())),
        None => Ok(AiSettings::default()),
    }
}

/// Сохраняет настройки. `api_key`: `None` — оставить прежний, `Some("")` — удалить, иначе заменить.
/// Настройки проверяются до записи, поэтому в базе не окажется заведомо неработающая конфигурация.
pub async fn save_settings(
    pool: &SqlitePool,
    vault: &CredentialVault,
    new_settings: &AiSettings,
    api_key: Option<&str>,
    now: i64,
) -> AppResult<()> {
    let effective_key = match api_key {
        Some(key) => Some(key.trim().to_string()).filter(|k| !k.is_empty()),
        None => load_key(pool, vault).await?,
    };
    ProviderConfig::build(new_settings, effective_key.clone()).map_err(|e| AppError::Validation(e.to_string()))?;
    let json = serde_json::to_string(new_settings).map_err(|e| AppError::Validation(e.to_string()))?;
    settings::set(pool, SETTINGS_KEY, &json, now).await?;
    match (api_key, effective_key) {
        (Some(_), Some(key)) => settings::set(pool, KEY_KEY, &hex_encode(&vault.encrypt(key.as_bytes())?), now).await?,
        (Some(_), None) => {
            settings::delete(pool, KEY_KEY).await?;
        }
        (None, _) => {}
    }
    Ok(())
}

/// Есть ли сохранённый ключ (сам ключ наружу не отдаётся).
pub async fn has_key(pool: &SqlitePool) -> AppResult<bool> {
    Ok(settings::get(pool, KEY_KEY).await?.is_some())
}

async fn load_key(pool: &SqlitePool, vault: &CredentialVault) -> AppResult<Option<String>> {
    let Some(hex) = settings::get(pool, KEY_KEY).await? else { return Ok(None) };
    let blob = hex_decode(&hex).ok_or_else(|| AppError::Validation("сохранённый ключ ИИ повреждён".into()))?;
    let plain = vault.decrypt(&blob)?;
    String::from_utf8(plain).map(Some).map_err(|_| AppError::Validation("сохранённый ключ ИИ повреждён".into()))
}

/// Собирает генератор из сохранённых настроек.
pub async fn load_generator(pool: &SqlitePool, vault: &CredentialVault) -> AppResult<(Arc<dyn ContentGenerator>, String)> {
    let settings = load_settings(pool).await?;
    let key = load_key(pool, vault).await?;
    let config = ProviderConfig::build(&settings, key).map_err(|e| AppError::Validation(e.to_string()))?;
    let generator = HttpGenerator::new(config).map_err(|e| AppError::Validation(e.to_string()))?;
    Ok((Arc::new(generator), settings.language))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::providers::ProviderKind;

    async fn pool() -> (tempfile::TempDir, SqlitePool) {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("a.db")).await.unwrap();
        (dir, pool)
    }

    fn openai() -> AiSettings {
        AiSettings { provider: ProviderKind::Openai, model: None, base_url: None, language: "ru".into() }
    }

    #[tokio::test]
    async fn defaults_to_local_ollama() {
        let (_d, pool) = pool().await;
        assert_eq!(load_settings(&pool).await.unwrap().provider, ProviderKind::Ollama);
        let vault = CredentialVault::from_key([1; 32]);
        assert!(load_generator(&pool, &vault).await.unwrap().0.name().starts_with("ollama"));
    }

    #[tokio::test]
    async fn key_is_encrypted_at_rest_and_kept_when_not_resent() {
        let (_d, pool) = pool().await;
        let vault = CredentialVault::from_key([1; 32]);
        save_settings(&pool, &vault, &openai(), Some("sk-test-12345678"), 1).await.unwrap();
        let stored = settings::get(&pool, KEY_KEY).await.unwrap().unwrap();
        assert!(!stored.contains("sk-test") && hex_decode(&stored).is_some());
        // Смена модели без повторного ввода ключа.
        let mut changed = openai();
        changed.model = Some("gpt-4o".into());
        save_settings(&pool, &vault, &changed, None, 2).await.unwrap();
        assert!(load_generator(&pool, &vault).await.unwrap().0.name().contains("gpt-4o"));
        // Другой файл ключа — понятная ошибка, а не мусор.
        assert!(load_generator(&pool, &CredentialVault::from_key([2; 32])).await.is_err());
    }

    #[tokio::test]
    async fn invalid_settings_are_rejected_before_saving() {
        let (_d, pool) = pool().await;
        let vault = CredentialVault::from_key([1; 32]);
        assert!(save_settings(&pool, &vault, &openai(), None, 1).await.is_err()); // нет ключа
        assert!(settings::get(&pool, SETTINGS_KEY).await.unwrap().is_none());
        save_settings(&pool, &vault, &openai(), Some("sk-test-12345678"), 1).await.unwrap();
        // Пустая строка стирает ключ — но тогда облачный провайдер без ключа невалиден.
        assert!(save_settings(&pool, &vault, &openai(), Some(""), 2).await.is_err());
        assert!(load_generator(&pool, &vault).await.is_ok());
        // Переход на локальную модель разрешает стереть ключ.
        save_settings(&pool, &vault, &AiSettings::default(), Some(""), 3).await.unwrap();
        assert!(settings::get(&pool, KEY_KEY).await.unwrap().is_none());
    }

    #[tokio::test]
    async fn corrupted_stored_settings_are_an_error() {
        let (_d, pool) = pool().await;
        settings::set(&pool, SETTINGS_KEY, "{oops", 1).await.unwrap();
        assert!(load_settings(&pool).await.is_err());
    }
}
