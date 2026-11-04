//! Единый тип ошибки приложения. Технические детали пишутся в журнал, пользователю — понятный русский текст.

use crate::pricing::PricingError;

#[derive(Debug, thiserror::Error)]
pub enum AppError {
    #[error("database error: {0}")]
    Database(#[from] sqlx::Error),
    #[error("migration error: {0}")]
    Migration(#[from] sqlx::migrate::MigrateError),
    #[error("pricing error: {0}")]
    Pricing(#[from] PricingError),
    #[error("invalid data: {0}")]
    Validation(String),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
}

pub type AppResult<T> = Result<T, AppError>;

impl AppError {
    /// Текст для показа пользователю: без стектрейсов, SQL и внутренних названий.
    pub fn to_user_friendly_ru(&self) -> String {
        match self {
            AppError::Database(sqlx::Error::Database(e)) if e.is_unique_violation() => {
                "Такая запись уже существует.".to_string()
            }
            AppError::Database(_) | AppError::Migration(_) => {
                "Не удалось обратиться к локальной базе данных. Закройте другие копии приложения и повторите.".to_string()
            }
            AppError::Pricing(e) => format!("Не удалось рассчитать цену: {e}."),
            AppError::Validation(message) => format!("Некорректные данные: {message}."),
            AppError::Io(_) => "Не удалось прочитать или записать файл. Проверьте права доступа к папке данных.".to_string(),
        }
    }
}

// Tauri-команды возвращают ошибку как строку; наружу уходит только дружелюбный текст.
impl serde::Serialize for AppError {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&self.to_user_friendly_ru())
    }
}
