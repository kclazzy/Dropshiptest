//! Провайдеры ИИ: локальная Ollama и облако. Четыре формата обмена (wire):
//! Ollama `/api/chat`, OpenAI-совместимый `/chat/completions` (OpenAI, OpenRouter, Groq, Together, Mistral, DeepSeek, любой «custom»),
//! Anthropic `/messages`, Gemini `generateContent`.
//! Построение запроса и разбор ответа — чистые функции; сетевой вызов один (`HttpGenerator::generate`).
//! ⚠ Адреса и названия моделей в `presets()` указаны по памяти и не сверены с актуальной документацией провайдеров.

use std::time::Duration;

use reqwest::{redirect, StatusCode, Url};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use super::content::{build_prompts, output_schema, parse_model_output, ContentRequest, GeneratedContent};
use super::{AiError, ContentGenerator};

/// Модель по умолчанию для локального запуска (ТЗ).
pub const DEFAULT_LOCAL_MODEL: &str = "qwen3.5:9b-q4_K_M";
const TEMPERATURE: f64 = 0.4;
const MAX_ERROR_SNIPPET: usize = 200;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ProviderKind {
    Ollama,
    Openai,
    Openrouter,
    Groq,
    Together,
    Mistral,
    Deepseek,
    Gemini,
    Anthropic,
    /// Любой сервис с OpenAI-совместимым API (LM Studio, vLLM, Яндекс и др. — адрес указывает пользователь).
    Custom,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Wire {
    Ollama,
    OpenAi,
    Anthropic,
    Gemini,
}

#[derive(Debug, Clone, Serialize)]
pub struct Preset {
    pub kind: ProviderKind,
    pub label: &'static str,
    pub wire: Wire,
    pub base_url: &'static str,
    pub default_model: &'static str,
    pub needs_key: bool,
    /// Где получить ключ (показывается в интерфейсе рядом с полем).
    pub key_url: &'static str,
}

pub fn presets() -> Vec<Preset> {
    use ProviderKind::*;
    let p = |kind, label, wire, base_url, default_model, needs_key, key_url| Preset {
        kind, label, wire, base_url, default_model, needs_key, key_url,
    };
    vec![
        p(Ollama, "Ollama (на вашем компьютере, бесплатно)", Wire::Ollama, "http://127.0.0.1:11434", DEFAULT_LOCAL_MODEL, false, "https://ollama.com/download"),
        p(Openai, "OpenAI", Wire::OpenAi, "https://api.openai.com/v1", "gpt-4o-mini", true, "https://platform.openai.com/api-keys"),
        p(Anthropic, "Anthropic (Claude)", Wire::Anthropic, "https://api.anthropic.com/v1", "claude-haiku-5-5", true, "https://console.anthropic.com/settings/keys"),
        p(Gemini, "Google Gemini", Wire::Gemini, "https://generativelanguage.googleapis.com/v1beta", "gemini-2.0-flash", true, "https://aistudio.google.com/app/apikey"),
        p(Openrouter, "OpenRouter (много моделей, один ключ)", Wire::OpenAi, "https://openrouter.ai/api/v1", "openai/gpt-4o-mini", true, "https://openrouter.ai/keys"),
        p(Groq, "Groq", Wire::OpenAi, "https://api.groq.com/openai/v1", "llama-3.3-70b-versatile", true, "https://console.groq.com/keys"),
        p(Together, "Together AI", Wire::OpenAi, "https://api.together.xyz/v1", "meta-llama/Llama-3.3-70B-Instruct-Turbo", true, "https://api.together.ai/settings/api-keys"),
        p(Mistral, "Mistral", Wire::OpenAi, "https://api.mistral.ai/v1", "mistral-small-latest", true, "https://console.mistral.ai/api-keys"),
        p(Deepseek, "DeepSeek", Wire::OpenAi, "https://api.deepseek.com/v1", "deepseek-chat", true, "https://platform.deepseek.com/api_keys"),
        p(Custom, "Другой сервис (совместим с OpenAI)", Wire::OpenAi, "", "", false, ""),
    ]
}

pub fn preset(kind: ProviderKind) -> Preset {
    presets().into_iter().find(|p| p.kind == kind).unwrap_or_else(|| presets().remove(0))
}

/// Настройки, которые видит и меняет пользователь (без ключа: ключ хранится отдельно, зашифрованным).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AiSettings {
    pub provider: ProviderKind,
    #[serde(default)]
    pub model: Option<String>,
    #[serde(default)]
    pub base_url: Option<String>,
    #[serde(default = "default_language")]
    pub language: String,
}

