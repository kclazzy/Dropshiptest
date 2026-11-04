//! Заказы покупателей и помощь при выкупе. Режим по умолчанию — **ассистированный**: ядро проверяет заказ
//! (наличие, прибыль), готовит данные и передаёт их расширению, но оплату подтверждает человек.
//!
//! Главный принцип безопасности: результат выкупа неизвестен ⇒ **никогда не повторяем автоматически**
//! (иначе можно купить товар дважды). Повтор — только по явной команде пользователя.

use serde::Serialize;
use serde_json::json;
use sqlx::SqlitePool;

use crate::catalog;
use crate::db::{self, Order, OrderStatus, TaskType};
use crate::error::{AppError, AppResult};
use crate::pricing::profit_at;
use crate::stores::{StoreAdapter, StoreErrorKind, StoreOrder};

const MAX_QUANTITY: i64 = 1_000;
const MAX_TOTAL: i64 = 1_000_000_000_000;
const MAX_ADDRESS_BYTES: usize = 4_096;
const AUTO_ORDER_PRIORITY: i64 = 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Event {
    /// Проверка заказа завершена (true — можно выкупать).
    Evaluated(bool),
    /// Данные переданы расширению.
    HandedOff,
    PurchaseOk,
    PurchaseFailed,
    NeedsManual,
    TrackingReceived,
    /// Пользователь подтвердил выкуп вопреки предупреждению / нажал «Повторить».
    UserRetry,
    Cancel,
}

/// Чистая таблица переходов. `Ok(None)` — событие уже учтено (повтор безопасен), состояние не меняется.
pub fn transition(from: OrderStatus, event: Event) -> Result<Option<OrderStatus>, AppError> {
    use Event::*;
    use OrderStatus::*;
    let to = match (from, event) {
        (New, Evaluated(true)) => ReadyToBuy,
        (New, Evaluated(false)) => ManualReview,
        (ReadyToBuy, HandedOff) => QueuedForExt,
        (QueuedForExt, PurchaseOk) => Ordered,
        (QueuedForExt, PurchaseFailed) => Error,
        (QueuedForExt, NeedsManual) => ManualReview,
        (Ordered, TrackingReceived) => Shipped,
        (ManualReview | Error, UserRetry) => ReadyToBuy,
        (Cancelled, Cancel) => return Ok(None),
        (QueuedForExt | Ordered | Shipped, Cancel) => {
            return Err(AppError::Validation("выкуп уже начат или завершён — сначала проверьте заказ у поставщика, отменить здесь нельзя".into()));
        }
        (_, Cancel) => Cancelled,
        // повторная доставка того же события
        (ReadyToBuy, Evaluated(true)) | (QueuedForExt, HandedOff) | (Ordered, PurchaseOk) | (Shipped, TrackingReceived) | (Error, PurchaseFailed) | (ManualReview, NeedsManual) => return Ok(None),
        (from, event) => {
            return Err(AppError::Validation(format!("недопустимое действие {event:?} для заказа в статусе {from:?}")));
        }
    };
    Ok(Some(to))
}

fn rub(minor: i64) -> String {
    let sign = if minor < 0 { "−" } else { "" };
    let m = minor.abs();
    format!("{sign}{}.{:02} ₽", m / 100, m % 100)
}

#[derive(Debug, Default, PartialEq, Eq, Serialize)]
pub struct IngestReport {
    pub created: usize,
    pub duplicates: usize,
}

fn validate(o: &StoreOrder) -> AppResult<()> {
    let bad = |m: &str| Err(AppError::Validation(m.to_string()));
    if o.external_order_id.trim().is_empty() || o.external_order_id.len() > 200 {
        return bad("номер заказа пустой или слишком длинный");
    }
    if !(1..=MAX_QUANTITY).contains(&o.quantity) {
        return bad("количество в заказе должно быть от 1 до 1000");
    }
    if !(1..=MAX_TOTAL).contains(&o.total_minor) {
        return bad("сумма заказа некорректна");
    }
    if o.buyer_address_json.len() > MAX_ADDRESS_BYTES || serde_json::from_str::<serde_json::Value>(&o.buyer_address_json).is_err() {
        return bad("адрес покупателя некорректен");
    }
    Ok(())
}

