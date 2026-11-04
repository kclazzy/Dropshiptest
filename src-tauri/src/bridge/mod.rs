//! Локальный WebSocket-мост для расширения браузера: `ws://127.0.0.1:8765/ws`, JSON-RPC 2.0.
//! Безопасность: слушаем только loopback; принимаем только Origin `chrome-extension://` (страницы сайтов отсекаются);
//! первый пакет — `AUTH`/`PAIR` в течение 10 секунд; до авторизации ничего, кроме PING, не доступно.

pub mod pairing;
pub mod rpc;

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use async_trait::async_trait;
use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::State;
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::Router;
use serde_json::Value;
use sqlx::SqlitePool;
use tokio::net::TcpListener;
use tokio_util::sync::CancellationToken;

use rpc::{Dispatch, RpcError};

pub const DEFAULT_ADDR: &str = "127.0.0.1:8765";
const AUTH_TIMEOUT: Duration = Duration::from_secs(10);
const IDLE_TIMEOUT: Duration = Duration::from_secs(90);
const MAX_MESSAGE_BYTES: usize = 1024 * 1024;
const MAX_FAILED_AUTH: u32 = 5;

/// Дополнительные методы (заказы, обновление…) подключаются снаружи, чтобы мост не знал о каждой подсистеме.
#[async_trait]
pub trait ExtraMethods: Send + Sync {
    async fn handle(&self, method: &str, params: &Value, now: i64) -> Result<Value, RpcError>;
}

pub struct BridgeState {
    pub pool: SqlitePool,
    pub pairing: pairing::Pairing,
    pub extra: Option<Arc<dyn ExtraMethods>>,
    /// Сообщения ядро → расширение (готовый JSON-RPC запрос); получают только авторизованные сессии.
    outbound: tokio::sync::broadcast::Sender<String>,
    next_push_id: std::sync::atomic::AtomicI64,
}

impl BridgeState {
    pub fn new(pool: SqlitePool, extra: Option<Arc<dyn ExtraMethods>>) -> Arc<Self> {
        let (outbound, _) = tokio::sync::broadcast::channel(64);
        Arc::new(Self { pool, pairing: pairing::Pairing::default(), extra, outbound, next_push_id: std::sync::atomic::AtomicI64::new(1) })
    }

    /// Отправляет запрос всем подключённым расширениям; возвращает число получателей (0 — никого нет).
    pub fn push(&self, method: &str, params: Value) -> usize {
        let id = self.next_push_id.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let text = serde_json::json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params}).to_string();
        self.outbound.send(text).unwrap_or(0)
    }
}

impl crate::orders::OrderNotifier for BridgeState {
    fn notify(&self, method: &str, params: Value) -> usize {
        self.push(method, params)
    }
}

fn now_secs() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

/// Origin расширения Chrome; запросы без Origin (не из браузера) допустимы — они всё равно требуют токен.
pub fn origin_allowed(headers: &HeaderMap) -> bool {
    match headers.get(header::ORIGIN) {
        None => true,
        Some(value) => value.to_str().map(|o| o.starts_with("chrome-extension://")).unwrap_or(false),
    }
}

pub fn router(state: Arc<BridgeState>) -> Router {
    Router::new().route("/ws", get(ws_handler)).with_state(state)
}

async fn ws_handler(State(state): State<Arc<BridgeState>>, headers: HeaderMap, upgrade: WebSocketUpgrade) -> Response {
    if !origin_allowed(&headers) {
        return StatusCode::FORBIDDEN.into_response();
    }
    upgrade.max_message_size(MAX_MESSAGE_BYTES).on_upgrade(move |socket| session(socket, state))
}

async fn next_text(socket: &mut WebSocket, wait: Duration) -> Option<String> {
    loop {
        match tokio::time::timeout(wait, socket.recv()).await {
            Ok(Some(Ok(Message::Text(t)))) => return Some(t.as_str().to_string()),
            Ok(Some(Ok(Message::Ping(_) | Message::Pong(_)))) => continue,
            _ => return None,
        }
    }
}

async fn send(socket: &mut WebSocket, text: String) -> bool {
    socket.send(Message::Text(text.into())).await.is_ok()
}

/// Ответы расширения на наши запросы (`result`/`error` без `method`) в этой версии не нужны.
fn is_response(text: &str) -> bool {
    serde_json::from_str::<Value>(text).is_ok_and(|v| v.get("method").is_none() && (v.get("result").is_some() || v.get("error").is_some()))
}

async fn session(mut socket: WebSocket, state: Arc<BridgeState>) {
    let mut pushes = state.outbound.subscribe();
    let mut authed = false;
    let mut failures = 0u32;
    let deadline = tokio::time::Instant::now() + AUTH_TIMEOUT;
    loop {
        let wait = if authed { IDLE_TIMEOUT } else { deadline.saturating_duration_since(tokio::time::Instant::now()) };
        let text = tokio::select! {
            incoming = next_text(&mut socket, wait) => match incoming {
                Some(t) => t,
                None => break,
            },
            Ok(push) = pushes.recv(), if authed => {
                if !send(&mut socket, push).await {
                    break;
                }
                continue;
            }
        };
        if is_response(&text) {
            continue;
        }
        let request = match rpc::parse_request(&text) {
            Ok(r) => r,
            Err((id, e)) => {
                if !send(&mut socket, rpc::failure(id.as_ref(), &e)).await {
                    break;
                }
                continue;
            }
        };
        let now = now_secs();
        let outcome = if authed {
            Dispatch::Reply(rpc::dispatch_authenticated(&state, &request, now).await)
        } else {
            rpc::dispatch_unauthenticated(&state, &request, now).await
        };
        let reply = match outcome {
            Dispatch::Authenticated(result) => {
                authed = true;
                request.id.as_ref().map(|id| rpc::success(id, result))
            }
            Dispatch::Reply(Ok(result)) => request.id.as_ref().map(|id| rpc::success(id, result)),
            Dispatch::Reply(Err(e)) => {
                if !authed {
                    failures += 1;
                }
                Some(rpc::failure(request.id.as_ref(), &e))
            }
        };
        if let Some(text) = reply {
            if !send(&mut socket, text).await {
                break;
            }
        }
        if failures >= MAX_FAILED_AUTH {
            break;
        }
    }
    let _ = socket.send(Message::Close(None)).await;
}

/// Запускает сервер на готовом слушателе; завершается по `cancel`.
pub async fn serve(listener: TcpListener, state: Arc<BridgeState>, cancel: CancellationToken) -> std::io::Result<()> {
    axum::serve(listener, router(state)).with_graceful_shutdown(async move { cancel.cancelled().await }).await
}

/// Привязка только к loopback: адрес вне 127.0.0.1/::1 — ошибка конфигурации.
pub async fn bind_loopback(addr: &str) -> Result<TcpListener, crate::error::AppError> {
    let parsed: SocketAddr = addr.parse().map_err(|_| crate::error::AppError::Validation("некорректный адрес моста".into()))?;
    if !parsed.ip().is_loopback() {
        return Err(crate::error::AppError::Validation("мост может слушать только локальный адрес 127.0.0.1".into()));
    }
    Ok(TcpListener::bind(parsed).await?)
}

#[cfg(test)]
mod tests;
