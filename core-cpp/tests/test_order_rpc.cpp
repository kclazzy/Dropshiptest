#include "TestDb.hpp"
#include "bridge/Handlers.hpp"
#include "orders/Orders.hpp"

using namespace testdb;
namespace br = dropship::bridge;

namespace {

struct Rig {
    TempDir dir;
    std::unique_ptr<DatabaseManager> db = openDb(dir);
    Fixture f = insertFixture(*db);
    br::SupplierSessionStore sessions;
    br::RpcRouter router;
    std::int64_t orderId = 0;
    std::int64_t nextId = 1;

    Rig() {
        unwrap(db->execute("UPDATE product_variants SET external_variant_id = 'OFFER-1' WHERE id = ?", {f.variantId}));
        unwrap(db->execute(
            "INSERT INTO pricing_rules(store_id, price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, "
            "marketplace_fee_percent, rounding_cents, min_stock_threshold, min_profit_cents) "
            "VALUES(?, 0, 9000000000000, 0, 0, 10.0, NULL, 1, 200)",
            {f.storeId}));
        orderId = unwrap(orders::ingestOrder(*db, {f.storeId, "ORD-1", "OFFER-1", 1, 14499, R"({"city":"Казань"})"}, 100)).orderId;
        br::registerCoreHandlers(router, {*db, sessions, [] { return UnixSeconds{500}; }, {}});
    }
    Json call(const std::string& method, Json params) {
        br::RpcRequest request;
        request.id = nextId++;
        request.method = method;
        request.params = std::move(params);
        return Json::parse(*router.handle(request));
    }
};

}  // namespace

TEST(order_rpc_fulfillment_and_tracking_flow) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE orders SET status = 'QUEUED_FOR_EXT'"));
    auto r = rig.call("ORDER_FULFILLMENT_RESULT", {{"order_id", rig.orderId}, {"status", "ordered"}, {"supplier_order_id", "S-9"}});
    CHECK(r["result"]["changed"] == true);
    r = rig.call("ORDER_FULFILLMENT_RESULT", {{"order_id", rig.orderId}, {"status", "ordered"}, {"supplier_order_id", "S-9"}});
    CHECK(r["result"]["changed"] == false);
    r = rig.call("ORDER_TRACKING_UPDATE", {{"order_id", rig.orderId}, {"tracking_number", "TRK1"}, {"carrier", "CDEK"}});
    CHECK(r["result"]["changed"] == true);
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue WHERE task_type = 'TRACKING_SYNC'"), 1);
}

TEST(order_rpc_rejects_bad_params) {
    Rig rig;
    for (const Json& p : {Json{{"status", "ordered"}}, Json{{"order_id", 0}, {"status", "ordered"}},
                          Json{{"order_id", 1}, {"status", "weird"}}, Json{{"order_id", 1}, {"status", "ordered"}},
                          Json{{"order_id", 9999}, {"status", "failed"}}}) {
        const auto r = rig.call("ORDER_FULFILLMENT_RESULT", p);
        CHECK(r.contains("error"));
        CHECK_EQ(r["error"]["code"].get<int>(), br::rpc_code::kInvalidParams);
    }
    CHECK(rig.call("ORDER_TRACKING_UPDATE", {{"order_id", rig.orderId}, {"tracking_number", "T"}}).contains("error"));  // ещё не куплен
}

TEST(order_rpc_lists_orders_with_address_and_supplier_link) {
    Rig rig;
    const auto r = rig.call("LIST_ORDERS", Json::object());
    REQUIRE(r["result"]["orders"].size() == 1);
    const Json& o = r["result"]["orders"][0];
    CHECK_EQ(o["status"].get<std::string>(), std::string("NEW"));
    CHECK_EQ(o["address"]["city"].get<std::string>(), std::string("Казань"));
    CHECK_EQ(o["sku"].get<std::string>(), std::string("SKU-1"));
    CHECK(o["tracking_number"].is_null());
    CHECK(rig.call("LIST_ORDERS", {{"status", "SHIPPED"}})["result"]["orders"].empty());
    CHECK(rig.call("LIST_ORDERS", nullptr).contains("result"));
}