/// Сохраняет заказы магазина. Повторная загрузка того же заказа ничего не меняет.
pub async fn ingest_orders(pool: &SqlitePool, store_id: i64, orders: &[StoreOrder], now: i64) -> AppResult<IngestReport> {
    let mut report = IngestReport::default();
    for o in orders {
        validate(o)?;
        let variant: Option<i64> = sqlx::query_scalar(
            "SELECT v.id FROM product_variants v JOIN products p ON p.id = v.product_id WHERE p.store_id = ? AND v.external_variant_id = ? LIMIT 1",
        )
        .bind(store_id)
        .bind(&o.external_variant_id)
        .fetch_optional(pool)
        .await?;
        let id: Option<i64> = sqlx::query_scalar(
            "INSERT INTO orders (store_id, external_order_id, variant_id, quantity, buyer_address_json, retail_total_minor, status, created_at, updated_at) \
             VALUES (?, ?, ?, ?, ?, ?, 'NEW', ?, ?) ON CONFLICT(external_order_id) DO NOTHING RETURNING id",
        )
        .bind(store_id)
        .bind(o.external_order_id.trim())
        .bind(variant)
        .bind(o.quantity)
        .bind(&o.buyer_address_json)
        .bind(o.total_minor)
        .bind(now)
        .bind(now)
        .fetch_optional(pool)
        .await?;
        match id {
            Some(id) => {
                evaluate_order(pool, id, now).await?;
                report.created += 1;
            }
            None => report.duplicates += 1,
        }
    }
    Ok(report)
}

struct Verdict {
    ok: bool,
    cost: Option<i64>,
    profit: Option<i64>,
    hint: String,
}

async fn judge(pool: &SqlitePool, order: &Order) -> AppResult<Verdict> {
    let refuse = |hint: &str| Verdict { ok: false, cost: None, profit: None, hint: hint.to_string() };
    let Some(variant_id) = order.variant_id else {
        return Ok(refuse("Товар из заказа не найден в каталоге. Свяжите артикул магазина с товаром и нажмите «Повторить»."));
    };
    let (price, shipping, stock, in_stock): (i64, i64, i64, bool) =
        sqlx::query_as("SELECT supplier_price_minor, shipping_price_minor, supplier_stock, is_in_stock FROM product_variants WHERE id = ?").bind(variant_id).fetch_one(pool).await?;
    if !in_stock || stock < order.quantity {
        return Ok(refuse("У поставщика нет нужного количества. Проверьте наличие вручную или отмените заказ."));
    }
    let cost = (price + shipping).checked_mul(order.quantity).ok_or_else(|| AppError::Validation("сумма выкупа слишком велика".into()))?;
    let mut conn = pool.acquire().await?;
    let Some(rules) = catalog::load_rule_set(&mut conn, order.store_id).await? else {
        return Ok(Verdict { cost: Some(cost), ..refuse("Для магазина не заданы правила цены — прибыль заказа посчитать нельзя.") });
    };
    let Some(rule) = rules.find(price) else {
        return Ok(Verdict { cost: Some(cost), ..refuse("Для цены этого товара нет правила — прибыль заказа посчитать нельзя.") });
    };
    let profit = profit_at(order.retail_total_minor, cost, rule)?;
    if profit < rule.min_profit_minor() {
        let hint = format!("Выкуп невыгоден: прибыль {} (минимум по правилу {}). Подтвердите вручную или отмените.", rub(profit), rub(rule.min_profit_minor()));
        return Ok(Verdict { ok: false, cost: Some(cost), profit: Some(profit), hint });
    }
    Ok(Verdict { ok: true, cost: Some(cost), profit: Some(profit), hint: format!("Готов к выкупу. Ожидаемая прибыль {}.", rub(profit)) })
}

