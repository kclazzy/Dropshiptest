#include "orders/Orders.hpp"

#include "core/Json.hpp"
#include "pricing/RuleRepository.hpp"

namespace dropship::orders {

namespace {

using db::DbError;

db::Result<IngestResult> bad(std::string message) { return unexpected(DbError{0, std::move(message)}); }

struct Check {
    bool ok = false;
    std::string reason;
    Cents cost = 0;
    Cents profit = 0;
};

Check evaluate(db::Tx& tx, std::int64_t storeId, std::int64_t variantId, std::int64_t quantity, Cents retail) {
    Check out;
    const auto row = tx.query(
        "SELECT supplier_price_cents, COALESCE(shipping_price_cents, 0) AS ship, supplier_stock, is_in_stock "
        "FROM product_variants WHERE id = ?",
        {variantId});
    if (!row || row->rows.empty()) {
        out.reason = "variant not linked to a supplier product";
        return out;
    }
    if (row->intAt(0, "is_in_stock") == 0 || row->intAt(0, "supplier_stock") < quantity) {
        out.reason = "insufficient stock at the supplier";
        return out;
    }
    const Cents unit = row->intAt(0, "supplier_price_cents");
    const Cents ship = row->intAt(0, "ship");
    Cents unitTotal = 0;
    if (__builtin_add_overflow(unit, ship, &unitTotal) || __builtin_mul_overflow(unitTotal, quantity, &out.cost)) {
        out.reason = "cost overflow";
        return out;
    }
    const auto rules = pricing::loadRuleSet(tx, storeId);
    const pricing::PricingRule* rule = rules ? rules->find(unit) : nullptr;
    if (!rule) {
        out.reason = "no pricing rule for this price";
        return out;
    }
    const auto profit = pricing::netProfit(*rule, retail, out.cost);
    if (!profit) {
        out.reason = "profit cannot be calculated";
        return out;
    }
    out.profit = *profit;
    if (out.profit < rule->minProfitCents()) {
        out.reason = "profit below the minimum (" + std::to_string(out.profit) + " < " +
                     std::to_string(rule->minProfitCents()) + ")";
        return out;
    }
    out.ok = true;
    return out;
}

db::Result<void> enqueueOrderTask(db::Tx& tx, std::int64_t orderId, UnixSeconds now) {
    const auto task = db::DatabaseManager::enqueueTaskIn(tx, TaskType::AutoOrder, 3,
                                                         dumpJson(Json{{"order_id", orderId}}), now, 5);
    if (!task) return unexpected(task.error());
    return {};
}

}  // namespace

Evaluation evaluateOrder(db::Tx& tx, std::int64_t storeId, std::int64_t variantId, std::int64_t quantity,
                         Cents retailTotalCents) {
    const Check c = evaluate(tx, storeId, variantId, quantity, retailTotalCents);
    return Evaluation{c.ok, c.reason, c.cost, c.profit};
}

db::Result<IngestResult> ingestOrder(db::DatabaseManager& database, const NewOrder& order, UnixSeconds now) {
    if (order.externalOrderId.empty() || order.externalOrderId.size() > 200) return bad("invalid external order id");
    if (order.quantity < 1 || order.quantity > kMaxQuantity) return bad("invalid quantity");
    if (order.retailTotalCents <= 0) return bad("invalid order total");
    if (order.buyerAddressJson.size() > kMaxAddressBytes) return bad("buyer address too large");
    const Json address = parseJson(order.buyerAddressJson);
    if (address.is_discarded() || !address.is_object()) return bad("buyer address must be a JSON object");

    IngestResult result;
    const auto done = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        const auto existing = tx.query("SELECT id FROM orders WHERE external_order_id = ?", {order.externalOrderId});
        if (!existing) return unexpected(existing.error());
        if (!existing->rows.empty()) {
            result = {existing->intAt(0, "id"), IngestStatus::Duplicate, ""};
            return {};
        }
        const auto store = tx.query("SELECT id FROM stores WHERE id = ?", {order.storeId});
        if (!store) return unexpected(store.error());
        if (store->rows.empty()) return unexpected(DbError{0, "store not found"});

        std::optional<std::int64_t> variantId;
        const auto variant = tx.query(
            "SELECT v.id FROM product_variants v JOIN products p ON p.id = v.product_id "
            "WHERE p.store_id = ? AND v.external_variant_id = ? ORDER BY v.id LIMIT 1",
            {order.storeId, order.externalVariantId});
        if (!variant) return unexpected(variant.error());
        if (!variant->rows.empty()) variantId = variant->intAt(0, "id");

        Check check;
        if (variantId) check = evaluate(tx, order.storeId, *variantId, order.quantity, order.retailTotalCents);
        else check.reason = "variant not linked to a supplier product";

        db::Value variantValue = variantId ? db::Value{*variantId} : db::Value{};
        const auto inserted = tx.execute(
            "INSERT INTO orders(store_id, external_order_id, variant_id, quantity, buyer_address_json, "
            "retail_total_cents, supplier_cost_cents, net_profit_cents, status, error_message, created_at, updated_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            {order.storeId, order.externalOrderId, variantValue, order.quantity, order.buyerAddressJson,
             order.retailTotalCents, variantId ? db::Value{check.cost} : db::Value{},
             variantId ? db::Value{check.profit} : db::Value{}, std::string(check.ok ? "NEW" : "MANUAL_REVIEW"),
             check.ok ? db::Value{} : db::Value{check.reason}, now, now});
        if (!inserted) return unexpected(inserted.error());
        result.orderId = inserted->lastInsertRowId;
        result.status = check.ok ? IngestStatus::Queued : IngestStatus::NeedsReview;
        result.reason = check.reason;
        if (check.ok) return enqueueOrderTask(tx, result.orderId, now);
        return {};
    });
    if (!done) return unexpected(done.error());
    return result;
}

db::Result<bool> retryOrder(db::DatabaseManager& database, std::int64_t orderId, UnixSeconds now) {
    bool requeued = false;
    const auto done = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        const auto row = tx.query(
            "SELECT store_id, variant_id, quantity, retail_total_cents, status FROM orders WHERE id = ?", {orderId});
        if (!row) return unexpected(row.error());
        if (row->rows.empty()) return {};
        const std::string& status = row->textAt(0, "status");
        if (status != "MANUAL_REVIEW" && status != "ERROR") return {};
        std::string reason = "variant not linked to a supplier product";
        Check check;
        if (!row->isNull(0, "variant_id")) {
            check = evaluate(tx, row->intAt(0, "store_id"), row->intAt(0, "variant_id"), row->intAt(0, "quantity"),
                             row->intAt(0, "retail_total_cents"));
            reason = check.reason;
        }
        if (!check.ok) {
            const auto upd = tx.execute("UPDATE orders SET error_message = ?, updated_at = ? WHERE id = ?",
                                        {reason, now, orderId});
            if (!upd) return unexpected(upd.error());
            return {};
        }
        const auto upd = tx.execute(
            "UPDATE orders SET status = 'NEW', error_message = NULL, supplier_cost_cents = ?, net_profit_cents = ?, "
            "updated_at = ? WHERE id = ? AND status IN ('MANUAL_REVIEW','ERROR')",
            {check.cost, check.profit, now, orderId});
        if (!upd) return unexpected(upd.error());
        if (upd->changes != 1) return {};
        requeued = true;
        return enqueueOrderTask(tx, orderId, now);
    });
    if (!done) return unexpected(done.error());
    return requeued;
}

}  // namespace dropship::orders
