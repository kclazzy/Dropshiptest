use std::sync::Arc;

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::Message as WsMessage;
use tokio_util::sync::CancellationToken;

use super::*;

type Client = tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

struct Harness {
    _dir: tempfile::TempDir,
    state: Arc<BridgeState>,
    addr: SocketAddr,
    cancel: CancellationToken,
}

async fn start() -> Harness {
    start_on("127.0.0.1:0").await
}

async fn start_on(bind: &str) -> Harness {
    let dir = tempfile::tempdir().unwrap();
    let pool = crate::db::init_pool(&dir.path().join("b.db")).await.unwrap();
    let state = BridgeState::new(pool, None);
    let listener = bind_loopback(bind).await.unwrap();
    let addr = listener.local_addr().unwrap();
    let cancel = CancellationToken::new();
    tokio::spawn(serve(listener, state.clone(), cancel.clone()));
    Harness { _dir: dir, state, addr, cancel }
}

async fn connect(h: &Harness, origin: Option<&str>) -> Result<Client, tokio_tungstenite::tungstenite::Error> {
    let mut req = format!("ws://{}/ws", h.addr).into_client_request().unwrap();
    if let Some(o) = origin {
        req.headers_mut().insert("Origin", o.parse().unwrap());
    }
    tokio_tungstenite::connect_async(req).await.map(|(c, _)| c)
}

async fn call(c: &mut Client, id: i64, method: &str, params: Value) -> Value {
    c.send(WsMessage::Text(json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params}).to_string().into())).await.unwrap();
    let msg = tokio::time::timeout(Duration::from_secs(5), c.next()).await.unwrap().unwrap().unwrap();
    serde_json::from_str(msg.to_text().unwrap()).unwrap()
}

fn product() -> Value {
    json!({"url": "https://s.example/p/1", "title": "Чехол", "variants": [{"sku": "X", "price_cents": 50000, "stock": 3}]})
}

#[tokio::test]
async fn pairing_code_is_exchanged_for_token_and_token_logs_in() {
    let h = start().await;
    let code = h.state.pairing.start(now_secs());
    let mut c = connect(&h, Some("chrome-extension://abc")).await.unwrap();
    let paired = call(&mut c, 1, "PAIR", json!({"code": code})).await;
    let token = paired["result"]["token"].as_str().unwrap().to_string();
    assert_eq!(token.len(), 64);
    let imported = call(&mut c, 2, "IMPORT_PRODUCT_PAYLOAD", product()).await;
    assert_eq!(imported["result"]["duplicate"], false);

    let mut again = connect(&h, None).await.unwrap();
    assert_eq!(call(&mut again, 1, "AUTH", json!({"token": token})).await["result"]["ok"], true);
    let dup = call(&mut again, 2, "IMPORT_PRODUCT_PAYLOAD", product()).await;
    assert_eq!(dup["result"]["duplicate"], true);
    h.cancel.cancel();
}

#[tokio::test]
async fn unauthenticated_calls_are_refused_and_wrong_tokens_rejected() {
    let h = start().await;
    let mut c = connect(&h, None).await.unwrap();
    assert_eq!(call(&mut c, 1, "IMPORT_PRODUCT_PAYLOAD", product()).await["error"]["code"], rpc::UNAUTHORIZED);
    assert_eq!(call(&mut c, 2, "AUTH", json!({"token": "nope"})).await["error"]["code"], rpc::UNAUTHORIZED);
    assert_eq!(call(&mut c, 3, "PING", json!({})).await["result"]["pong"], true);
    let products: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM products").fetch_one(&h.state.pool).await.unwrap();
    assert_eq!(products, 0);
}

#[tokio::test]
async fn pairing_code_is_single_use_expires_and_burns_after_wrong_guesses() {
    let p = pairing::Pairing::default();
    let code = p.start(0);
    assert!(p.redeem(&code.to_lowercase(), 10));
    assert!(!p.redeem(&code, 11), "одноразовый");
    let code = p.start(0);
    assert!(!p.redeem(&code, 121), "истёк");
    let code = p.start(0);
    for _ in 0..5 {
        assert!(!p.redeem("WRONG000", 1));
    }
    assert!(!p.redeem(&code, 2), "сгорел после 5 неверных попыток");
}

#[tokio::test]
async fn new_token_revokes_the_old_one() {
    let h = start().await;
    let first = pairing::issue_token(&h.state.pool, 1).await.unwrap();
    let second = pairing::issue_token(&h.state.pool, 2).await.unwrap();
    assert!(!pairing::verify_token(&h.state.pool, &first).await.unwrap());
    assert!(pairing::verify_token(&h.state.pool, &second).await.unwrap());
    assert!(pairing::revoke_token(&h.state.pool).await.unwrap());
    assert!(!pairing::verify_token(&h.state.pool, &second).await.unwrap());
}

