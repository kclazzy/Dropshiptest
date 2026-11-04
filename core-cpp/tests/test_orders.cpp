#include "TestDb.hpp"
#include "orders/Orders.hpp"

using namespace testdb;
using namespace dropship::orders;

namespace {

// Правило: наценки нет, комиссия 10 %, мин. прибыль 200 коп.
void addRule(DatabaseManager& db, std::int64_t storeId, double fee = 10.0, std::int64_t minProfit = 200) {
    unwrap(db.execute(
        "INSERT INTO pricing_rules(store_id, price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, "
        "marketplace_fee_percent, rounding_cents, min_stock_threshold, min_profit_cents) VALUES(?, 0, ?, 0, 0, ?, NULL, 1, ?)",
        {storeId, std::numeric_limits<std::int64_t>::max(), fee, minProfit}));
}

struct Rig {
    TempDir dir;
    std::unique_ptr<DatabaseManager> db = openDb(dir);
    Fixture f = insertFixture(*db);  // закупка 10000, остаток 10, розница 14499

    Rig() {
        unwrap(db->execute("UPDATE product_variants SET external_variant_id = 'OFFER-1' WHERE id = ?", {f.variantId}));
        addRule(*db, f.storeId);
    }
    NewOrder order(const std::string& id = "ORD-1", std::int64_t qty = 1, Cents total = 14499) {
        return NewOrder{f.storeId, id, "OFFER-1", qty, total, R"({"name":"Иван","phone":"+7000","city":"Москва","street":"Тверская 1"})"};
    }
    IngestResult ingest(const NewOrder& o, UnixSeconds now = 100) { return unwrap(ingestOrder(*db, o, now)); }
    std::string status(std::int64_t id) {
        return unwrap(db->query("SELECT status FROM orders WHERE id = ?", {id})).textAt(0, "status");
    }
};

}  // namespace

TEST(orders_profitable_order_is_queued_with_cost_and_profit) {
    Rig rig;
    const auto r = rig.ingest(rig.order());
    CHECK(r.status == IngestStatus::Queued);
    CHECK_EQ(rig.status(r.orderId), std::string("NEW"));
    const auto row = unwrap(rig.db->query("SELECT supplier_cost_cents, net_profit_cents FROM orders"));
    CHECK_EQ(row.intAt(0, "supplier_cost_cents"), std::int64_t{10000});
    CHECK_EQ(row.intAt(0, "net_profit_cents"), std::int64_t{14499 - 1450 - 10000});  // комиссия 10 % вверх
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue WHERE task_type = 'AUTO_ORDER'"), 1);
}

TEST(orders_cost_scales_with_quantity_and_includes_shipping) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE product_variants SET shipping_price_cents = 500"));
    const auto r = rig.ingest(rig.order("ORD-2", 3, 50000));
    CHECK(r.status == IngestStatus::Queued);
    CHECK_EQ(scalarInt(*rig.db, "SELECT supplier_cost_cents FROM orders"), 3 * (10000 + 500));
}

TEST(orders_duplicate_is_ignored_even_if_data_differs) {
    Rig rig;
    const auto first = rig.ingest(rig.order());
    auto changed = rig.order();
    changed.retailTotalCents = 1;
    const auto second = rig.ingest(changed);
    CHECK(second.status == IngestStatus::Duplicate);
    CHECK_EQ(second.orderId, first.orderId);
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM orders"), 1);
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue WHERE task_type = 'AUTO_ORDER'"), 1);
}

TEST(orders_unprofitable_order_goes_to_manual_review_without_a_task) {
    Rig rig;
    const auto r = rig.ingest(rig.order("LOW", 1, 10500));  // прибыль 10500-1050-10000 < 200
    CHECK(r.status == IngestStatus::NeedsReview);
    CHECK(r.reason.find("profit") != std::string::npos);
    CHECK_EQ(rig.status(r.orderId), std::string("MANUAL_REVIEW"));
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue"), 0);
    CHECK(unwrap(rig.db->query("SELECT error_message FROM orders")).textAt(0, "error_message").find("profit") != std::string::npos);
}

TEST(orders_review_reasons_stock_rule_and_unknown_variant) {
    Rig rig;
    auto big = rig.ingest(rig.order("BIG", 11, 999999));  // остаток 10
    CHECK(big.status == IngestStatus::NeedsReview);
    CHECK(big.reason.find("stock") != std::string::npos);

    unwrap(rig.db->execute("UPDATE product_variants SET is_in_stock = 0"));
    CHECK(rig.ingest(rig.order("OOS")).status == IngestStatus::NeedsReview);
    unwrap(rig.db->execute("UPDATE product_variants SET is_in_stock = 1"));

    unwrap(rig.db->execute("DELETE FROM pricing_rules"));
    const auto noRule = rig.ingest(rig.order("NORULE"));
    CHECK(noRule.status == IngestStatus::NeedsReview);
    CHECK(noRule.reason.find("rule") != std::string::npos);

    auto unknown = rig.order("UNK");
    unknown.externalVariantId = "NOPE";
    const auto u = rig.ingest(unknown);
    CHECK(u.status == IngestStatus::NeedsReview);
    CHECK(u.reason.find("not linked") != std::string::npos);
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue"), 0);
}

TEST(orders_validate_input_and_never_store_garbage) {
    Rig rig;
    for (auto mutate : std::vector<std::function<void(NewOrder&)>>{
             [](NewOrder& o) { o.externalOrderId = ""; },
             [](NewOrder& o) { o.externalOrderId = std::string(201, 'x'); },
             [](NewOrder& o) { o.quantity = 0; },
             [](NewOrder& o) { o.quantity = kMaxQuantity + 1; },
             [](NewOrder& o) { o.retailTotalCents = 0; },
             [](NewOrder& o) { o.buyerAddressJson = "not json"; },
             [](NewOrder& o) { o.buyerAddressJson = "[]"; },
             [](NewOrder& o) { o.buyerAddressJson = "{\"x\":\"" + std::string(kMaxAddressBytes, 'a') + "\"}"; },
             [](NewOrder& o) { o.storeId = 9999; }}) {
        auto o = rig.order();
        mutate(o);
        CHECK(!ingestOrder(*rig.db, o, 100).has_value());
    }
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM orders"), 0);
}

TEST(orders_retry_requeues_only_orders_waiting_for_a_human) {
    Rig rig;
    const auto low = rig.ingest(rig.order("LOW", 1, 10500));
    CHECK(!unwrap(retryOrder(*rig.db, low.orderId, 200)) );        // всё ещё невыгодно: остаётся на проверке
    CHECK_EQ(rig.status(low.orderId), std::string("MANUAL_REVIEW"));
    unwrap(rig.db->execute("UPDATE orders SET retail_total_cents = 14499 WHERE id = ?", {low.orderId}));
    CHECK(unwrap(retryOrder(*rig.db, low.orderId, 300)));
    CHECK_EQ(rig.status(low.orderId), std::string("NEW"));
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue WHERE task_type = 'AUTO_ORDER'"), 1);
    CHECK(!unwrap(retryOrder(*rig.db, low.orderId, 400)));          // уже в очереди: второй раз не ставится
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue WHERE task_type = 'AUTO_ORDER'"), 1);
    CHECK(!unwrap(retryOrder(*rig.db, 9999, 400)));
    unwrap(rig.db->execute("UPDATE orders SET status = 'ORDERED'"));
    CHECK(!unwrap(retryOrder(*rig.db, low.orderId, 500)));          // выкупленный заказ повторно не выкупается
}