pub async fn get_order(pool: &SqlitePool, order_id: i64) -> AppResult<Order> {
    sqlx::query_as("SELECT * FROM orders WHERE id = ?").bind(order_id).fetch_optional(pool).await?.ok_or_else(|| AppError::Validation("заказ не найден".into()))
}

/// Проверяет заказ в статусе NEW и ставит задачу выкупа, если всё в порядке.
pub async fn evaluate_order(pool: &SqlitePool, order_id: i64, now: i64) -> AppResult<OrderStatus> {
    let order = get_order(pool, order_id).await?;
    let verdict = judge(pool, &order).await?;
    sqlx::query("UPDATE orders SET supplier_cost_minor = ?, net_profit_minor = ? WHERE id = ?").bind(verdict.cost).bind(verdict.profit).bind(order_id).execute(pool).await?;
    let status = apply_event(pool, order_id, Event::Evaluated(verdict.ok), Some(&verdict.hint), None, now).await?;
    if status == OrderStatus::ReadyToBuy {
        enqueue_handoff(pool, order_id, now).await?;
    }
    Ok(status)
}

async fn enqueue_handoff(pool: &SqlitePool, order_id: i64, now: i64) -> AppResult<()> {
    let payload = json!({ "order_id": order_id }).to_string();
    let open: Option<i64> = sqlx::query_scalar("SELECT id FROM task_queue WHERE task_type = 'AUTO_ORDER' AND status IN ('PENDING','IN_PROGRESS') AND payload_json = ?").bind(&payload).fetch_optional(pool).await?;
    if open.is_none() {
        // одна попытка: повторов выкупа автоматически не бывает
        db::enqueue_task(pool, TaskType::AutoOrder, AUTO_ORDER_PRIORITY, &payload, now, 1).await?;
    }
    Ok(())
}

/// Применяет событие с проверкой «статус не изменился с момента чтения». Возвращает итоговый статус.
pub async fn apply_event(pool: &SqlitePool, order_id: i64, event: Event, hint: Option<&str>, error: Option<&str>, now: i64) -> AppResult<OrderStatus> {
    for _ in 0..3 {
        let from = get_order(pool, order_id).await?.status;
        let Some(to) = transition(from, event)? else { return Ok(from) };
        let done = sqlx::query(
            "UPDATE orders SET status = ?, human_readable_hint = COALESCE(?, human_readable_hint), error_message = ?, updated_at = ? WHERE id = ? AND status = ?",
        )
        .bind(to)
        .bind(hint)
        .bind(error)
        .bind(now)
        .bind(order_id)
        .bind(from)
        .execute(pool)
        .await?;
        if done.rows_affected() == 1 {
            if to == OrderStatus::ReadyToBuy && event == Event::UserRetry {
                enqueue_handoff(pool, order_id, now).await?;
            }
            return Ok(to);
        }
    }
    Err(AppError::Validation("заказ изменился одновременно, повторите действие".into()))
}

/// Получатель уведомлений для расширения; возвращает число подключённых получателей.
pub trait OrderNotifier: Send + Sync {
    fn notify(&self, method: &str, params: serde_json::Value) -> usize;
}

#[derive(Debug, PartialEq, Eq)]
pub enum HandoffOutcome {
    NoTask,
    /// Данные переданы; `delivered` — сколько расширений получили сообщение (0 — заказ ждёт в списке).
    HandedOff { order_id: i64, delivered: usize },
    /// Заказ уже не требует передачи (отменён, изменён).
    Stale { order_id: i64 },
}