fn default_language() -> String {
    "ru".into()
}

impl Default for AiSettings {
    fn default() -> Self {
        Self { provider: ProviderKind::Ollama, model: None, base_url: None, language: default_language() }
    }
}

/// Готовые к использованию параметры (после проверки на границе).
#[derive(Clone)]
pub struct ProviderConfig {
    pub kind: ProviderKind,
    pub wire: Wire,
    pub base_url: Url,
    pub model: String,
    pub api_key: Option<String>,
    pub language: String,
    pub timeout: Duration,
}

impl std::fmt::Debug for ProviderConfig {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ProviderConfig")
            .field("kind", &self.kind)
            .field("base_url", &self.base_url.as_str())
            .field("model", &self.model)
            .field("api_key", &self.api_key.as_ref().map(|_| "***"))
            .finish()
    }
}

fn is_local_host(url: &Url) -> bool {
    match url.host_str() {
        Some("localhost") => true,
        Some(host) => host.parse::<std::net::IpAddr>().map(|ip| ip.is_loopback()).unwrap_or(false),
        None => false,
    }
}

impl ProviderConfig {
    /// Проверка настроек на входе: после неё внутри можно доверять данным.
    pub fn build(settings: &AiSettings, api_key: Option<String>) -> Result<Self, AiError> {
        let preset = preset(settings.provider);
        let base = settings.base_url.clone().filter(|s| !s.trim().is_empty()).unwrap_or_else(|| preset.base_url.to_string());
        let base_url = Url::parse(base.trim().trim_end_matches('/'))
            .map_err(|_| AiError::Config("адрес сервиса указан неверно".into()))?;
        if base_url.scheme() != "https" && base_url.scheme() != "http" {
            return Err(AiError::Config("адрес сервиса должен начинаться с https://".into()));
        }
        let model = settings.model.clone().filter(|s| !s.trim().is_empty()).unwrap_or_else(|| preset.default_model.to_string());
        if model.trim().is_empty() {
            return Err(AiError::Config("укажите название модели".into()));
        }
        let api_key = api_key.map(|k| k.trim().to_string()).filter(|k| !k.is_empty());
        if preset.needs_key && api_key.is_none() {
            return Err(AiError::Config("не указан ключ API".into()));
        }
        // Ключ не отправляется по открытому http на чужой адрес.
        if api_key.is_some() && base_url.scheme() == "http" && !is_local_host(&base_url) {
            return Err(AiError::Config("с ключом API нужен адрес https://".into()));
        }
        Ok(Self {
            kind: settings.provider,
            wire: preset.wire,
            base_url,
            model,
            api_key,
            language: settings.language.clone(),
            timeout: Duration::from_secs(if settings.provider == ProviderKind::Ollama { 300 } else { 90 }),
        })
    }
}

