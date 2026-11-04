use super::*;

async fn pool() -> (tempfile::TempDir, SqlitePool) {
    let dir = tempfile::tempdir().unwrap();
    let pool = crate::db::init_pool(&dir.path().join("u.db")).await.unwrap();
    (dir, pool)
}

#[tokio::test]
async fn wizard_default_catalog_is_idempotent_and_prices_existing_products() {
    let (_d, pool) = pool().await;
    let state = onboarding_state(&pool).await.unwrap();
    assert_eq!((state["done"].as_bool(), state["has_store"].as_bool(), state["has_rules"].as_bool()), (Some(false), Some(false), Some(false)));
    let store = setup_default_catalog(&pool, 1).await.unwrap();
    assert_eq!(setup_default_catalog(&pool, 2).await.unwrap(), store);
    assert_eq!(list_rules(&pool, store).await.unwrap().len(), 1);
    finish_onboarding(&pool, 3).await.unwrap();
    let state = onboarding_state(&pool).await.unwrap();
    assert_eq!((state["done"].as_bool(), state["has_rules"].as_bool()), (Some(true), Some(true)));
}

#[tokio::test]
async fn changing_rules_reprices_the_catalog() {
    let (_d, pool) = pool().await;
    let (store, _p, variant) = crate::testutil::insert_product(&pool, "local", "u1").await;
    let mut standard = RuleInput::standard();
    standard.min_profit_minor = 0;
    save_rules(&pool, store, &[standard.clone()], 10).await.unwrap();
    let before: i64 = sqlx::query_scalar("SELECT retail_price_minor FROM product_variants WHERE id = ?").bind(variant).fetch_one(&pool).await.unwrap();
    let mut greedy = standard;
    greedy.margin_percent = 100.0;
    save_rules(&pool, store, &[greedy], 20).await.unwrap();
    let after: i64 = sqlx::query_scalar("SELECT retail_price_minor FROM product_variants WHERE id = ?").bind(variant).fetch_one(&pool).await.unwrap();
    assert!(after > before && before > 10_000, "{before} -> {after}");
}

#[tokio::test]
async fn bad_rule_sets_are_rejected_without_touching_existing_rules() {
    let (_d, pool) = pool().await;
    let store = setup_default_catalog(&pool, 1).await.unwrap();
    let mut broken = RuleInput::standard();
    broken.marketplace_fee_percent = 99.0;
    assert!(save_rules(&pool, store, &[broken], 2).await.is_err());
    let mut a = RuleInput::standard();
    a.price_to_minor = 50_000;
    let mut b = RuleInput::standard();
    b.price_from_minor = 40_000; // пересечение диапазонов
    assert!(save_rules(&pool, store, &[a, b], 2).await.is_err());
    assert!(save_rules(&pool, store, &[], 2).await.is_err());
    assert!(save_rules(&pool, 999, &[RuleInput::standard()], 2).await.is_err());
    assert_eq!(list_rules(&pool, store).await.unwrap().len(), 1, "прежние правила на месте");
}

#[tokio::test]
async fn drafts_and_mine_split_by_linking_and_search_works() {
    let (_d, pool) = pool().await;
    let (_s, p1, v1) = crate::testutil::insert_product(&pool, "local", "a").await;
    let (_s2, p2, _v2) = crate::testutil::insert_product(&pool, "local", "b").await;
    sqlx::query("UPDATE products SET raw_title = 'Чайник' WHERE id = ?").bind(p2).execute(&pool).await.unwrap();
    let drafts = list_products(&pool, ProductFilter::Drafts, "", 50, 0).await.unwrap();
    assert_eq!(drafts.len(), 2);
    link_variant(&pool, v1, " OFFER-9 ").await.unwrap();
    let mine = list_products(&pool, ProductFilter::Mine, "", 50, 0).await.unwrap();
    assert_eq!((mine.len(), mine[0].id, mine[0].linked), (1, p1, 1));
    assert_eq!(list_products(&pool, ProductFilter::Drafts, "", 50, 0).await.unwrap()[0].id, p2);
    assert_eq!(list_products(&pool, ProductFilter::All, "чайн", 50, 0).await.unwrap().len(), 1);
    assert_eq!(list_products(&pool, ProductFilter::All, "ЧАЙН", 50, 0).await.unwrap().len(), 1, "регистр кириллицы не важен");
    assert_eq!(list_products(&pool, ProductFilter::All, "%", 50, 0).await.unwrap().len(), 0, "символы шаблона не работают как подстановка");
    link_variant(&pool, v1, "").await.unwrap();
    assert_eq!(list_products(&pool, ProductFilter::Mine, "", 50, 0).await.unwrap().len(), 0);
    assert!(link_variant(&pool, 999, "x").await.is_err());
    assert!(link_variant(&pool, v1, "bad\nid").await.is_err());
}