#[tokio::test]
async fn website_origins_cannot_connect() {
    let h = start().await;
    assert!(connect(&h, Some("https://evil.example")).await.is_err());
    assert!(connect(&h, Some("chrome-extension://abcdef")).await.is_ok());
}

#[tokio::test]
async fn malformed_requests_get_protocol_errors() {
    let h = start().await;
    let mut c = connect(&h, None).await.unwrap();
    c.send(WsMessage::Text("{not json".into())).await.unwrap();
    let reply: Value = serde_json::from_str(c.next().await.unwrap().unwrap().to_text().unwrap()).unwrap();
    assert_eq!(reply["error"]["code"], rpc::PARSE_ERROR);
    c.send(WsMessage::Text(json!({"id": 7, "method": "PING"}).to_string().into())).await.unwrap();
    let reply: Value = serde_json::from_str(c.next().await.unwrap().unwrap().to_text().unwrap()).unwrap();
    assert_eq!((reply["error"]["code"].clone(), reply["id"].clone()), (json!(rpc::INVALID_REQUEST), json!(7)));
}

#[tokio::test]
async fn authenticated_session_reports_bad_params_and_unknown_methods() {
    let h = start().await;
    let token = pairing::issue_token(&h.state.pool, 1).await.unwrap();
    let mut c = connect(&h, None).await.unwrap();
    call(&mut c, 1, "AUTH", json!({"token": token})).await;
    let bad = call(&mut c, 2, "IMPORT_PRODUCT_PAYLOAD", json!({"url": "javascript:1"})).await;
    assert_eq!(bad["error"]["code"], rpc::INVALID_PARAMS);
    assert!(bad["error"]["message"].as_str().unwrap().contains("ссылк"));
    assert_eq!(call(&mut c, 3, "NOPE", json!({})).await["error"]["code"], rpc::METHOD_NOT_FOUND);
}

#[tokio::test]
async fn repeated_failed_logins_close_the_connection() {
    let h = start().await;
    let mut c = connect(&h, None).await.unwrap();
    for i in 0..5 {
        call(&mut c, i, "AUTH", json!({"token": "bad"})).await;
    }
    let end = tokio::time::timeout(Duration::from_secs(5), c.next()).await.unwrap();
    assert!(matches!(end, None | Some(Err(_)) | Some(Ok(WsMessage::Close(_)))));
}

#[tokio::test]
async fn pushes_reach_only_authenticated_sessions_and_replies_are_ignored() {
    let h = start().await;
    let token = pairing::issue_token(&h.state.pool, 1).await.unwrap();
    let mut anon = connect(&h, None).await.unwrap();
    let mut authed = connect(&h, None).await.unwrap();
    call(&mut authed, 1, "AUTH", json!({"token": token})).await;
    assert_eq!(h.state.push("EXECUTE_AUTO_ORDER", json!({"order_id": 5})), 2, "оба подписаны на канал");
    let msg = tokio::time::timeout(Duration::from_secs(5), authed.next()).await.unwrap().unwrap().unwrap();
    let v: Value = serde_json::from_str(msg.to_text().unwrap()).unwrap();
    assert_eq!((v["method"].as_str(), v["params"]["order_id"].as_i64()), (Some("EXECUTE_AUTO_ORDER"), Some(5)));
    // ответ расширения на push не порождает ошибку «не запрос»
    authed.send(WsMessage::Text(json!({"jsonrpc": "2.0", "id": v["id"], "result": {"opened": true}}).to_string().into())).await.unwrap();
    assert_eq!(call(&mut authed, 2, "PING", json!({})).await["result"]["pong"], true);
    // неавторизованная сессия ничего не получила
    assert_eq!(call(&mut anon, 1, "PING", json!({})).await["result"]["pong"], true);
}

#[tokio::test]
async fn bridge_refuses_non_loopback_addresses() {
    assert!(bind_loopback("0.0.0.0:0").await.is_err());
    assert!(bind_loopback("192.168.1.5:8765").await.is_err());
    assert!(bind_loopback("garbage").await.is_err());
}

/// Ручная сквозная проверка с настоящим клиентом расширения (Node): `E2E_PORT=… E2E_CODE_FILE=… cargo test -- --ignored`.
#[tokio::test]
#[ignore]
async fn serve_for_extension_e2e() {
    let (Ok(port), Ok(code_file)) = (std::env::var("E2E_PORT"), std::env::var("E2E_CODE_FILE")) else { return };
    let h = start_on(&format!("127.0.0.1:{port}")).await;
    std::fs::write(code_file, h.state.pairing.start(now_secs())).unwrap();
    tokio::time::sleep(Duration::from_secs(20)).await;
    let products: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM products").fetch_one(&h.state.pool).await.unwrap();
    assert_eq!(products, 1, "расширение должно было импортировать ровно один товар");
}
