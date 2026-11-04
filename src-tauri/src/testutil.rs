//! Тестовый HTTP-сервер: записывает запросы и отдаёт заранее заданные ответы (по пути; по умолчанию 200 `{}`).

use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use axum::body::Body;
use axum::extract::State;
use axum::http::{Request, StatusCode};
use axum::response::Response;
use axum::Router;

#[derive(Debug, Clone)]
pub struct Recorded {
    pub method: String,
    pub path: String,
    pub headers: HashMap<String, String>,
    pub body: String,
}

impl Recorded {
    pub fn json(&self) -> serde_json::Value {
        serde_json::from_str(&self.body).unwrap_or(serde_json::Value::Null)
    }
}

#[derive(Default)]
struct Inner {
    recorded: Vec<Recorded>,
    replies: HashMap<String, VecDeque<(u16, String, Option<String>)>>,
}

#[derive(Clone)]
pub struct MockServer {
    pub base_url: String,
    inner: Arc<Mutex<Inner>>,
}

async fn handle(State(inner): State<Arc<Mutex<Inner>>>, request: Request<Body>) -> Response {
    let (parts, body) = request.into_parts();
    let bytes = axum::body::to_bytes(body, 1 << 20).await.unwrap_or_default();
    let headers = parts
        .headers
        .iter()
        .map(|(k, v)| (k.as_str().to_string(), v.to_str().unwrap_or("").to_string()))
        .collect();
    let path = parts.uri.path().to_string();
    let reply = {
        let mut guard = inner.lock().unwrap();
        guard.recorded.push(Recorded {
            method: parts.method.to_string(),
            path: path.clone(),
            headers,
            body: String::from_utf8_lossy(&bytes).to_string(),
        });
        guard.replies.get_mut(&path).and_then(|q| if q.len() > 1 { q.pop_front() } else { q.front().cloned() })
    };
    let (status, body, location) = reply.unwrap_or((200, "{}".to_string(), None));
    let mut builder = Response::builder().status(StatusCode::from_u16(status).unwrap_or(StatusCode::OK));
    if let Some(location) = location {
        builder = builder.header("location", location);
    }
    builder.header("content-type", "application/json").body(Body::from(body)).unwrap_or_default()
}

impl MockServer {
    pub async fn start() -> Self {
        let inner = Arc::new(Mutex::new(Inner::default()));
        let app = Router::new().fallback(handle).with_state(inner.clone());
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let base_url = format!("http://{}", listener.local_addr().unwrap());
        tokio::spawn(async move {
            let _ = axum::serve(listener, app).await;
        });
        Self { base_url, inner }
    }

    /// Ответы по пути выдаются по очереди; последний повторяется.
    pub fn reply(&self, path: &str, status: u16, body: &str) {
        self.inner.lock().unwrap().replies.entry(path.to_string()).or_default().push_back((status, body.to_string(), None));
    }

    pub fn redirect(&self, path: &str, to: &str) {
        self.inner.lock().unwrap().replies.entry(path.to_string()).or_default().push_back((302, String::new(), Some(to.to_string())));
    }

    pub fn requests(&self) -> Vec<Recorded> {
        self.inner.lock().unwrap().recorded.clone()
    }

    pub fn requests_to(&self, path: &str) -> Vec<Recorded> {
        self.requests().into_iter().filter(|r| r.path == path).collect()
    }
}

/// Магазин + товар с одним вариантом для тестов. Возвращает (store_id, product_id, variant_id).
pub async fn insert_product(pool: &sqlx::SqlitePool, platform: &str, sha: &str) -> (i64, i64, i64) {
    let store: i64 = match sqlx::query_scalar("SELECT id FROM stores WHERE platform_type = ?").bind(platform).fetch_optional(pool).await.unwrap() {
        Some(id) => id,
        None => sqlx::query_scalar(
            "INSERT INTO stores(name, platform_type, api_url, encrypted_credentials, created_at) VALUES ('Магазин', ?, 'http://x', x'00', 0) RETURNING id",
        )
        .bind(platform)
        .fetch_one(pool)
        .await
        .unwrap(),
    };
    let product: i64 = sqlx::query_scalar(
        "INSERT INTO products(store_id, external_offer_id, supplier_url, supplier_product_id, url_sha256, raw_title, raw_description, images_json) \
         VALUES (?, ?, 'https://supplier.example/item', 'P1', ?, 'Наушники', 'Bluetooth 5.3', '[]') RETURNING id",
    )
    .bind(store)
    .bind(format!("offer-{sha}"))
    .bind(sha)
    .fetch_one(pool)
    .await
    .unwrap();
    let variant: i64 = sqlx::query_scalar(
        "INSERT INTO product_variants(product_id, supplier_sku, attributes_json, supplier_price_minor, retail_price_minor, min_price_minor, old_price_minor, supplier_stock, state_hash, updated_at) \
         VALUES (?, 'SKU-1', '{\"color\":\"Black\"}', 10000, 14499, 12000, 20000, 10, 'h', 0) RETURNING id",
    )
    .bind(product)
    .fetch_one(pool)
    .await
    .unwrap();
    (store, product, variant)
}