#[tokio::test]
async fn detail_shows_sync_state_and_history() {
    let (_d, pool) = pool().await;
    let (store, product, variant) = crate::testutil::insert_product(&pool, "local", "d").await;
    save_rules(&pool, store, &[RuleInput::standard()], 5).await.unwrap();
    link_variant(&pool, variant, "OFFER").await.unwrap();
    let d = product_detail(&pool, product).await.unwrap();
    assert_eq!(d["variants"][0]["sync_state"], "pending");
    assert!(d["history"].as_array().unwrap().len() == 1);
    let hash: String = sqlx::query_scalar("SELECT state_hash FROM product_variants WHERE id = ?").bind(variant).fetch_one(&pool).await.unwrap();
    sqlx::query("UPDATE product_variants SET published_hash = ? WHERE id = ?").bind(&hash).bind(variant).execute(&pool).await.unwrap();
    assert_eq!(product_detail(&pool, product).await.unwrap()["variants"][0]["sync_state"], "synced");
    sqlx::query("UPDATE product_variants SET published_hash = NULL, rejected_hash = ? WHERE id = ?").bind(&hash).bind(variant).execute(&pool).await.unwrap();
    assert_eq!(product_detail(&pool, product).await.unwrap()["variants"][0]["sync_state"], "rejected");
    assert!(product_detail(&pool, 999).await.is_err());
}

#[tokio::test]
async fn manual_text_edit_wins_and_is_validated() {
    let (_d, pool) = pool().await;
    let (_s, product, _v) = crate::testutil::insert_product(&pool, "local", "e").await;
    update_product_text(&pool, product, "  Мои наушники ", "Описание").await.unwrap();
    let rows = list_products(&pool, ProductFilter::All, "", 10, 0).await.unwrap();
    assert_eq!((rows[0].title.as_str(), rows[0].ai_status.as_str()), ("Мои наушники", "READY"));
    assert!(update_product_text(&pool, product, "", "x").await.is_err());
    assert!(update_product_text(&pool, product, &"я".repeat(501), "x").await.is_err());
    assert!(update_product_text(&pool, 999, "t", "d").await.is_err());
}

#[tokio::test]
async fn monitoring_toggle_and_delete_keep_orders() {
    let (_d, pool) = pool().await;
    let (store, product, variant) = crate::testutil::insert_product(&pool, "local", "m").await;
    set_monitoring(&pool, product, false).await.unwrap();
    let status: String = sqlx::query_scalar("SELECT monitor_status FROM products WHERE id = ?").bind(product).fetch_one(&pool).await.unwrap();
    assert_eq!(status, "PAUSED");
    sqlx::query("INSERT INTO orders(store_id, external_order_id, variant_id, quantity, buyer_address_json, retail_total_minor, created_at, updated_at) VALUES (?, 'O1', ?, 1, '{}', 100, 0, 0)").bind(store).bind(variant).execute(&pool).await.unwrap();
    delete_product(&pool, product).await.unwrap();
    let (orders, linked): (i64, i64) = sqlx::query_as("SELECT COUNT(*), COUNT(variant_id) FROM orders").fetch_one(&pool).await.unwrap();
    assert_eq!((orders, linked), (1, 0));
    assert!(delete_product(&pool, product).await.is_err());
}

#[tokio::test]
async fn stores_hide_secrets_validate_input_and_refuse_destructive_delete() {
    let (_d, pool) = pool().await;
    let vault = CredentialVault::from_key([9; 32]);
    let input = NewStoreInput { name: "Мой Ozon".into(), platform: "ozon".into(), credentials: json!({"client_id": "123", "api_key": "super-secret-key"}), warehouse_id: Some("777".into()) };
    let id = add_store(&pool, &vault, &input, 1).await.unwrap();
    let listed = list_stores(&pool).await.unwrap();
    let text = serde_json::to_string(&listed).unwrap();
    assert!(!text.contains("super-secret-key") && !text.contains("encrypted_credentials"), "{text}");
    assert_eq!(listed[0]["name"], "Мой Ozon");
    let bad = NewStoreInput { platform: "wildberries".into(), ..NewStoreInput { name: "x".into(), platform: String::new(), credentials: json!({}), warehouse_id: None } };
    assert!(add_store(&pool, &vault, &bad, 1).await.is_err());
    let missing_key = NewStoreInput { name: "Ozon".into(), platform: "ozon".into(), credentials: json!({"client_id": "1"}), warehouse_id: Some("1".into()) };
    assert!(add_store(&pool, &vault, &missing_key, 1).await.is_err());
    crate::testutil::insert_product(&pool, "ozon", "s").await;
    assert!(delete_store(&pool, id).await.is_err(), "магазин с товарами не удаляется");
    set_store_active(&pool, id, false).await.unwrap();
    assert_eq!(list_stores(&pool).await.unwrap()[0]["is_active"], false);
}

#[tokio::test]
async fn proxies_are_listed_without_credentials() {
    let (_d, pool) = pool().await;
    crate::monitor::fetch::add_proxy(&pool, "http://user:pass@proxy.example:8080").await.unwrap();
    let listed = list_proxies(&pool).await.unwrap();
    assert!(!serde_json::to_string(&listed).unwrap().contains("pass"));
    remove_proxy(&pool, listed[0]["id"].as_i64().unwrap()).await.unwrap();
    assert!(list_proxies(&pool).await.unwrap().is_empty());
}
