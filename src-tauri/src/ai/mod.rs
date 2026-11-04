//! Генерация карточек товара локальной (Ollama) или облачной моделью.
//!
//! * `content` — данные запроса/ответа, проверка ответа модели, подсказки (prompts);
//! * `providers` — настройки провайдеров и обмен по HTTP (Ollama, OpenAI-совместимые, Anthropic, Gemini);
//! * `stopwords` — фильтр запрещённых слов (aho-corasick);
//! * `worker` — обработка задач `AI_REWRITE` из очереди.

pub mod content;
pub mod providers;
pub mod store;
pub mod stopwords;
pub mod worker;

pub use content::{ContentRequest, GeneratedContent, Target};

/// Что пошло не так при обращении к модели.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum AiError {
    #[error("настройки ИИ: {0}")]
    Config(String),
    #[error("ключ API не принят провайдером (HTTP {0})")]
    Auth(u16),
    #[error("провайдер просит подождать (лимит запросов)")]
    RateLimited,
    #[error("ошибка провайдера (HTTP {status}): {message}")]
    Http { status: u16, message: String },
    #[error("нет связи с провайдером: {0}")]
    Transport(String),
    #[error("модель вернула непригодный ответ: {0}")]
    BadResponse(String),
}

impl AiError {
    /// Имеет ли смысл повторить позже. Неверный ключ и настройки — нет.
    pub fn retryable(&self) -> bool {
        match self {
            AiError::Config(_) | AiError::Auth(_) => false,
            AiError::RateLimited | AiError::Transport(_) => true,
            AiError::Http { status, .. } => *status >= 500 || *status == 408,
            // Модель могла просто сбиться: вторая попытка часто помогает.
            AiError::BadResponse(_) => true,
        }
    }

    pub fn to_user_friendly_ru(&self) -> String {
        match self {
            AiError::Config(m) => format!("Проверьте настройки ИИ: {m}."),
            AiError::Auth(_) => "Провайдер не принял ключ API. Проверьте, что ключ скопирован целиком и не истёк.".into(),
            AiError::RateLimited => "Провайдер ограничил число запросов. Приложение повторит попытку позже.".into(),
            AiError::Http { status, .. } => format!("Провайдер ИИ ответил ошибкой (код {status}). Попробуйте позже или смените модель."),
            AiError::Transport(_) => "Нет связи с ИИ. Если используете Ollama — запущена ли она? Для облака — проверьте интернет.".into(),
            AiError::BadResponse(_) => "ИИ вернул ответ, который не удалось разобрать. Приложение попробует ещё раз.".into(),
        }
    }
}

#[async_trait::async_trait]
pub trait ContentGenerator: Send + Sync {
    async fn generate(&self, request: &ContentRequest) -> Result<GeneratedContent, AiError>;
    /// Короткое имя для журнала («ollama / qwen3.5:9b-q4_K_M»).
    fn name(&self) -> String;
}
