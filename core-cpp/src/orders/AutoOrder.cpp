#include "orders/AutoOrder.hpp"

#include "core/Json.hpp"
#include "orders/Orders.hpp"

namespace dropship::orders {

namespace {

using db::DbError;

struct Prepared {
    enum class Kind { Skip, Review, Go } kind = Kind::Skip;
    std::string reason;
    Json command;
};

// Одна транзакция: проверка заново (цены могли измениться) и захват заказа.
db::Result<Prepared> prepare(db::DatabaseManager& database, std::int64_t orderId, UnixSeconds now) {
    Prepared out;
    const auto done = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        const auto row = tx.query(
            "SELECT o.store_id, o.variant_id, o.quantity, o.retail_total_cents, o.status, o.buyer_address_json, "
            "p.supplier_url, v.supplier_sku FROM orders o "
            "LEFT JOIN product_variants v ON v.id = o.variant_id LEFT JOIN products p ON p.id = v.product_id "
            "WHERE o.id = ?",
            {orderId});
        if (!row) return unexpected(row.error());
        if (row->rows.empty() || row->textAt(0, "status") != "NEW") return {};  // Skip
        Evaluation ev;
        if (row->isNull(0, "variant_id")) ev.reason = "variant not linked to a supplier product";
        else ev = evaluateOrder(tx, row->intAt(0, "store_id"), row->intAt(0, "variant_id"), row->intAt(0, "quantity"),
                                row->intAt(0, "retail_total_cents"));
        if (!ev.ok) {
            const auto upd = tx.execute(
                "UPDATE orders SET status = 'MANUAL_REVIEW', error_message = ?, updated_at = ? "
                "WHERE id = ? AND status = 'NEW'",
                {ev.reason, now, orderId});
            if (!upd) return unexpected(upd.error());
            out = {Prepared::Kind::Review, ev.reason, nullptr};
            return {};
        }
        const auto upd = tx.execute(
            "UPDATE orders SET status = 'QUEUED_FOR_EXT', supplier_cost_cents = ?, net_profit_cents = ?, "
            "updated_at = ? WHERE id = ? AND status = 'NEW'",
            {ev.costCents, ev.netProfitCents, now, orderId});
        if (!upd) return unexpected(upd.error());
        if (upd->changes != 1) return {};
        out.kind = Prepared::Kind::Go;
        out.command = Json{{"order_id", orderId},
                           {"url", row->textAt(0, "supplier_url")},
                           {"sku", row->textAt(0, "supplier_sku")},
                           {"quantity", row->intAt(0, "quantity")},
                           {"address", parseJson(row->textAt(0, "buyer_address_json"))},
                           {"auto_pay", false}};
        return {};
    });
    if (!done) return unexpected(done.error());
    return out;
}

db::Result<void> setStatus(db::DatabaseManager& database, std::int64_t orderId, const char* from, const char* to,
                           const std::string& message, UnixSeconds now) {
    const auto r = database.execute(
        std::string("UPDATE orders SET status = ?, error_message = ?, updated_at = ? WHERE id = ? AND status = '") +
            from + "'",
        {std::string(to), message.empty() ? db::Value{} : db::Value{message}, now, orderId});
    if (!r) return unexpected(r.error());
    return {};
}

}  // namespace