/// Передаёт готовый заказ расширению. Задача завершается всегда: повторной передачи нет.
pub async fn process_next_handoff(pool: &SqlitePool, notifier: &dyn OrderNotifier, now: i64) -> AppResult<HandoffOutcome> {
    let Some(task) = db::dequeue_next_task_of(pool, now, 300, &[TaskType::AutoOrder]).await? else { return Ok(HandoffOutcome::NoTask) };
    let order_id = serde_json::from_str::<serde_json::Value>(&task.payload_json).ok().and_then(|v| v["order_id"].as_i64());
    let Some(order_id) = order_id else {
        db::fail_task(pool, task.id, "повреждённая задача", now, 0, false).await?;
        return Ok(HandoffOutcome::NoTask);
    };
    let order = get_order(pool, order_id).await?;
    if order.status != OrderStatus::ReadyToBuy {
        db::complete_task(pool, task.id).await?;
        return Ok(HandoffOutcome::Stale { order_id });
    }
    let (url, sku): (String, String) = sqlx::query_as(
        "SELECT p.supplier_url, v.supplier_sku FROM product_variants v JOIN products p ON p.id = v.product_id WHERE v.id = ?",
    )
    .bind(order.variant_id)
    .fetch_one(pool)
    .await?;
    // Сначала фиксируем состояние, потом уведомляем: сбой между шагами не приведёт к повторной передаче.
    apply_event(pool, order_id, Event::HandedOff, Some("Передано расширению. Откройте страницу поставщика, проверьте данные и оплатите заказ сами."), None, now).await?;
    db::complete_task(pool, task.id).await?;
    let address = serde_json::from_str::<serde_json::Value>(&order.buyer_address_json).unwrap_or(serde_json::Value::Null);
    let delivered = notifier.notify(
        "EXECUTE_AUTO_ORDER",
        json!({ "order_id": order_id, "url": url, "sku": sku, "quantity": order.quantity, "address": address, "mode": "assisted" }),
    );
    Ok(HandoffOutcome::HandedOff { order_id, delivered })
}

// ---------------------------------------------------------------------------------------------
// Результаты от расширения и трек-номера
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PurchaseResult {
    Ordered,
    Failed,
    NeedsManual,
}

pub async fn record_purchase_result(pool: &SqlitePool, order_id: i64, result: PurchaseResult, supplier_order_id: Option<&str>, message: Option<&str>, now: i64) -> AppResult<OrderStatus> {
    if supplier_order_id.is_some_and(|s| s.len() > 200) || message.is_some_and(|m| m.len() > 1_000) {
        return Err(AppError::Validation("слишком длинный ответ расширения".into()));
    }
    let (event, hint) = match result {
        PurchaseResult::Ordered => (Event::PurchaseOk, "Заказ у поставщика оформлен. Ждём трек-номер."),
        PurchaseResult::Failed => (Event::PurchaseFailed, "Выкуп не удался. Проверьте заказ у поставщика, чтобы не купить дважды, затем нажмите «Повторить»."),
        PurchaseResult::NeedsManual => (Event::NeedsManual, "Нужно оформить заказ вручную."),
    };
    let status = apply_event(pool, order_id, event, Some(hint), message, now).await?;
    if result == PurchaseResult::Ordered {
        sqlx::query("UPDATE orders SET supplier_order_id = COALESCE(supplier_order_id, ?) WHERE id = ?").bind(supplier_order_id).bind(order_id).execute(pool).await?;
    }
    Ok(status)
}

pub async fn record_tracking(pool: &SqlitePool, order_id: i64, tracking: &str, carrier: Option<&str>, now: i64) -> AppResult<OrderStatus> {
    let tracking = tracking.trim();
    if tracking.is_empty() || tracking.len() > 100 || !tracking.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.')) {
        return Err(AppError::Validation("трек-номер должен состоять из букв, цифр и знаков - _ .".into()));
    }
    if carrier.is_some_and(|c| c.len() > 100) {
        return Err(AppError::Validation("название перевозчика слишком длинное".into()));
    }
    let status = apply_event(pool, order_id, Event::TrackingReceived, Some("Трек-номер получен."), None, now).await?;
    sqlx::query("UPDATE orders SET tracking_number = ?, tracking_carrier = ? WHERE id = ? AND tracking_number IS NULL").bind(tracking).bind(carrier).bind(order_id).execute(pool).await?;
    Ok(status)
}

