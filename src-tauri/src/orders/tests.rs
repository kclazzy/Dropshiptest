use std::sync::Mutex;

use async_trait::async_trait;

use super::*;
use crate::stores::{ItemResult, StoreError, StoreResult, VariantUpdate};

const ALL: [OrderStatus; 8] = [
    OrderStatus::New,
    OrderStatus::ReadyToBuy,
    OrderStatus::QueuedForExt,
    OrderStatus::Ordered,
    OrderStatus::Shipped,
    OrderStatus::ManualReview,
    OrderStatus::Error,
    OrderStatus::Cancelled,
];

#[test]
fn happy_path_transitions() {
    use Event::*;
    use OrderStatus::*;
    let steps = [(New, Evaluated(true), ReadyToBuy), (ReadyToBuy, HandedOff, QueuedForExt), (QueuedForExt, PurchaseOk, Ordered), (Ordered, TrackingReceived, Shipped)];
    for (from, event, to) in steps {
        assert_eq!(transition(from, event).unwrap(), Some(to), "{from:?} {event:?}");
    }
    assert_eq!(transition(New, Evaluated(false)).unwrap(), Some(ManualReview));
    assert_eq!(transition(QueuedForExt, PurchaseFailed).unwrap(), Some(Error));
    assert_eq!(transition(Error, UserRetry).unwrap(), Some(ReadyToBuy));
    assert_eq!(transition(ManualReview, UserRetry).unwrap(), Some(ReadyToBuy));
}

#[test]
fn repeated_events_are_noops_and_nonsense_is_refused() {
    use Event::*;
    use OrderStatus::*;
    for (state, event) in [(QueuedForExt, HandedOff), (Ordered, PurchaseOk), (Shipped, TrackingReceived), (Cancelled, Cancel)] {
        assert_eq!(transition(state, event).unwrap(), None, "{state:?} {event:?}");
    }
    assert!(transition(New, PurchaseOk).is_err(), "нельзя «выкупить» непроверенный заказ");
    assert!(transition(ReadyToBuy, TrackingReceived).is_err());
    assert!(transition(Cancelled, UserRetry).is_err());
}

#[test]
fn a_started_purchase_can_never_be_cancelled_or_retried_automatically() {
    use Event::*;
    use OrderStatus::*;
    for state in [QueuedForExt, Ordered, Shipped] {
        assert!(transition(state, Cancel).is_err(), "{state:?}");
    }
    // из «выкуп начат» нет прямого пути обратно в «готов к выкупу» — только через результат и явное решение человека
    assert!(transition(QueuedForExt, UserRetry).is_err());
    assert!(transition(QueuedForExt, Evaluated(true)).is_err());
    // каждое состояние и каждое событие дают определённый ответ (без паники)
    for s in ALL {
        for e in [Evaluated(true), Evaluated(false), HandedOff, PurchaseOk, PurchaseFailed, NeedsManual, TrackingReceived, UserRetry, Cancel] {
            let _ = transition(s, e);
        }
    }
}

struct Notifier(Mutex<Vec<(String, serde_json::Value)>>, usize);

impl OrderNotifier for Notifier {
    fn notify(&self, method: &str, params: serde_json::Value) -> usize {
        self.0.lock().unwrap().push((method.to_string(), params));
        self.1
    }
}

async fn setup() -> (tempfile::TempDir, SqlitePool, i64, i64) {
    let dir = tempfile::tempdir().unwrap();
    let pool = crate::db::init_pool(&dir.path().join("o.db")).await.unwrap();
    let (store, _p, variant) = crate::testutil::insert_product(&pool, "ozon", "o1").await;
    sqlx::query("UPDATE product_variants SET external_variant_id = 'OFFER-1' WHERE id = ?").bind(variant).execute(&pool).await.unwrap();
    // закупка 100.00 ₽, комиссия 15% + эквайринг/налог 6%, минимальная прибыль 300 ₽
    sqlx::query("INSERT INTO pricing_rules(store_id, price_from_minor, price_to_minor, margin_percent, min_profit_minor, min_stock_threshold) VALUES (?, 0, 9000000000, 50.0, 30000, 1)")
        .bind(store)
        .execute(&pool)
        .await
        .unwrap();
    (dir, pool, store, variant)
}

