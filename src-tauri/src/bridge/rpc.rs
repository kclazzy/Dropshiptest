//! JSON-RPC 2.0: разбор запроса, маршрутизация методов, коды ошибок.
//! Состояние соединения (`authed`) живёт в сессии; здесь только решение «что делать с этим вызовом».

use std::sync::Arc;

use serde::Deserialize;
use serde_json::{json, Value};

use super::{pairing, BridgeState};
use crate::error::AppError;
use crate::import;

pub const PARSE_ERROR: i64 = -32700;
pub const INVALID_REQUEST: i64 = -32600;
pub const METHOD_NOT_FOUND: i64 = -32601;
pub const INVALID_PARAMS: i64 = -32602;
pub const INTERNAL_ERROR: i64 = -32603;
pub const UNAUTHORIZED: i64 = -32001;

#[derive(Debug, Clone, PartialEq)]
pub struct RpcError {
    pub code: i64,
    pub message: String,
}

impl RpcError {
    pub fn new(code: i64, message: impl Into<String>) -> Self {
        Self { code, message: message.into() }
    }
}

impl From<AppError> for RpcError {
    fn from(e: AppError) -> Self {
        let code = if matches!(e, AppError::Validation(_) | AppError::Pricing(_)) { INVALID_PARAMS } else { INTERNAL_ERROR };
        tracing::warn!(error = %e, "ошибка вызова моста");
        Self::new(code, e.to_user_friendly_ru())
    }
}

#[derive(Debug, Deserialize)]
struct RawRequest {
    jsonrpc: Option<String>,
    id: Option<Value>,
    method: Option<String>,
    #[serde(default)]
    params: Value,
}

#[derive(Debug, PartialEq)]
pub struct Request {
    pub id: Option<Value>,
    pub method: String,
    pub params: Value,
}

pub fn parse_request(text: &str) -> Result<Request, (Option<Value>, RpcError)> {
    let raw: RawRequest = serde_json::from_str(text).map_err(|_| (None, RpcError::new(PARSE_ERROR, "некорректный JSON")))?;
    let valid_id = matches!(raw.id, None | Some(Value::Number(_)) | Some(Value::String(_)));
    match (raw.jsonrpc.as_deref(), raw.method, valid_id) {
        (Some("2.0"), Some(method), true) if !method.is_empty() && method.len() <= 64 => Ok(Request { id: raw.id, method, params: raw.params }),
        _ => Err((raw.id.filter(|_| valid_id), RpcError::new(INVALID_REQUEST, "это не запрос JSON-RPC 2.0"))),
    }
}

pub fn success(id: &Value, result: Value) -> String {
    json!({"jsonrpc": "2.0", "id": id, "result": result}).to_string()
}

pub fn failure(id: Option<&Value>, error: &RpcError) -> String {
    json!({"jsonrpc": "2.0", "id": id.cloned().unwrap_or(Value::Null), "error": {"code": error.code, "message": error.message}}).to_string()
}

fn string_param<'a>(params: &'a Value, key: &str) -> Result<&'a str, RpcError> {
    params.get(key).and_then(Value::as_str).filter(|s| !s.is_empty() && s.len() <= 256).ok_or_else(|| RpcError::new(INVALID_PARAMS, format!("нужен параметр {key}")))
}

/// Результат `AUTH`/`PAIR` для сессии.
pub enum Dispatch {
    Reply(Result<Value, RpcError>),
    /// Ответ + соединение стало авторизованным.
    Authenticated(Value),
}

/// Методы, доступные до авторизации: только привязка и вход.
pub async fn dispatch_unauthenticated(state: &Arc<BridgeState>, req: &Request, now: i64) -> Dispatch {
    match req.method.as_str() {
        "AUTH" => match string_param(&req.params, "token") {
            Ok(token) => match pairing::verify_token(&state.pool, token).await {
                Ok(true) => Dispatch::Authenticated(json!({"ok": true})),
                Ok(false) => Dispatch::Reply(Err(RpcError::new(UNAUTHORIZED, "неверный токен: привяжите расширение заново"))),
                Err(e) => Dispatch::Reply(Err(e.into())),
            },
            Err(e) => Dispatch::Reply(Err(e)),
        },
        "PAIR" => match string_param(&req.params, "code") {
            Ok(code) if state.pairing.redeem(code, now) => match pairing::issue_token(&state.pool, now).await {
                Ok(token) => Dispatch::Authenticated(json!({"token": token})),
                Err(e) => Dispatch::Reply(Err(e.into())),
            },
            Ok(_) => Dispatch::Reply(Err(RpcError::new(UNAUTHORIZED, "код неверный или устарел: получите новый в приложении"))),
            Err(e) => Dispatch::Reply(Err(e)),
        },
        "PING" => Dispatch::Reply(Ok(json!({"pong": true}))),
        _ => Dispatch::Reply(Err(RpcError::new(UNAUTHORIZED, "сначала выполните AUTH"))),
    }
}

pub async fn dispatch_authenticated(state: &Arc<BridgeState>, req: &Request, now: i64) -> Result<Value, RpcError> {
    match req.method.as_str() {
        "PING" => Ok(json!({"pong": true})),
        "AUTH" | "PAIR" => Ok(json!({"ok": true})),
        "IMPORT_PRODUCT_PAYLOAD" => {
            let payload = import::parse_import_payload(&req.params)?;
            let out = import::import_product(&state.pool, &payload, now).await?;
            Ok(json!({"product_id": out.product_id, "duplicate": out.duplicate, "variants": out.variants}))
        }
        other => match &state.extra {
            Some(handler) => handler.handle(other, &req.params, now).await,
            None => Err(RpcError::new(METHOD_NOT_FOUND, format!("метод {other} не поддерживается"))),
        },
    }
}
