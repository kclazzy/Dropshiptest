//! Ozon Seller API: остатки (`/v2/products/stocks`) и цены (`/v1/product/import/prices`).
//! ⚠ Не сверено с официальной документацией (см. `stores/mod.rs`).

use reqwest::Url;
use serde_json::{json, Value};

use super::{
    classify, http_client, join, limiter, rubles_ceil, ItemResult, StoreAdapter, StoreError, StoreErrorKind, StoreResult,
    StoreOrder, VariantUpdate,
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

/// Unix-секунды -> «2026-11-04T12:00:00Z» (без внешних крейтов; алгоритм civil-from-days).
pub(crate) fn iso_utc(unix: i64) -> String {
    let days = unix.div_euclid(86_400);
    let secs = unix.rem_euclid(86_400);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z", secs / 3_600, secs % 3_600 / 60, secs % 60)
}

/// Отправления Ozon -> наши заказы: по строке на каждый товар отправления.
fn parse_postings(response: &Value) -> Vec<StoreOrder> {
    let Some(postings) = response.pointer("/result/postings").and_then(Value::as_array) else { return Vec::new() };
    let mut orders = Vec::new();
    for posting in postings {
        let Some(number) = posting.get("posting_number").and_then(Value::as_str) else { continue };
        let products = posting.get("products").and_then(Value::as_array).cloned().unwrap_or_default();
        let multi = products.len() > 1;
        let address = json!({
            "name": posting.pointer("/addressee/name"),
            "phone": posting.pointer("/addressee/phone"),
            "city": posting.pointer("/analytics_data/city"),
            "street": posting.pointer("/customer/address/address_tail"),
        });
        for (i, product) in products.iter().enumerate() {
            let (Some(offer), Some(quantity)) = (product.get("offer_id").and_then(Value::as_str), product.get("quantity").and_then(Value::as_i64)) else { continue };
            let unit = product.get("price").and_then(Value::as_str).and_then(crate::monitor::parse::parse_price_minor).unwrap_or(0);
            orders.push(StoreOrder {
                external_order_id: if multi { format!("{number}#{}", i + 1) } else { number.to_string() },
                external_variant_id: offer.to_string(),
                quantity,
                total_minor: unit.saturating_mul(quantity),
                buyer_address_json: address.to_string(),
            });
        }
    }
    orders
}

#[async_trait::async_trait]
impl StoreAdapter for OzonAdapter {
    fn name(&self) -> String {
        "ozon".into()
    }

    /// ⚠ По памяти: POST /v3/posting/fbs/list, фильтр по дате и статусу «ожидает сборки».
    async fn fetch_new_orders(&self, since: i64) -> StoreResult<Vec<StoreOrder>> {
        let mut orders = Vec::new();
        let mut offset = 0;
        for _ in 0..20 {
            let body = json!({
                "dir": "ASC",
                "filter": { "since": iso_utc(since.max(0)), "to": iso_utc(since.max(0) + 90 * 86_400), "status": "awaiting_packaging" },
                "limit": 100, "offset": offset, "with": { "analytics_data": true }
            });
            let response = self.post("/v3/posting/fbs/list", &body).await?;
            orders.extend(parse_postings(&response));
            if response.pointer("/result/has_next").and_then(Value::as_bool) != Some(true) {
                break;
            }
            offset += 100;
        }
        Ok(orders)
    }

    /// ⚠ По памяти: POST /v2/fbs/posting/tracking-number/set.
    async fn submit_tracking(&self, external_order_id: &str, tracking_number: &str) -> StoreResult<()> {
        let posting = external_order_id.split('#').next().unwrap_or(external_order_id);
        let body = json!({ "tracking_numbers": [{ "posting_number": posting, "tracking_number": tracking_number }] });
        let response = self.post("/v2/fbs/posting/tracking-number/set", &body).await?;
        let row = response.get("result").and_then(Value::as_array).and_then(|r| r.first());
        match row {
            Some(r) if r.get("result").and_then(Value::as_bool) == Some(true) => Ok(()),
            Some(r) => Err(StoreError::new(StoreErrorKind::Rejected, format!("Ozon не принял трек-номер: {}", r.get("error").and_then(Value::as_str).unwrap_or("причина не указана")))),
            None => Err(StoreError::new(StoreErrorKind::Transport, "Ozon не вернул результат по треку")),
        }
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