/// Запрос к провайдеру в виде данных: удобно проверять без сети.
#[derive(Debug, Clone, PartialEq)]
pub struct WireRequest {
    pub url: String,
    pub headers: Vec<(&'static str, String)>,
    pub body: Value,
}

pub fn build_wire_request(config: &ProviderConfig, system: &str, user: &str) -> WireRequest {
    let base = config.base_url.as_str().trim_end_matches('/').to_string();
    let bearer = config.api_key.as_ref().map(|k| ("authorization", format!("Bearer {k}")));
    match config.wire {
        Wire::Ollama => WireRequest {
            url: format!("{base}/api/chat"),
            headers: bearer.into_iter().collect(),
            body: json!({
                "model": config.model,
                "stream": false,
                "format": output_schema(),
                "keep_alive": "30m",
                "options": { "temperature": TEMPERATURE },
                "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
            }),
        },
        Wire::OpenAi => WireRequest {
            url: format!("{base}/chat/completions"),
            headers: bearer.into_iter().collect(),
            body: json!({
                "model": config.model,
                "temperature": TEMPERATURE,
                "response_format": {"type": "json_object"},
                "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
            }),
        },
        Wire::Anthropic => WireRequest {
            url: format!("{base}/messages"),
            headers: config
                .api_key
                .iter()
                .map(|k| ("x-api-key", k.clone()))
                .chain([("anthropic-version", "2023-06-01".to_string())])
                .collect(),
            body: json!({
                "model": config.model,
                "max_tokens": 2000,
                "temperature": TEMPERATURE,
                "system": system,
                "messages": [{"role": "user", "content": user}],
            }),
        },
        Wire::Gemini => WireRequest {
            url: format!("{base}/models/{}:generateContent", config.model),
            headers: config.api_key.iter().map(|k| ("x-goog-api-key", k.clone())).collect(),
            body: json!({
                "systemInstruction": {"parts": [{"text": system}]},
                "contents": [{"role": "user", "parts": [{"text": user}]}],
                "generationConfig": {"responseMimeType": "application/json", "temperature": TEMPERATURE},
            }),
        },
    }
}

/// Текст ответа модели из тела ответа провайдера.
pub fn extract_text(wire: Wire, body: &Value) -> Result<String, AiError> {
    let text = match wire {
        Wire::Ollama => body.pointer("/message/content").and_then(Value::as_str).map(str::to_string),
        Wire::OpenAi => body.pointer("/choices/0/message/content").and_then(Value::as_str).map(str::to_string),
        Wire::Anthropic => body.get("content").and_then(Value::as_array).map(|blocks| {
            blocks.iter().filter_map(|b| b.get("text").and_then(Value::as_str)).collect::<Vec<_>>().join("")
        }),
        Wire::Gemini => body.pointer("/candidates/0/content/parts").and_then(Value::as_array).map(|parts| {
            parts.iter().filter_map(|p| p.get("text").and_then(Value::as_str)).collect::<Vec<_>>().join("")
        }),
    };
    text.filter(|t| !t.trim().is_empty()).ok_or_else(|| AiError::BadResponse("в ответе провайдера нет текста".into()))
}

fn snippet(body: &str, api_key: Option<&str>) -> String {
    let mut text: String = body.chars().take(MAX_ERROR_SNIPPET).collect();
    if let Some(key) = api_key.filter(|k| k.len() >= 8) {
        text = text.replace(key, "***");
    }
    text
}

pub fn classify_status(status: StatusCode, body: &str, api_key: Option<&str>) -> AiError {
    match status.as_u16() {
        401 | 403 => AiError::Auth(status.as_u16()),
        429 => AiError::RateLimited,
        code => AiError::Http { status: code, message: snippet(body, api_key) },
    }
}

pub struct HttpGenerator {
    config: ProviderConfig,
    client: reqwest::Client,
}

impl HttpGenerator {
    pub fn new(config: ProviderConfig) -> Result<Self, AiError> {
        // Редиректы отключены: заголовок с ключом не должен уйти на другой адрес.
        let client = reqwest::Client::builder()
            .timeout(config.timeout)
            .redirect(redirect::Policy::none())
            .build()
            .map_err(|e| AiError::Config(format!("не удалось создать HTTP-клиент: {e}")))?;
        Ok(Self { config, client })
    }
}

#[async_trait::async_trait]
impl ContentGenerator for HttpGenerator {
    async fn generate(&self, request: &ContentRequest) -> Result<GeneratedContent, AiError> {
        let mut request = request.clone();
        request.language = self.config.language.clone();
        let (system, user) = build_prompts(&request);
        let wire = build_wire_request(&self.config, &system, &user);
        let mut builder = self.client.post(&wire.url).json(&wire.body);
        for (name, value) in &wire.headers {
            builder = builder.header(*name, value);
        }
        let key = self.config.api_key.as_deref();
        let response = builder.send().await.map_err(|e| AiError::Transport(snippet(&e.without_url().to_string(), key)))?;
        let status = response.status();
        let text = response.text().await.map_err(|e| AiError::Transport(snippet(&e.without_url().to_string(), key)))?;
        if !status.is_success() {
            return Err(classify_status(status, &text, key));
        }
        let body: Value = serde_json::from_str(&text).map_err(|_| AiError::BadResponse("ответ провайдера не JSON".into()))?;
        parse_model_output(&extract_text(self.config.wire, &body)?)
    }

    fn name(&self) -> String {
        format!("{:?} / {}", self.config.kind, self.config.model).to_lowercase()
    }
}

#[cfg(test)]
mod tests;
