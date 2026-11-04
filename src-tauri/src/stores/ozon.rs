//! Ozon Seller API: остатки (`/v2/products/stocks`) и цены (`/v1/product/import/prices`).
//! ⚠ Не сверено с официальной документацией (см. `stores/mod.rs`).

use reqwest::Url;
use serde_json::{json, Value};

use super::{
    classify, http_client, join, limiter, rubles_ceil, ItemResult, StoreAdapter, StoreError, StoreErrorKind, StoreResult,
    VariantUpdate,
};

/// Лимит Ozon на размер одного запроса.
const BATCH: usize = 100;

pub struct OzonAdapter {
    client: reqwest::Client,
    base: Url,
    client_id: String,
    api_key: String,
    warehouse_id: i64,
    limiter: governor::DefaultDirectRateLimiter,
}

impl OzonAdapter {
    pub fn new(base: Url, client_id: String, api_key: String, warehouse_id: i64, rps: f64) -> StoreResult<Self> {
        Ok(Self { client: http_client()?, base, client_id, api_key, warehouse_id, limiter: limiter(rps)? })
    }

    async fn post(&self, path: &str, body: &Value) -> StoreResult<Value> {
        self.limiter.until_ready().await;
        let response = self
            .client
            .post(join(&self.base, path))
            .header("Client-Id", &self.client_id)
            .header("Api-Key", &self.api_key)
            .json(body)
            .send()
            .await
            .map_err(|e| StoreError::new(StoreErrorKind::Transport, e.without_url().to_string()))?;
        let status = response.status();
        let headers = response.headers().clone();
        let text = response.text().await.map_err(|e| StoreError::new(StoreErrorKind::Transport, e.without_url().to_string()))?;
        if !status.is_success() {
            return Err(classify(status, &headers, &text));
        }
        serde_json::from_str(&text).map_err(|_| StoreError::new(StoreErrorKind::Transport, "ответ Ozon не является JSON"))
    }
}

/// Разбор `{"result":[{"offer_id","updated","errors":[{"code","message"}]}]}` -> (offer_id, ошибка или пусто).
fn parse_result(response: &Value) -> Vec<(String, String)> {
    response
        .get("result")
        .and_then(Value::as_array)
        .map(|rows| {
            rows.iter()
                .filter_map(|row| {
                    let offer = row.get("offer_id")?.as_str()?.to_string();
                    let errors: Vec<String> = row
                        .get("errors")
                        .and_then(Value::as_array)
                        .map(|list| {
                            list.iter()
                                .filter_map(|e| e.get("message").or_else(|| e.get("code")).and_then(Value::as_str).map(str::to_string))
                                .collect()
                        })
                        .unwrap_or_default();
                    let updated = row.get("updated").and_then(Value::as_bool).unwrap_or(false);
                    let problem = if !errors.is_empty() {
                        errors.join("; ")
                    } else if !updated {
                        "Ozon не применил изменение".to_string()
                    } else {
                        String::new()
                    };
                    Some((offer, problem))
                })
                .collect()
        })
        .unwrap_or_default()
}

#[async_trait::async_trait]
impl StoreAdapter for OzonAdapter {
    fn name(&self) -> String {
        "ozon".into()
    }

    async fn update_variants(&self, updates: &[VariantUpdate]) -> StoreResult<Vec<ItemResult>> {
        let mut results: Vec<ItemResult> = Vec::with_capacity(updates.len());
        let linked: Vec<&VariantUpdate> = updates.iter().filter(|u| !u.external_id.trim().is_empty()).collect();
        for update in updates.iter().filter(|u| u.external_id.trim().is_empty()) {
            results.push(ItemResult {
                variant_id: update.variant_id,
                ok: false,
                price_sent: false,
                stock_sent: false,
                error: "вариант не привязан к карточке Ozon (offer_id)".into(),
            });
        }

        for chunk in linked.chunks(BATCH) {
            let stocks: Vec<Value> = chunk
                .iter()
                .map(|u| json!({"offer_id": u.external_id, "stock": u.stock.max(0), "warehouse_id": self.warehouse_id}))
                .collect();
            let stock_response = self.post("/v2/products/stocks", &json!({ "stocks": stocks })).await?;
            let stock_rows = parse_result(&stock_response);

            let priced: Vec<&&VariantUpdate> = chunk.iter().filter(|u| u.retail_minor > 0).collect();
            let price_rows = if priced.is_empty() {
                Vec::new()
            } else {
                let prices: Vec<Value> = priced
                    .iter()
                    .map(|u| {
                        let price = rubles_ceil(u.retail_minor);
                        let old = rubles_ceil(u.old_price_minor);
                        json!({
                            "offer_id": u.external_id,
                            "price": price.to_string(),
                            // Ozon требует old_price > price; иначе зачёркнутой цены нет.
                            "old_price": if old > price { old.to_string() } else { "0".to_string() },
                            "min_price": rubles_ceil(u.min_price_minor.min(u.retail_minor)).to_string(),
                            "currency_code": "RUB",
                        })
                    })
                    .collect();
                parse_result(&self.post("/v1/product/import/prices", &json!({ "prices": prices })).await?)
            };

            for update in chunk {
                let stock_problem = stock_rows.iter().find(|(offer, _)| *offer == update.external_id).map(|(_, p)| p.clone());
                let wants_price = update.retail_minor > 0;
                let price_problem = price_rows.iter().find(|(offer, _)| *offer == update.external_id).map(|(_, p)| p.clone());
                let stock_sent = matches!(&stock_problem, Some(p) if p.is_empty());
                let price_sent = wants_price && matches!(&price_problem, Some(p) if p.is_empty());
                let mut problems = Vec::new();
                match &stock_problem {
                    None => problems.push("Ozon не вернул результат по остатку".to_string()),
                    Some(p) if !p.is_empty() => problems.push(format!("остаток: {p}")),
                    _ => {}
                }
                if wants_price {
                    match &price_problem {
                        None => problems.push("Ozon не вернул результат по цене".to_string()),
                        Some(p) if !p.is_empty() => problems.push(format!("цена: {p}")),
                        _ => {}
                    }
                }
                results.push(ItemResult {
                    variant_id: update.variant_id,
                    ok: problems.is_empty(),
                    price_sent,
                    stock_sent,
                    error: problems.join("; "),
                });
            }
        }
        Ok(results)
    }
}
