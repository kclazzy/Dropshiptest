#include "orders/Tracking.hpp"

#include "core/Json.hpp"
#include "orders/Orders.hpp"

namespace dropship::orders {

namespace {

using db::DbError;

bool hasControlChars(const std::string& s) {
    for (const unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

void noteOrder(db::DatabaseManager& database, std::int64_t orderId, const std::string& message, UnixSeconds now) {
    (void)database.execute("UPDATE orders SET error_message = ?, updated_at = ? WHERE id = ?", {message, now, orderId});
}

}  // namespace

db::Result<bool> applyTracking(db::DatabaseManager& database, std::int64_t orderId, const std::string& trackingNumber,
                               const std::string& carrier, UnixSeconds now) {
    if (trackingNumber.empty() || trackingNumber.size() > kMaxTrackingLength || hasControlChars(trackingNumber)) {
        return unexpected(DbError{0, "invalid tracking number"});
    }
    if (carrier.size() > 100 || hasControlChars(carrier)) return unexpected(DbError{0, "invalid carrier"});
    bool changed = false;
    const auto done = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        const auto row = tx.query("SELECT status, tracking_number, tracking_carrier FROM orders WHERE id = ?", {orderId});
        if (!row) return unexpected(row.error());
        if (row->rows.empty()) return unexpected(DbError{0, "order not found"});
        const std::string& status = row->textAt(0, "status");
        if (status == "NEW") return unexpected(DbError{0, "order has not been purchased yet"});
        const bool same = !row->isNull(0, "tracking_number") && row->textAt(0, "tracking_number") == trackingNumber &&
                          (row->isNull(0, "tracking_carrier") ? std::string() : row->textAt(0, "tracking_carrier")) == carrier;
        if (same && status == "SHIPPED") return {};
        const auto upd = tx.execute(
            "UPDATE orders SET status = 'SHIPPED', tracking_number = ?, tracking_carrier = ?, "
            "tracking_submitted_at = NULL, error_message = NULL, updated_at = ? WHERE id = ?",
            {trackingNumber, carrier.empty() ? db::Value{} : db::Value{carrier}, now, orderId});
        if (!upd) return unexpected(upd.error());
        changed = true;
        const auto task = db::DatabaseManager::enqueueTaskIn(tx, TaskType::TrackingSync, 4,
                                                             dumpJson(Json{{"order_id", orderId}}), now, 5);
        if (!task) return unexpected(task.error());
        return {};
    });
    if (!done) return unexpected(done.error());
    return changed;
}

db::Result<TrackingOutcome> processNextTrackingSync(db::DatabaseManager& database, store::IAdapterProvider& provider,
                                                    UnixSeconds now, std::chrono::seconds retryDelay) {
    const TaskType types[] = {TaskType::TrackingSync};
    const auto claimed = database.claimNextTask(now, std::chrono::minutes(10), types);
    if (!claimed) return unexpected(claimed.error());
    if (!*claimed) return TrackingOutcome::NoTask;
    const db::Task& task = **claimed;

    const auto fail = [&](const std::string& message, bool retry, std::optional<std::int64_t> orderId)
        -> db::Result<TrackingOutcome> {
        const auto f = database.failTask(task.id, message, now, retryDelay, retry);
        if (!f) return unexpected(f.error());
        if (*f == db::FailOutcome::WillRetry) return TrackingOutcome::Retrying;
        if (orderId) noteOrder(database, *orderId, "tracking was not sent to the store: " + message, now);
        return TrackingOutcome::Failed;
    };

    const Json payload = parseJson(task.payloadJson);
    const auto id = payload.is_object() && payload.contains("order_id") ? asInt64(payload["order_id"]) : std::nullopt;
    if (!id) return fail("invalid TRACKING_SYNC payload", false, std::nullopt);
    const std::int64_t orderId = *id;

    const auto row = database.query(
        "SELECT o.store_id, o.external_order_id, o.tracking_number, o.tracking_submitted_at, o.status, s.platform_type "
        "FROM orders o JOIN stores s ON s.id = o.store_id WHERE o.id = ?",
        {orderId});
    if (!row) return unexpected(row.error());
    if (row->rows.empty()) return fail("order not found", false, std::nullopt);

    const auto complete = [&](const char* mark) -> db::Result<TrackingOutcome> {
        if (mark) {
            const auto u = database.execute("UPDATE orders SET tracking_submitted_at = ? WHERE id = ?", {now, orderId});
            if (!u) return unexpected(u.error());
        }
        const auto c = database.completeTask(task.id);
        if (!c) return unexpected(c.error());
        return mark ? TrackingOutcome::Submitted : TrackingOutcome::Skipped;
    };

    if (row->isNull(0, "tracking_number") || !row->isNull(0, "tracking_submitted_at") ||
        row->textAt(0, "status") != "SHIPPED") {
        return complete(nullptr);  // уже отправлено или устарело
    }
    if (row->textAt(0, "platform_type") == "local") return complete("local");  // отправлять некуда

    const auto adapter = provider.adapterFor(row->intAt(0, "store_id"));
    if (!adapter) return fail(adapter.error().message, adapter.error().retryable(), orderId);
    const auto sent = (*adapter)->submitTrackingNumber(row->textAt(0, "external_order_id"),
                                                       row->textAt(0, "tracking_number"));
    if (sent) return complete("sent");
    if (sent.error().kind == store::StoreError::Kind::Unsupported) {
        return fail("the store does not accept tracking numbers automatically: enter it manually in the seller cabinet",
                    false, orderId);
    }
    return fail(sent.error().message, sent.error().retryable(), orderId);
}

db::Result<PollResult> pollStoreOrders(db::DatabaseManager& database, store::IAdapterProvider& provider, UnixSeconds now) {
    const auto stores = database.query(
        "SELECT id, last_orders_poll_at FROM stores WHERE is_active = 1 AND platform_type != 'local' ORDER BY id");
    if (!stores) return unexpected(stores.error());
    PollResult out;
    for (std::size_t i = 0; i < stores->rows.size(); ++i) {
        const std::int64_t storeId = stores->intAt(i, "id");
        const auto adapter = provider.adapterFor(storeId);
        if (!adapter) continue;  // нет учётных данных: ошибку покажет синхронизация цен
        const auto orders = (*adapter)->fetchNewOrders(stores->intAt(i, "last_orders_poll_at"));
        if (!orders) {
            if (orders.error().kind != store::StoreError::Kind::Unsupported) {
                (void)database.execute("UPDATE stores SET last_error = ? WHERE id = ?",
                                       {"orders: " + orders.error().message, storeId});
            }
            continue;
        }
        bool allOk = true;
        for (const auto& o : *orders) {
            const auto r = ingestOrder(database, {storeId, o.externalOrderId, o.externalVariantId, o.quantity,
                                                  o.totalCents, o.buyerAddressJson}, now);
            if (!r) {
                allOk = false;
                (void)database.execute("UPDATE stores SET last_error = ? WHERE id = ?",
                                       {"orders: " + r.error().message, storeId});
            } else if (r->status != IngestStatus::Duplicate) {
                ++out.ingested;
            }
        }
        // Курсор двигаем только если приняли всё: иначе следующий опрос повторит (приём идемпотентен).
        if (allOk) {
            const auto u = database.execute("UPDATE stores SET last_orders_poll_at = ? WHERE id = ?", {now, storeId});
            if (!u) return unexpected(u.error());
        }
        ++out.stores;
    }
    return out;
}

}  // namespace dropship::orders