db::Result<OrderTaskOutcome> processNextOrderTask(db::DatabaseManager& database, IExtensionLink& link, UnixSeconds now,
                                                  std::chrono::seconds retryDelay,
                                                  std::chrono::milliseconds callTimeout) {
    const TaskType types[] = {TaskType::AutoOrder};
    const auto claimed = database.claimNextTask(now, std::chrono::minutes(10), types);
    if (!claimed) return unexpected(claimed.error());
    if (!*claimed) return OrderTaskOutcome::NoTask;
    const db::Task& task = **claimed;

    const Json payload = parseJson(task.payloadJson);
    const auto id = payload.is_object() && payload.contains("order_id") ? asInt64(payload["order_id"]) : std::nullopt;
    if (!id) {
        const auto f = database.failTask(task.id, "invalid AUTO_ORDER payload", now, retryDelay, false);
        if (!f) return unexpected(f.error());
        return OrderTaskOutcome::Failed;
    }
    const std::int64_t orderId = *id;

    if (!link.connected()) {
        const auto d = database.deferTask(task.id, now + retryDelay.count());
        if (!d) return unexpected(d.error());
        return OrderTaskOutcome::Deferred;
    }

    const auto prepared = prepare(database, orderId, now);
    if (!prepared) return unexpected(prepared.error());
    if (prepared->kind != Prepared::Kind::Go) {
        const auto c = database.completeTask(task.id);
        if (!c) return unexpected(c.error());
        return prepared->kind == Prepared::Kind::Review ? OrderTaskOutcome::Review : OrderTaskOutcome::Skipped;
    }

    // Сетевой вызов — вне транзакции.
    const auto reply = link.call("EXECUTE_AUTO_ORDER", prepared->command, callTimeout);
    if (reply) {
        const auto c = database.completeTask(task.id);
        if (!c) return unexpected(c.error());
        return OrderTaskOutcome::Sent;
    }
    const int code = reply.error().code;
    const bool unknownOutcome = code == bridge::rpc_code::kTimeout || code == bridge::rpc_code::kExtensionNotConnected;
    const std::string message = "extension: " + reply.error().message;
    if (unknownOutcome) {
        // Возможно, покупка уже началась: автоматически не повторяем.
        const auto s = setStatus(database, orderId, "QUEUED_FOR_EXT", "MANUAL_REVIEW",
                                 message + " (outcome unknown, check the supplier account before retrying)", now);
        if (!s) return unexpected(s.error());
        const auto c = database.completeTask(task.id);
        if (!c) return unexpected(c.error());
        return OrderTaskOutcome::Review;
    }
    // Явный отказ: ничего не куплено.
    const auto back = setStatus(database, orderId, "QUEUED_FOR_EXT", "NEW", message, now);
    if (!back) return unexpected(back.error());
    const auto f = database.failTask(task.id, message, now, retryDelay, true);
    if (!f) return unexpected(f.error());
    if (*f == db::FailOutcome::WillRetry) return OrderTaskOutcome::Rejected;
    const auto s = setStatus(database, orderId, "NEW", "MANUAL_REVIEW", message + " (attempts exhausted)", now);
    if (!s) return unexpected(s.error());
    return OrderTaskOutcome::Review;
}

db::Result<bool> applyFulfillment(db::DatabaseManager& database, const Fulfillment& result, UnixSeconds now) {
    if (result.supplierOrderId.size() > 200 || result.message.size() > 2000) {
        return unexpected(DbError{0, "fulfillment fields too long"});
    }
    if (result.status == FulfillmentStatus::Ordered && result.supplierOrderId.empty()) {
        return unexpected(DbError{0, "supplier_order_id is required for status ordered"});
    }
    bool changed = false;
    const auto done = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        const auto row = tx.query("SELECT status FROM orders WHERE id = ?", {result.orderId});
        if (!row) return unexpected(row.error());
        if (row->rows.empty()) return unexpected(DbError{0, "order not found"});
        const std::string& status = row->textAt(0, "status");
        Expected<db::ExecResult, DbError> upd = db::ExecResult{};
        switch (result.status) {
            case FulfillmentStatus::Ordered:
                upd = tx.execute(
                    "UPDATE orders SET status = 'ORDERED', supplier_order_id = ?, error_message = NULL, updated_at = ? "
                    "WHERE id = ? AND status IN ('NEW','QUEUED_FOR_EXT','MANUAL_REVIEW','ERROR')",
                    {result.supplierOrderId, now, result.orderId});
                break;
            case FulfillmentStatus::Failed:
                upd = tx.execute(
                    "UPDATE orders SET status = 'ERROR', error_message = ?, updated_at = ? "
                    "WHERE id = ? AND status = 'QUEUED_FOR_EXT'",
                    {result.message.empty() ? std::string("purchase failed") : result.message, now, result.orderId});
                break;
            case FulfillmentStatus::NeedsManual:
                upd = tx.execute(
                    "UPDATE orders SET status = 'MANUAL_REVIEW', error_message = ?, updated_at = ? "
                    "WHERE id = ? AND status IN ('NEW','QUEUED_FOR_EXT')",
                    {result.message.empty() ? std::string("needs manual action") : result.message, now, result.orderId});
                break;
        }
        (void)status;
        if (!upd) return unexpected(upd.error());
        changed = upd->changes == 1;
        return {};
    });
    if (!done) return unexpected(done.error());
    return changed;
}

}  // namespace dropship::orders
