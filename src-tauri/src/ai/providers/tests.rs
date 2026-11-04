use super::*;
use crate::ai::content::Target;
use crate::testutil::MockServer;

const MODEL_JSON: &str = r#"{"seo_title":"Наушники Brand X1","avito_title_50chars":"Наушники Brand X1 беспроводные","description_formatted":"Описание","bullet_points":["Bluetooth 5.3"],"extracted_attributes":{"Цвет":"Чёрный"},"search_tags":["наушники"]}"#;

fn request() -> ContentRequest {
    ContentRequest { target: Target::Avito, title: "Наушники".into(), description: None, attributes: vec![], language: "ru".into() }
}

fn config(kind: ProviderKind, base: &str, key: Option<&str>) -> ProviderConfig {
    ProviderConfig::build(
        &AiSettings { provider: kind, model: Some("m1".into()), base_url: Some(base.into()), language: "ru".into() },
        key.map(str::to_string),
    )
    .unwrap()
}

fn generator(kind: ProviderKind, base: &str, key: Option<&str>) -> HttpGenerator {
    HttpGenerator::new(config(kind, base, key)).unwrap()
}

fn wrap(wire: Wire, text: &str) -> String {
    match wire {
        Wire::Ollama => json!({"message": {"content": text}}),
        Wire::OpenAi => json!({"choices": [{"message": {"content": text}}]}),
        Wire::Anthropic => json!({"content": [{"type": "text", "text": text}]}),
        Wire::Gemini => json!({"candidates": [{"content": {"parts": [{"text": text}]}}]}),
    }
    .to_string()
}

#[tokio::test]
async fn every_wire_sends_the_right_request_and_parses_the_answer() {
    let cases = [
        (ProviderKind::Ollama, Wire::Ollama, "/api/chat", None),
        (ProviderKind::Openai, Wire::OpenAi, "/chat/completions", Some(("authorization", "Bearer sk-test-12345678"))),
        (ProviderKind::Anthropic, Wire::Anthropic, "/messages", Some(("x-api-key", "sk-test-12345678"))),
        (ProviderKind::Gemini, Wire::Gemini, "/models/m1:generateContent", Some(("x-goog-api-key", "sk-test-12345678"))),
    ];
    for (kind, wire, path, header) in cases {
        let server = MockServer::start().await;
        server.reply(path, 200, &wrap(wire, MODEL_JSON));
        let key = header.map(|_| "sk-test-12345678");
        let out = generator(kind, &server.base_url, key).generate(&request()).await.unwrap();
        assert_eq!(out.avito_title, "Наушники Brand X1 беспроводные");
        assert_eq!(out.extracted_attributes["Цвет"], "Чёрный");
        let sent = server.requests_to(path);
        assert_eq!(sent.len(), 1, "{kind:?}");
        assert_eq!(sent[0].method, "POST");
        if let Some((name, value)) = header {
            assert_eq!(sent[0].headers.get(name).map(String::as_str), Some(value), "{kind:?}");
        } else {
            assert!(!sent[0].headers.contains_key("authorization"));
        }
        let text = sent[0].body.clone();
        assert!(text.contains("avito_title_50chars") && text.contains("Наушники"), "{kind:?}");
    }
}

#[tokio::test]
async fn ollama_requests_structured_output_and_keeps_the_model_loaded() {
    let server = MockServer::start().await;
    server.reply("/api/chat", 200, &wrap(Wire::Ollama, MODEL_JSON));
    generator(ProviderKind::Ollama, &server.base_url, None).generate(&request()).await.unwrap();
    let body = server.requests_to("/api/chat")[0].json();
    assert_eq!(body["stream"], false);
    assert_eq!(body["keep_alive"], "30m");
    assert_eq!(body["format"]["required"][0], "seo_title");
}

#[tokio::test]
async fn http_errors_are_classified_and_never_leak_the_key() {
    let key = "sk-secret-0123456789";
    let cases: [(u16, &str, bool); 5] =
        [(401, "bad key", false), (403, "forbidden", false), (429, "slow down", true), (500, "oops", true), (400, "bad request", false)];
    for (status, body, retryable) in cases {
        let server = MockServer::start().await;
        server.reply("/chat/completions", status, &format!("{body} {key}"));
        let err = generator(ProviderKind::Openai, &server.base_url, Some(key)).generate(&request()).await.unwrap_err();
        assert_eq!(err.retryable(), retryable, "{status}: {err:?}");
        assert!(!format!("{err:?} {err} {}", err.to_user_friendly_ru()).contains(key), "{status}");
    }
}

