use super::*;
use crate::testutil::{insert_product, MockServer};

fn upd(id: i64, ext: &str, retail: i64, stock: i64) -> VariantUpdate {
    VariantUpdate { variant_id: id, external_id: ext.into(), retail_minor: retail, min_price_minor: 12_000, old_price_minor: 20_000, stock }
}

fn ozon(server: &MockServer) -> ozon::OzonAdapter {
    ozon::OzonAdapter::new(Url::parse(&server.base_url).unwrap(), "CID".into(), "KEY".into(), 5, 1000.0).unwrap()
}

fn avito(server: &MockServer) -> avito::AvitoAdapter {
    avito::AvitoAdapter::new(Url::parse(&server.base_url).unwrap(), "id".into(), "secret".into(), 1000.0).unwrap()
}

fn ok_rows(offers: &[&str]) -> String {
    let rows: Vec<_> = offers.iter().map(|o| serde_json::json!({"offer_id": o, "updated": true, "errors": []})).collect();
    serde_json::json!({ "result": rows }).to_string()
}

#[tokio::test]
async fn ozon_sends_stock_and_rounded_up_prices_with_credentials() {
    let server = MockServer::start().await;
    server.reply("/v2/products/stocks", 200, &ok_rows(&["A"]));
    server.reply("/v1/product/import/prices", 200, &ok_rows(&["A"]));
    let results = ozon(&server).update_variants(&[upd(1, "A", 14_499, 7)]).await.unwrap();
    assert_eq!(results, vec![ItemResult { variant_id: 1, ok: true, price_sent: true, stock_sent: true, error: String::new() }]);

    let stock = &server.requests_to("/v2/products/stocks")[0];
    assert_eq!(stock.headers.get("client-id").map(String::as_str), Some("CID"));
    assert_eq!(stock.headers.get("api-key").map(String::as_str), Some("KEY"));
    assert_eq!(stock.json()["stocks"][0], serde_json::json!({"offer_id": "A", "stock": 7, "warehouse_id": 5}));
    let price = &server.requests_to("/v1/product/import/prices")[0].json()["prices"][0];
    assert_eq!(price["price"], "145"); // 144,99 ₽ -> вверх
    assert_eq!(price["old_price"], "200");
    assert_eq!(price["min_price"], "120");
    assert_eq!(price["currency_code"], "RUB");
}

#[tokio::test]
async fn ozon_old_price_is_dropped_when_not_above_price_and_unpriced_variants_send_only_stock() {
    let server = MockServer::start().await;
    server.reply("/v2/products/stocks", 200, &ok_rows(&["A", "B"]));
    server.reply("/v1/product/import/prices", 200, &ok_rows(&["A"]));
    let mut a = upd(1, "A", 14_499, 1);
    a.old_price_minor = 14_499;
    let b = upd(2, "B", 0, 3); // цена не рассчитана
    let results = ozon(&server).update_variants(&[a, b]).await.unwrap();
    assert_eq!(server.requests_to("/v1/product/import/prices")[0].json()["prices"][0]["old_price"], "0");
    assert_eq!(server.requests_to("/v1/product/import/prices")[0].json()["prices"].as_array().unwrap().len(), 1);
    assert!(results[1].ok && results[1].stock_sent && !results[1].price_sent);
}

#[tokio::test]
async fn ozon_reports_per_item_errors_and_skips_unlinked_variants() {
    let server = MockServer::start().await;
    server.reply(
        "/v2/products/stocks",
        200,
        r#"{"result":[{"offer_id":"A","updated":true,"errors":[]},{"offer_id":"B","updated":false,"errors":[{"code":"NOT_FOUND","message":"Товар не найден"}]}]}"#,
    );
    server.reply("/v1/product/import/prices", 200, &ok_rows(&["A", "B"]));
    let results = ozon(&server).update_variants(&[upd(1, "A", 100, 1), upd(2, "B", 100, 1), upd(3, "", 100, 1)]).await.unwrap();
    let by_id = |id| results.iter().find(|r| r.variant_id == id).unwrap();
    assert!(by_id(1).ok);
    assert!(!by_id(2).ok && by_id(2).error.contains("Товар не найден") && by_id(2).price_sent);
    assert!(!by_id(3).ok && by_id(3).error.contains("не привязан"));
    assert_eq!(server.requests_to("/v2/products/stocks")[0].json()["stocks"].as_array().unwrap().len(), 2); // без «пустого» offer_id
}