fn order(id: &str, total: i64, qty: i64) -> StoreOrder {
    StoreOrder { external_order_id: id.into(), external_variant_id: "OFFER-1".into(), quantity: qty, total_minor: total, buyer_address_json: r#"{"city":"Москва","street":"Тверская 1"}"#.into() }
}

#[tokio::test]
async fn profitable_order_becomes_ready_and_is_queued_once() {
    let (_d, pool, store, _v) = setup().await;
    let report = ingest_orders(&pool, store, &[order("A-1", 200_000, 1)], 10).await.unwrap();
    assert_eq!((report.created, report.duplicates), (1, 0));
    let o = list_orders(&pool, 10).await.unwrap().remove(0);
    assert_eq!((o.status, o.supplier_cost_minor), (OrderStatus::ReadyToBuy, Some(10_000)));
    assert!(o.net_profit_minor.unwrap() >= 30_000 && o.human_readable_hint.unwrap().contains("прибыль"));
    let again = ingest_orders(&pool, store, &[order("A-1", 200_000, 1)], 11).await.unwrap();
    assert_eq!((again.created, again.duplicates), (0, 1));
    let tasks: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue WHERE task_type = 'AUTO_ORDER'").fetch_one(&pool).await.unwrap();
    assert_eq!(tasks, 1);
}

#[tokio::test]
async fn unprofitable_unlinked_and_out_of_stock_orders_wait_for_a_human() {
    let (_d, pool, store, variant) = setup().await;
    ingest_orders(&pool, store, &[order("LOSS", 11_000, 1)], 10).await.unwrap();
    let mut unlinked = order("UNL", 200_000, 1);
    unlinked.external_variant_id = "UNKNOWN".into();
    ingest_orders(&pool, store, &[unlinked], 10).await.unwrap();
    ingest_orders(&pool, store, &[order("MANY", 5_000_000, 50)], 10).await.unwrap(); // остаток 10
    let by_id = |ext: &str| {
        let pool = pool.clone();
        let ext = ext.to_string();
        async move { sqlx::query_as::<_, Order>("SELECT * FROM orders WHERE external_order_id = ?").bind(ext).fetch_one(&pool).await.unwrap() }
    };
    let loss = by_id("LOSS").await;
    assert_eq!(loss.status, OrderStatus::ManualReview);
    assert!(loss.human_readable_hint.unwrap().contains("невыгоден"));
    assert!(by_id("UNL").await.human_readable_hint.unwrap().contains("не найден"));
    assert!(by_id("MANY").await.human_readable_hint.unwrap().contains("нет нужного количества"));
    let queued: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue").fetch_one(&pool).await.unwrap();
    assert_eq!(queued, 0, "проблемные заказы не уходят на выкуп");

    // человек подтверждает убыточный выкуп — только после этого заказ попадает в очередь
    let loss_id = loss.id;
    assert_eq!(apply_event(&pool, loss_id, Event::UserRetry, None, None, 20).await.unwrap(), OrderStatus::ReadyToBuy);
    let queued: i64 = sqlx::query_scalar("SELECT COUNT(*) FROM task_queue").fetch_one(&pool).await.unwrap();
    assert_eq!(queued, 1);
    let _ = variant;
}

#[tokio::test]
async fn invalid_orders_are_rejected_at_the_boundary() {
    let (_d, pool, store, _v) = setup().await;
    let mut cases = vec![order("", 100, 1), order("X", 100, 0), order("X", 0, 1), order("X", 100, 1001)];
    let mut bad_address = order("X", 100, 1);
    bad_address.buyer_address_json = "not json".into();
    cases.push(bad_address);
    for case in cases {
        assert!(ingest_orders(&pool, store, &[case], 1).await.is_err());
    }
    assert!(list_orders(&pool, 10).await.unwrap().is_empty());
}

#[tokio::test]
async fn handoff_notifies_extension_once_and_never_repeats() {
    let (_d, pool, store, _v) = setup().await;
    ingest_orders(&pool, store, &[order("H-1", 200_000, 2)], 10).await.unwrap();
    let notifier = Notifier(Mutex::new(vec![]), 1);
    let out = process_next_handoff(&pool, &notifier, 20).await.unwrap();
    assert_eq!(out, HandoffOutcome::HandedOff { order_id: 1, delivered: 1 });
    {
        let sent = notifier.0.lock().unwrap();
        assert_eq!(sent[0].0, "EXECUTE_AUTO_ORDER");
        assert_eq!((sent[0].1["url"].as_str(), sent[0].1["quantity"].as_i64(), sent[0].1["mode"].as_str()), (Some("https://supplier.example/item"), Some(2), Some("assisted")));
        assert_eq!(sent[0].1["address"]["city"], "Москва");
    }
    assert_eq!(get_order(&pool, 1).await.unwrap().status, OrderStatus::QueuedForExt);
    assert_eq!(process_next_handoff(&pool, &notifier, 30).await.unwrap(), HandoffOutcome::NoTask);
    assert_eq!(notifier.0.lock().unwrap().len(), 1);
}

#[tokio::test]
async fn handoff_without_connected_extension_still_marks_order_waiting() {
    let (_d, pool, store, _v) = setup().await;
    ingest_orders(&pool, store, &[order("H-2", 200_000, 1)], 10).await.unwrap();
    let out = process_next_handoff(&pool, &Notifier(Mutex::new(vec![]), 0), 20).await.unwrap();
    assert_eq!(out, HandoffOutcome::HandedOff { order_id: 1, delivered: 0 });
    assert_eq!(get_order(&pool, 1).await.unwrap().status, OrderStatus::QueuedForExt);
}

#[tokio::test]
async fn cancelled_order_is_not_handed_off() {
    let (_d, pool, store, _v) = setup().await;
    ingest_orders(&pool, store, &[order("C-1", 200_000, 1)], 10).await.unwrap();
    apply_event(&pool, 1, Event::Cancel, None, None, 11).await.unwrap();
    let n = Notifier(Mutex::new(vec![]), 1);
    assert_eq!(process_next_handoff(&pool, &n, 20).await.unwrap(), HandoffOutcome::Stale { order_id: 1 });
    assert!(n.0.lock().unwrap().is_empty());
}

#[tokio::test]
async fn purchase_result_and_tracking_flow_is_idempotent() {
    let (_d, pool, store, _v) = setup().await;
    ingest_orders(&pool, store, &[order("P-1", 200_000, 1)], 10).await.unwrap();
    process_next_handoff(&pool, &Notifier(Mutex::new(vec![]), 1), 11).await.unwrap();
    assert_eq!(record_purchase_result(&pool, 1, PurchaseResult::Ordered, Some("SUP-77"), None, 12).await.unwrap(), OrderStatus::Ordered);
    record_purchase_result(&pool, 1, PurchaseResult::Ordered, Some("SUP-OTHER"), None, 13).await.unwrap();
    assert_eq!(get_order(&pool, 1).await.unwrap().supplier_order_id.as_deref(), Some("SUP-77"), "повтор не перезаписывает");
    assert!(record_tracking(&pool, 1, "bad track!", None, 14).await.is_err());
    assert_eq!(record_tracking(&pool, 1, "RB123-45", Some("Почта"), 15).await.unwrap(), OrderStatus::Shipped);
    record_tracking(&pool, 1, "RB123-45", Some("Почта"), 16).await.unwrap();
    let o = get_order(&pool, 1).await.unwrap();
    assert_eq!((o.tracking_number.as_deref(), o.tracking_carrier.as_deref()), (Some("RB123-45"), Some("Почта")));
}

#[tokio::test]
async fn failed_purchase_requires_explicit_user_retry() {
    let (_d, pool, store, _v) = setup().await;
    ingest_orders(&pool, store, &[order("F-1", 200_000, 1)], 10).await.unwrap();
    process_next_handoff(&pool, &Notifier(Mutex::new(vec![]), 1), 11).await.unwrap();
    assert_eq!(record_purchase_result(&pool, 1, PurchaseResult::Failed, None, Some("капча"), 12).await.unwrap(), OrderStatus::Error);
    let o = get_order(&pool, 1).await.unwrap();
    assert_eq!(o.error_message.as_deref(), Some("капча"));
    assert!(o.human_readable_hint.unwrap().contains("чтобы не купить дважды"));
    assert_eq!(process_next_handoff(&pool, &Notifier(Mutex::new(vec![]), 1), 99_999).await.unwrap(), HandoffOutcome::NoTask, "автоповтора нет");
    apply_event(&pool, 1, Event::UserRetry, None, None, 20).await.unwrap();
    assert!(matches!(process_next_handoff(&pool, &Notifier(Mutex::new(vec![]), 1), 21).await.unwrap(), HandoffOutcome::HandedOff { .. }));
}

struct TrackAdapter(Result<(), crate::stores::StoreErrorKind>, Mutex<Vec<(String, String)>>);

#[async_trait]
impl StoreAdapter for TrackAdapter {
    fn name(&self) -> String {
        "T".into()
    }
    async fn update_variants(&self, _u: &[VariantUpdate]) -> StoreResult<Vec<ItemResult>> {
        Ok(vec![])
    }
    async fn submit_tracking(&self, id: &str, track: &str) -> StoreResult<()> {
        self.1.lock().unwrap().push((id.into(), track.into()));
        self.0.map_err(|k| StoreError::new(k, "x"))
    }
}

#[tokio::test]
async fn tracking_is_pushed_to_the_store_or_left_as_manual_hint() {
    let (_d, pool, store, _v) = setup().await;
    ingest_orders(&pool, store, &[order("T-1", 200_000, 1)], 10).await.unwrap();
    process_next_handoff(&pool, &Notifier(Mutex::new(vec![]), 1), 11).await.unwrap();
    record_purchase_result(&pool, 1, PurchaseResult::Ordered, None, None, 12).await.unwrap();
    let ok = TrackAdapter(Ok(()), Mutex::new(vec![]));
    assert!(!push_tracking(&pool, 1, &ok, 13).await.unwrap(), "до получения трека передавать нечего");
    record_tracking(&pool, 1, "TRK1", None, 14).await.unwrap();
    let unsupported = TrackAdapter(Err(StoreErrorKind::Unsupported), Mutex::new(vec![]));
    assert!(!push_tracking(&pool, 1, &unsupported, 15).await.unwrap());
    assert!(get_order(&pool, 1).await.unwrap().human_readable_hint.unwrap().contains("вручную"));
    assert!(push_tracking(&pool, 1, &ok, 16).await.unwrap());
    assert_eq!(ok.1.lock().unwrap()[0], ("T-1".to_string(), "TRK1".to_string()));
    let sent: Option<i64> = sqlx::query_scalar("SELECT tracking_sent_at FROM orders WHERE id = 1").fetch_one(&pool).await.unwrap();
    assert_eq!(sent, Some(16));
}

struct Feed(Mutex<Vec<i64>>, Result<Vec<StoreOrder>, StoreErrorKind>);

#[async_trait]
impl StoreAdapter for Feed {
    fn name(&self) -> String {
        "F".into()
    }
    async fn update_variants(&self, _u: &[VariantUpdate]) -> StoreResult<Vec<ItemResult>> {
        Ok(vec![])
    }
    async fn fetch_new_orders(&self, since: i64) -> StoreResult<Vec<StoreOrder>> {
        self.0.lock().unwrap().push(since);
        self.1.clone().map_err(|k| StoreError::new(k, "x"))
    }
}

#[tokio::test]
async fn polling_ingests_once_throttles_and_moves_the_cursor() {
    let (_d, pool, store, _v) = setup().await;
    let mut broken = order("BAD", 100, 1);
    broken.quantity = 0; // некорректный заказ не мешает остальным
    let feed = Feed(Mutex::new(vec![]), Ok(vec![broken, order("P-1", 200_000, 1)]));
    let first = poll_orders(&pool, store, &feed, 1_000_000).await.unwrap().unwrap();
    assert_eq!((first.created, first.duplicates), (1, 0));
    assert_eq!(poll_orders(&pool, store, &feed, 1_000_100).await.unwrap(), None, "рано: опрос не чаще 5 минут");
    let again = poll_orders(&pool, store, &feed, 1_000_400).await.unwrap().unwrap();
    assert_eq!((again.created, again.duplicates), (0, 1));
    assert_eq!(*feed.0.lock().unwrap(), vec![1_000_000 - 7 * 86_400, 1_000_000 - 3_600], "курсор сдвинут с запасом");
}

#[tokio::test]
async fn polling_failures_go_to_store_error_and_unsupported_is_silent() {
    let (_d, pool, store, _v) = setup().await;
    let down = Feed(Mutex::new(vec![]), Err(StoreErrorKind::Auth));
    assert_eq!(poll_orders(&pool, store, &down, 10_000).await.unwrap(), None);
    let err: Option<String> = sqlx::query_scalar("SELECT last_error FROM stores WHERE id = ?").bind(store).fetch_one(&pool).await.unwrap();
    assert!(err.is_some());
    let unsupported = Feed(Mutex::new(vec![]), Err(StoreErrorKind::Unsupported));
    sqlx::query("UPDATE stores SET last_error = NULL").execute(&pool).await.unwrap();
    assert_eq!(poll_orders(&pool, store, &unsupported, 20_000).await.unwrap(), None);
    let err: Option<String> = sqlx::query_scalar("SELECT last_error FROM stores WHERE id = ?").bind(store).fetch_one(&pool).await.unwrap();
    assert_eq!(err, None);
}