#[tokio::test]
async fn garbage_answers_are_bad_response_and_retryable() {
    for reply in ["not json", &json!({"choices": []}).to_string(), &wrap(Wire::OpenAi, "no object here"), &wrap(Wire::OpenAi, "{\"seo_title\":\"A\"}")] {
        let server = MockServer::start().await;
        server.reply("/chat/completions", 200, reply);
        let err = generator(ProviderKind::Openai, &server.base_url, Some("sk-test-12345678")).generate(&request()).await.unwrap_err();
        assert!(matches!(err, AiError::BadResponse(_)), "{reply}: {err:?}");
        assert!(err.retryable());
    }
}

#[tokio::test]
async fn redirects_are_not_followed_so_the_key_cannot_travel() {
    let target = MockServer::start().await;
    let server = MockServer::start().await;
    server.redirect("/chat/completions", &format!("{}/steal", target.base_url));
    let err = generator(ProviderKind::Openai, &server.base_url, Some("sk-test-12345678")).generate(&request()).await.unwrap_err();
    assert!(matches!(err, AiError::Http { status: 302, .. }), "{err:?}");
    assert!(target.requests().is_empty());
}

#[tokio::test]
async fn unreachable_server_is_a_retryable_transport_error() {
    let err = generator(ProviderKind::Ollama, "http://127.0.0.1:1", None).generate(&request()).await.unwrap_err();
    assert!(matches!(err, AiError::Transport(_)) && err.retryable(), "{err:?}");
}

#[test]
fn settings_are_validated_on_entry() {
    let s = |kind, model: Option<&str>, base: Option<&str>| AiSettings {
        provider: kind, model: model.map(str::to_string), base_url: base.map(str::to_string), language: "ru".into(),
    };
    // Нужен ключ.
    assert!(matches!(ProviderConfig::build(&s(ProviderKind::Openai, None, None), None), Err(AiError::Config(_))));
    assert!(matches!(ProviderConfig::build(&s(ProviderKind::Openai, None, None), Some("  ".into())), Err(AiError::Config(_))));
    // Для custom обязательны адрес и модель.
    assert!(ProviderConfig::build(&s(ProviderKind::Custom, None, None), None).is_err());
    assert!(ProviderConfig::build(&s(ProviderKind::Custom, Some("m"), Some("not a url")), None).is_err());
    assert!(ProviderConfig::build(&s(ProviderKind::Custom, Some("m"), Some("ftp://x")), None).is_err());
    assert!(ProviderConfig::build(&s(ProviderKind::Custom, Some("m"), Some("http://127.0.0.1:1234/v1")), None).is_ok());
    // Ключ по голому http на чужой хост запрещён, на localhost — можно.
    assert!(ProviderConfig::build(&s(ProviderKind::Custom, Some("m"), Some("http://example.com/v1")), Some("sk-12345678".into())).is_err());
    assert!(ProviderConfig::build(&s(ProviderKind::Custom, Some("m"), Some("http://localhost:8080/v1")), Some("sk-12345678".into())).is_ok());
    // Значения по умолчанию.
    let ollama = ProviderConfig::build(&AiSettings::default(), None).unwrap();
    assert_eq!((ollama.model.as_str(), ollama.base_url.as_str()), (DEFAULT_LOCAL_MODEL, "http://127.0.0.1:11434/"));
    // Ключ не попадает в Debug.
    let cfg = ProviderConfig::build(&s(ProviderKind::Openai, None, None), Some("sk-very-secret".into())).unwrap();
    assert!(!format!("{cfg:?}").contains("sk-very-secret"));
}

#[test]
fn presets_cover_all_providers_with_instructions_links() {
    let all = presets();
    assert_eq!(all.len(), 10);
    for p in &all {
        if p.needs_key {
            assert!(p.key_url.starts_with("https://") && !p.default_model.is_empty() && p.base_url.starts_with("https://"), "{:?}", p.kind);
        }
    }
    assert_eq!(preset(ProviderKind::Anthropic).wire, Wire::Anthropic);
}