#[tokio::test]
async fn ozon_batches_by_one_hundred() {
    let server = MockServer::start().await;
    let offers: Vec<String> = (0..250).map(|i| format!("O{i}")).collect();
    let refs: Vec<&str> = offers.iter().map(String::as_str).collect();
    server.reply("/v2/products/stocks", 200, &ok_rows(&refs));
    server.reply("/v1/product/import/prices", 200, &ok_rows(&refs));
    let updates: Vec<_> = offers.iter().enumerate().map(|(i, o)| upd(i as i64, o, 100, 1)).collect();
    let results = ozon(&server).update_variants(&updates).await.unwrap();
    assert_eq!(results.len(), 250);
    assert!(results.iter().all(|r| r.ok));
    assert_eq!(server.requests_to("/v2/products/stocks").len(), 3);
}

#[tokio::test]
async fn ozon_whole_request_failures_are_classified() {
    for (status, kind, retryable) in [
        (401, StoreErrorKind::Auth, false),
        (403, StoreErrorKind::Auth, false),
        (429, StoreErrorKind::RateLimited, true),
        (500, StoreErrorKind::Transport, true),
        (400, StoreErrorKind::Rejected, false),
    ] {
        let server = MockServer::start().await;
        server.reply("/v2/products/stocks", status, r#"{"message":"x"}"#);
        let err = ozon(&server).update_variants(&[upd(1, "A", 100, 1)]).await.unwrap_err();
        assert_eq!((err.kind, err.retryable()), (kind, retryable), "{status}");
    }
    let unreachable = ozon::OzonAdapter::new(Url::parse("http://127.0.0.1:1").unwrap(), "c".into(), "k".into(), 1, 1000.0).unwrap();
    assert_eq!(unreachable.update_variants(&[upd(1, "A", 1, 1)]).await.unwrap_err().kind, StoreErrorKind::Transport);
}

#[tokio::test]
async fn ozon_retry_after_header_is_reported() {
    // Заголовок Retry-After проверяем на чистой функции: тестовый сервер заголовков ответа не задаёт.
    let mut headers = HeaderMap::new();
    headers.insert("retry-after", "17".parse().unwrap());
    let e = classify(StatusCode::TOO_MANY_REQUESTS, &headers, "");
    assert_eq!((e.kind, e.retry_after_secs), (StoreErrorKind::RateLimited, 17));
}

#[tokio::test]
async fn avito_gets_a_token_once_and_updates_stock_and_price() {
    let server = MockServer::start().await;
    server.reply("/token", 200, r#"{"access_token":"T1","expires_in":86400}"#);
    server.reply("/stock-management/1/stocks", 200, r#"{"stocks":[{"item_id":111,"success":true}]}"#);
    server.reply("/core/v1/items/111/update_price", 200, "{}");
    let adapter = avito(&server);
    for _ in 0..2 {
        let results = adapter.update_variants(&[upd(1, "111", 14_499, 4)]).await.unwrap();
        assert_eq!(results, vec![ItemResult { variant_id: 1, ok: true, price_sent: true, stock_sent: true, error: String::new() }]);
    }
    assert_eq!(server.requests_to("/token").len(), 1);
    let token_request = &server.requests_to("/token")[0];
    assert!(token_request.body.contains("grant_type=client_credentials") && token_request.body.contains("client_id=id"));
    let stock = &server.requests_to("/stock-management/1/stocks")[0];
    assert_eq!(stock.method, "PUT");
    assert_eq!(stock.headers.get("authorization").map(String::as_str), Some("Bearer T1"));
    assert_eq!(stock.json()["stocks"][0], serde_json::json!({"item_id": 111, "quantity": 4}));
    assert_eq!(server.requests_to("/core/v1/items/111/update_price")[0].json(), serde_json::json!({"price": 145}));
}

#[tokio::test]
async fn avito_refreshes_a_short_lived_token_and_retries_once_on_401() {
    let server = MockServer::start().await;
    server.reply("/token", 200, r#"{"access_token":"SHORT","expires_in":30}"#); // короче запаса — обновляется каждый раз
    server.reply("/stock-management/1/stocks", 200, r#"{"stocks":[{"item_id":5,"success":true}]}"#);
    let adapter = avito(&server);
    adapter.update_variants(&[upd(1, "5", 0, 1)]).await.unwrap();
    adapter.update_variants(&[upd(1, "5", 0, 1)]).await.unwrap();
    assert_eq!(server.requests_to("/token").len(), 2);

    let server = MockServer::start().await;
    server.reply("/token", 200, r#"{"access_token":"T","expires_in":86400}"#);
    server.reply("/stock-management/1/stocks", 401, "{}");
    server.reply("/stock-management/1/stocks", 200, r#"{"stocks":[{"item_id":5,"success":true}]}"#);
    let results = avito(&server).update_variants(&[upd(1, "5", 0, 1)]).await.unwrap();
    assert!(results[0].ok);
    assert_eq!(server.requests_to("/token").len(), 2); // токен получен заново

    let server = MockServer::start().await;
    server.reply("/token", 200, r#"{"access_token":"T","expires_in":86400}"#);
    server.reply("/stock-management/1/stocks", 401, "{}");
    assert_eq!(avito(&server).update_variants(&[upd(1, "5", 0, 1)]).await.unwrap_err().kind, StoreErrorKind::Auth);
    assert_eq!(server.requests_to("/stock-management/1/stocks").len(), 2); // не больше одного повтора
}

#[tokio::test]
async fn avito_bad_credentials_and_bad_items() {
    let server = MockServer::start().await;
    server.reply("/token", 400, r#"{"error":"invalid_client"}"#);
    assert_eq!(avito(&server).update_variants(&[upd(1, "5", 0, 1)]).await.unwrap_err().kind, StoreErrorKind::Auth);

    let server = MockServer::start().await;
    server.reply("/token", 200, r#"{"access_token":"T","expires_in":86400}"#);
    server.reply("/stock-management/1/stocks", 200, r#"{"stocks":[{"item_id":5,"success":false,"errors":["Объявление не найдено"]}]}"#);
    server.reply("/core/v1/items/5/update_price", 400, r#"{"error":"bad price"}"#);
    let results = avito(&server).update_variants(&[upd(1, "5", 100, 1), upd(2, "abc", 100, 1)]).await.unwrap();
    let first = results.iter().find(|r| r.variant_id == 1).unwrap();
    assert!(first.error.contains("Объявление не найдено") && first.error.contains("цена"));
    assert!(!first.price_sent && !first.stock_sent);
    assert!(results.iter().any(|r| r.variant_id == 2 && r.error.contains("не привязан")));
}

#[tokio::test]
async fn avito_orders_and_tracking_are_reported_as_unsupported() {
    let server = MockServer::start().await;
    let adapter = avito(&server);
    assert_eq!(adapter.fetch_new_orders(0).await.unwrap_err().kind, StoreErrorKind::Unsupported);
    assert_eq!(adapter.submit_tracking("1", "2").await.unwrap_err().kind, StoreErrorKind::Unsupported);
    assert!(server.requests().is_empty());
}

#[test]
fn iso_dates_are_correct() {
    assert_eq!(ozon::iso_utc(0), "1970-01-01T00:00:00Z");
    assert_eq!(ozon::iso_utc(951_782_400 + 3_661), "2000-02-29T01:01:01Z");
    assert_eq!(ozon::iso_utc(1_793_793_600), "2026-11-04T12:00:00Z");
}

#[tokio::test]
async fn ozon_postings_become_orders_with_pagination() {
    let server = MockServer::start().await;
    server.reply("/v3/posting/fbs/list", 200, r#"{"result":{"has_next":true,"postings":[{"posting_number":"111-1","products":[{"offer_id":"A","quantity":2,"price":"1500.50"}],"addressee":{"name":"Иван","phone":"+7"},"analytics_data":{"city":"Москва"},"customer":{"address":{"address_tail":"Тверская 1"}}}]}}"#);
    server.reply("/v3/posting/fbs/list", 200, r#"{"result":{"has_next":false,"postings":[{"posting_number":"222-1","products":[{"offer_id":"B","quantity":1,"price":"100"},{"offer_id":"C","quantity":3,"price":"10"}]}]}}"#);
    let orders = ozon(&server).fetch_new_orders(1_793_793_600).await.unwrap();
    assert_eq!(orders.iter().map(|o| (o.external_order_id.as_str(), o.external_variant_id.as_str(), o.quantity, o.total_minor)).collect::<Vec<_>>(),
        vec![("111-1", "A", 2, 300_100), ("222-1#1", "B", 1, 10_000), ("222-1#2", "C", 3, 3_000)]);
    assert!(orders[0].buyer_address_json.contains("Москва"));
    let reqs = server.requests_to("/v3/posting/fbs/list");
    assert_eq!((reqs.len(), reqs[1].json()["offset"].as_i64()), (2, Some(100)));
    assert_eq!(reqs[0].json()["filter"]["since"], "2026-11-04T12:00:00Z");
}

#[tokio::test]
async fn ozon_tracking_accepts_success_and_reports_rejection() {
    let server = MockServer::start().await;
    server.reply("/v2/fbs/posting/tracking-number/set", 200, r#"{"result":[{"posting_number":"222-1","result":true,"error":""}]}"#);
    ozon(&server).submit_tracking("222-1#2", "TRK1").await.unwrap();
    assert_eq!(server.requests_to("/v2/fbs/posting/tracking-number/set")[0].json()["tracking_numbers"][0]["posting_number"], "222-1");
    let bad = MockServer::start().await;
    bad.reply("/v2/fbs/posting/tracking-number/set", 200, r#"{"result":[{"posting_number":"1","result":false,"error":"wrong status"}]}"#);
    let err = ozon(&bad).submit_tracking("1", "T").await.unwrap_err();
    assert_eq!(err.kind, StoreErrorKind::Rejected);
    assert!(err.message.contains("wrong status"));
}

#[test]
fn money_and_limits() {
    assert_eq!([rubles_ceil(0), rubles_ceil(1), rubles_ceil(100), rubles_ceil(14_499), rubles_ceil(i64::MAX)], [0, 1, 1, 145, i64::MAX / 100]);
    assert!(limiter(0.0).is_err() && limiter(f64::NAN).is_err() && limiter(5000.0).is_err() && limiter(2.0).is_ok());
}

#[test]
fn base_url_must_be_https_or_local() {
    assert!(validate_base_url("https://api-seller.ozon.ru").is_ok());
    assert!(validate_base_url("http://127.0.0.1:9999").is_ok());
    assert!(validate_base_url("http://evil.example").is_err());
    assert!(validate_base_url("ftp://x").is_err());
    assert!(validate_base_url("not a url").is_err());
}

async fn pool() -> (tempfile::TempDir, SqlitePool) {
    let dir = tempfile::tempdir().unwrap();
    let pool = crate::db::init_pool(&dir.path().join("s.db")).await.unwrap();
    (dir, pool)
}

fn new_store<'a>(platform: PlatformType, creds: &'a str, warehouse: Option<&'a str>) -> NewStore<'a> {
    NewStore { name: " Мой Ozon ", platform, credentials_json: creds, warehouse_id: warehouse, api_url: None, now: 5 }
}

#[tokio::test]
async fn add_store_encrypts_credentials_and_builds_a_working_adapter() {
    let (_d, pool) = pool().await;
    let vault = CredentialVault::from_key([3; 32]);
    let server = MockServer::start().await;
    server.reply("/v2/products/stocks", 200, &ok_rows(&["A"]));
    let creds = r#"{"client_id":"CID","api_key":"top-secret-key"}"#;
    let id = add_store(&pool, &vault, NewStore { api_url: Some(&server.base_url), ..new_store(PlatformType::Ozon, creds, Some("5")) }).await.unwrap();

    let store: Store = sqlx::query_as("SELECT * FROM stores WHERE id = ?").bind(id).fetch_one(&pool).await.unwrap();
    assert_eq!(store.name, "Мой Ozon");
    assert!(!String::from_utf8_lossy(&store.encrypted_credentials).contains("top-secret-key"));
    let adapter = build_adapter(&store, &vault).unwrap();
    assert_eq!(adapter.name(), "ozon");
    assert!(adapter.update_variants(&[upd(1, "A", 0, 2)]).await.unwrap()[0].ok);
    assert_eq!(server.requests_to("/v2/products/stocks")[0].headers.get("api-key").map(String::as_str), Some("top-secret-key"));
    // Чужой файл ключа — понятная ошибка настройки.
    assert_eq!(build_adapter(&store, &CredentialVault::from_key([4; 32])).err().unwrap().kind, StoreErrorKind::Config);
}

#[tokio::test]
async fn add_store_rejects_bad_input_without_touching_the_database() {
    let (_d, pool) = pool().await;
    let vault = CredentialVault::from_key([3; 32]);
    let ok = r#"{"client_id":"1","api_key":"k"}"#;
    let avito_ok = r#"{"client_id":"1","client_secret":"s"}"#;
    let bad: Vec<NewStore> = vec![
        NewStore { name: "  ", ..new_store(PlatformType::Ozon, ok, Some("5")) },
        new_store(PlatformType::Ozon, ok, None),              // нет склада
        new_store(PlatformType::Ozon, ok, Some("-1")),
        new_store(PlatformType::Ozon, "{}", Some("5")),       // нет ключей
        new_store(PlatformType::Ozon, "not json", Some("5")),
        new_store(PlatformType::Ozon, r#"{"client_id":"","api_key":"k"}"#, Some("5")),
        new_store(PlatformType::Avito, ok, None),             // чужой формат ключей
        new_store(PlatformType::Shopify, ok, None),           // адаптера нет
        NewStore { api_url: Some("http://evil.example"), ..new_store(PlatformType::Ozon, ok, Some("5")) },
    ];
    for store in bad {
        assert!(matches!(add_store(&pool, &vault, store).await, Err(AppError::Validation(_))));
    }
    let count: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM stores").fetch_one(&pool).await.unwrap();
    assert_eq!(count, 0);
    assert!(add_store(&pool, &vault, new_store(PlatformType::Avito, avito_ok, None)).await.is_ok());
    let _ = insert_product; // фикстура товара нужна в следующих шагах
}
