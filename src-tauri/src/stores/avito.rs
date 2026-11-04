//! Avito API: токен (`/token`, client_credentials), остатки (`/stock-management/1/stocks`), цена (`/core/v1/items/{id}/update_price`).
//! ⚠ Не сверено с официальной документацией (см. `stores/mod.rs`).

use std::time::{Duration, Instant};

use reqwest::{StatusCode, Url};
use serde_json::{json, Value};
use tokio::sync::Mutex;

use super::{
    classify, http_client, join, limiter, rubles_ceil, ItemResult, StoreAdapter, StoreError, StoreErrorKind, StoreResult,
    VariantUpdate,
};

const BATCH: usize = 200;
/// Токен обновляется заранее, чтобы запрос не упёрся в истёкший.
const TOKEN_MARGIN: Duration = Duration::from_secs(60);

struct Token {
    value: String,
    valid_until: Instant,
}

pub struct AvitoAdapter {
    client: reqwest::Client,
    base: Url,
    client_id: String,
    client_secret: String,
    limiter: governor::DefaultDirectRateLimiter,
    token: Mutex<Option<Token>>,
}

impl AvitoAdapter {
    pub fn new(base: Url, client_id: String, client_secret: String, rps: f64) -> StoreResult<Self> {
        Ok(Self { client: http_client()?, base, client_id, client_secret, limiter: limiter(rps)?, token: Mutex::new(None) })
    }

    async fn fetch_token(&self) -> StoreResult<Token> {
        self.limiter.until_ready().await;
        let response = self
            .client
            .post(join(&self.base, "/token"))
            .form(&[("grant_type", "client_credentials"), ("client_id", &self.client_id), ("client_secret", &self.client_secret)])
            .send()
            .await
            .map_err(|e| StoreError::new(StoreErrorKind::Transport, e.without_url().to_string()))?;
        let status = response.status();
        let headers = response.headers().clone();
        let text = response.text().await.map_err(|e| StoreError::new(StoreErrorKind::Transport, e.without_url().to_string()))?;
        if !status.is_success() {
            // Для токена любой 4xx — неверные ключи.
            let mut error = classify(status, &headers, &text);
            if status.is_client_error() && status != StatusCode::TOO_MANY_REQUESTS {
                error.kind = StoreErrorKind::Auth;
            }
            return Err(error);
        }
        let body: Value = serde_json::from_str(&text).map_err(|_| StoreError::new(StoreErrorKind::Transport, "ответ Avito не является JSON"))?;
        let value = body
            .get("access_token")
            .and_then(Value::as_str)
            .filter(|t| !t.is_empty())
            .ok_or_else(|| StoreError::new(StoreErrorKind::Transport, "Avito не вернул токен"))?
            .to_string();
        let ttl = body.get("expires_in").and_then(Value::as_u64).unwrap_or(0);
        Ok(Token { value, valid_until: Instant::now() + Duration::from_secs(ttl).saturating_sub(TOKEN_MARGIN) })
    }

    async fn bearer(&self) -> StoreResult<String> {
        let mut guard = self.token.lock().await;
        if let Some(token) = guard.as_ref().filter(|t| t.valid_until > Instant::now()) {
            return Ok(token.value.clone());
        }
        let fresh = self.fetch_token().await?;
        let value = fresh.value.clone();
        *guard = Some(fresh);
        Ok(value)
    }

    /// Вызов API; при 401 токен сбрасывается и запрос повторяется один раз.
    async fn call(&self, method: reqwest::Method, path: &str, body: &Value) -> StoreResult<Value> {
        for attempt in 0..2 {
            let token = self.bearer().await?;
            self.limiter.until_ready().await;
            let response = self
                .client
                .request(method.clone(), join(&self.base, path))
                .bearer_auth(token)
                .json(body)
                .send()
                .await
                .map_err(|e| StoreError::new(StoreErrorKind::Transport, e.without_url().to_string()))?;
            let status = response.status();
            let headers = response.headers().clone();
            let text = response.text().await.map_err(|e| StoreError::new(StoreErrorKind::Transport, e.without_url().to_string()))?;
            if status == StatusCode::UNAUTHORIZED && attempt == 0 {
                *self.token.lock().await = None;
                continue;
            }
            if !status.is_success() {
                return Err(classify(status, &headers, &text));
            }
            return Ok(serde_json::from_str(&text).unwrap_or(Value::Null));
        }
        Err(StoreError::new(StoreErrorKind::Auth, "ключи не приняты"))
    }
}

#[async_trait::async_trait]
impl StoreAdapter for AvitoAdapter {
    fn name(&self) -> String {
        "avito".into()
    }

    async fn update_variants(&self, updates: &[VariantUpdate]) -> StoreResult<Vec<ItemResult>> {
        let mut results = Vec::with_capacity(updates.len());
        let mut valid: Vec<(&VariantUpdate, i64)> = Vec::new();
        for update in updates {
            match update.external_id.trim().parse::<i64>().ok().filter(|id| *id > 0) {
                Some(id) => valid.push((update, id)),
                None => results.push(ItemResult {
                    variant_id: update.variant_id,
                    ok: false,
                    price_sent: false,
                    stock_sent: false,
                    error: "вариант не привязан к объявлению Avito (item_id — число)".into(),
                }),
            }
        }

        for chunk in valid.chunks(BATCH) {
            let stocks: Vec<Value> = chunk.iter().map(|(u, id)| json!({"item_id": id, "quantity": u.stock.max(0)})).collect();
            let response = self.call(reqwest::Method::PUT, "/stock-management/1/stocks", &json!({ "stocks": stocks })).await?;
            let rows = response.get("stocks").and_then(Value::as_array).cloned().unwrap_or_default();

            for (update, id) in chunk {
                let row = rows.iter().find(|r| r.get("item_id").and_then(Value::as_i64) == Some(*id));
                let stock_ok = row.and_then(|r| r.get("success")).and_then(Value::as_bool).unwrap_or(false);
                let mut problems = Vec::new();
                if !stock_ok {
                    let reason = row
                        .and_then(|r| r.get("errors"))
                        .and_then(Value::as_array)
                        .map(|e| e.iter().filter_map(Value::as_str).collect::<Vec<_>>().join("; "))
                        .filter(|s| !s.is_empty())
                        .unwrap_or_else(|| "Avito не подтвердил остаток".to_string());
                    problems.push(format!("остаток: {reason}"));
                }
                let mut price_sent = false;
                if update.retail_minor > 0 {
                    let body = json!({ "price": rubles_ceil(update.retail_minor) });
                    match self.call(reqwest::Method::POST, &format!("/core/v1/items/{id}/update_price"), &body).await {
                        Ok(_) => price_sent = true,
                        // Сбой одного объявления не должен ронять остальные; ключи/сеть/лимиты — наоборот, останавливают всё.
                        Err(e) if e.kind == StoreErrorKind::Rejected => problems.push(format!("цена: {}", e.message)),
                        Err(e) => return Err(e),
                    }
                }
                results.push(ItemResult {
                    variant_id: update.variant_id,
                    ok: problems.is_empty(),
                    price_sent,
                    stock_sent: stock_ok,
                    error: problems.join("; "),
                });
            }
        }
        Ok(results)
    }
}