/// Передаёт трек в магазин. Площадка не поддерживает — остаётся подсказка «передайте вручную».
pub async fn push_tracking(pool: &SqlitePool, order_id: i64, adapter: &dyn StoreAdapter, now: i64) -> AppResult<bool> {
    let order = get_order(pool, order_id).await?;
    let (OrderStatus::Shipped, Some(track)) = (order.status, order.tracking_number.as_deref()) else { return Ok(false) };
    match adapter.submit_tracking(&order.external_order_id, track).await {
        Ok(()) => {
            sqlx::query("UPDATE orders SET tracking_sent_at = ?, human_readable_hint = 'Трек-номер передан в магазин.', error_message = NULL, updated_at = ? WHERE id = ?").bind(now).bind(now).bind(order_id).execute(pool).await?;
            Ok(true)
        }
        Err(e) => {
            let hint = if e.kind == StoreErrorKind::Unsupported { format!("Передайте трек-номер {track} в кабинете магазина вручную.") } else { e.to_user_friendly_ru() };
            sqlx::query("UPDATE orders SET human_readable_hint = ?, updated_at = ? WHERE id = ?").bind(hint).bind(now).bind(order_id).execute(pool).await?;
            Ok(false)
        }
    }
}

/// Строка списка заказов в формате, который ждёт расширение (суммы в копейках, адрес — объектом).
#[derive(Debug, Serialize, sqlx::FromRow)]
struct ViewRow {
    id: i64,
    external_order_id: String,
    status: OrderStatus,
    quantity: i64,
    retail_total_cents: i64,
    net_profit_cents: Option<i64>,
    buyer_address_json: String,
    supplier_order_id: Option<String>,
    tracking_number: Option<String>,
    error_message: Option<String>,
    human_readable_hint: Option<String>,
    supplier_url: Option<String>,
    sku: Option<String>,
    title: Option<String>,
}

pub async fn list_order_views(pool: &SqlitePool, limit: i64) -> AppResult<Vec<serde_json::Value>> {
    let rows: Vec<ViewRow> = sqlx::query_as(
        "SELECT o.id, o.external_order_id, o.status, o.quantity, o.retail_total_minor AS retail_total_cents, o.net_profit_minor AS net_profit_cents, \
                o.buyer_address_json, o.supplier_order_id, o.tracking_number, o.error_message, o.human_readable_hint, \
                p.supplier_url, v.supplier_sku AS sku, p.raw_title AS title \
         FROM orders o LEFT JOIN product_variants v ON v.id = o.variant_id LEFT JOIN products p ON p.id = v.product_id \
         ORDER BY o.id DESC LIMIT ?",
    )
    .bind(limit.clamp(1, 500))
    .fetch_all(pool)
    .await?;
    Ok(rows
        .into_iter()
        .map(|r| {
            let address = serde_json::from_str(&r.buyer_address_json).unwrap_or_else(|_| json!({}));
            let mut value = serde_json::to_value(&r).unwrap_or_default();
            if let Some(map) = value.as_object_mut() {
                map.remove("buyer_address_json");
                map.insert("address".into(), address);
            }
            value
        })
        .collect())
}

pub async fn list_orders(pool: &SqlitePool, limit: i64) -> AppResult<Vec<Order>> {
    Ok(sqlx::query_as("SELECT * FROM orders ORDER BY id DESC LIMIT ?").bind(limit.clamp(1, 500)).fetch_all(pool).await?)
}

#[cfg(test)]
mod tests;
