#include <deque>

#include "TestDb.hpp"
#include "orders/AutoOrder.hpp"
#include "orders/Orders.hpp"

using namespace testdb;
using namespace dropship::orders;
namespace br = dropship::bridge;

namespace {

class FakeLink : public IExtensionLink {
public:
    bool connected() const override { return isConnected; }
    Expected<Json, br::RpcError> call(const std::string& method, const Json& params, std::chrono::milliseconds) override {
        methods.push_back(method);
        commands.push_back(params);
        if (replies.empty()) return Json{{"accepted", true}};
        auto r = std::move(replies.front());
        replies.pop_front();
        return r;
    }
    void failWith(int code, const std::string& message) { replies.emplace_back(unexpected(br::RpcError{code, message})); }

    bool isConnected = true;
    std::deque<Expected<Json, br::RpcError>> replies;
    std::vector<std::string> methods;
    std::vector<Json> commands;
};

struct Rig {
    TempDir dir;
    std::unique_ptr<DatabaseManager> db = openDb(dir);
    Fixture f = insertFixture(*db);
    FakeLink link;
    std::int64_t orderId = 0;

    Rig() {
        unwrap(db->execute("UPDATE product_variants SET external_variant_id = 'OFFER-1' WHERE id = ?", {f.variantId}));
        unwrap(db->execute(
            "INSERT INTO pricing_rules(store_id, price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, "
            "marketplace_fee_percent, rounding_cents, min_stock_threshold, min_profit_cents) "
            "VALUES(?, 0, 9000000000000, 0, 0, 10.0, NULL, 1, 200)",
            {f.storeId}));
        NewOrder o{f.storeId, "ORD-1", "OFFER-1", 2, 30000, R"({"name":"Иван","city":"Москва"})"};
        orderId = unwrap(ingestOrder(*db, o, 100)).orderId;
    }
    OrderTaskOutcome run(UnixSeconds now = 100) { return unwrap(processNextOrderTask(*db, link, now, std::chrono::seconds(60))); }
    std::string status() { return unwrap(db->query("SELECT status FROM orders WHERE id = ?", {orderId})).textAt(0, "status"); }
    std::string taskStatus() { return unwrap(db->query("SELECT status FROM task_queue")).textAt(0, "status"); }
};

}  // namespace

TEST(autoorder_sends_the_purchase_command_once) {
    Rig rig;
    CHECK(rig.run() == OrderTaskOutcome::Sent);
    CHECK_EQ(rig.status(), std::string("QUEUED_FOR_EXT"));
    CHECK_EQ(rig.taskStatus(), std::string("COMPLETED"));
    REQUIRE(rig.link.commands.size() == 1);
    CHECK_EQ(rig.link.methods[0], std::string("EXECUTE_AUTO_ORDER"));
    const Json& c = rig.link.commands[0];
    CHECK_EQ(c["order_id"].get<std::int64_t>(), rig.orderId);
    CHECK_EQ(c["sku"].get<std::string>(), std::string("SKU-1"));
    CHECK_EQ(c["quantity"].get<int>(), 2);
    CHECK_EQ(c["address"]["city"].get<std::string>(), std::string("Москва"));
    CHECK(c["auto_pay"] == false);
    CHECK(rig.run() == OrderTaskOutcome::NoTask);
    CHECK_EQ(rig.link.commands.size(), std::size_t{1});
}

TEST(autoorder_defers_without_spending_an_attempt_when_extension_is_offline) {
    Rig rig;
    rig.link.isConnected = false;
    CHECK(rig.run(100) == OrderTaskOutcome::Deferred);
    CHECK_EQ(rig.status(), std::string("NEW"));
    CHECK(rig.run(159) == OrderTaskOutcome::NoTask);
    rig.link.isConnected = true;
    CHECK(rig.run(160) == OrderTaskOutcome::Sent);
    CHECK_EQ(scalarInt(*rig.db, "SELECT attempts FROM task_queue"), 1);
}

TEST(autoorder_rechecks_profit_before_sending) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE product_variants SET supplier_price_cents = 20000"));  // поставщик подорожал
    CHECK(rig.run() == OrderTaskOutcome::Review);
    CHECK_EQ(rig.status(), std::string("MANUAL_REVIEW"));
    CHECK(rig.link.commands.empty());
    CHECK_EQ(rig.taskStatus(), std::string("COMPLETED"));
}

TEST(autoorder_skips_orders_that_are_no_longer_new) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE orders SET status = 'ORDERED'"));
    CHECK(rig.run() == OrderTaskOutcome::Skipped);
    CHECK(rig.link.commands.empty());
}

TEST(autoorder_rejection_returns_the_order_and_retries_then_escalates) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE task_queue SET max_attempts = 2"));
    rig.link.failWith(br::rpc_code::kInvalidParams, "no supplier tab");
    rig.link.failWith(br::rpc_code::kInvalidParams, "no supplier tab");
    CHECK(rig.run(100) == OrderTaskOutcome::Rejected);
    CHECK_EQ(rig.status(), std::string("NEW"));
    CHECK(rig.run(159) == OrderTaskOutcome::NoTask);
    CHECK(rig.run(160) == OrderTaskOutcome::Review);
    CHECK_EQ(rig.status(), std::string("MANUAL_REVIEW"));
}

TEST(autoorder_unknown_outcome_is_never_retried_automatically) {
    for (const int code : {br::rpc_code::kTimeout, br::rpc_code::kExtensionNotConnected}) {
        Rig rig;
        rig.link.failWith(code, "gone");
        CHECK(rig.run() == OrderTaskOutcome::Review);
        CHECK_EQ(rig.status(), std::string("MANUAL_REVIEW"));
        CHECK(rig.run(100000) == OrderTaskOutcome::NoTask);
        CHECK_EQ(rig.link.commands.size(), std::size_t{1});
        CHECK(unwrap(rig.db->query("SELECT error_message FROM orders")).textAt(0, "error_message").find("unknown") !=
              std::string::npos);
    }
}

TEST(autoorder_fails_broken_payload_permanently) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE task_queue SET payload_json = 'oops'"));
    CHECK(rig.run() == OrderTaskOutcome::Failed);
    CHECK_EQ(rig.taskStatus(), std::string("FAILED"));
}

TEST(fulfillment_transitions_are_idempotent_and_guarded) {
    Rig rig;
    rig.run();  // QUEUED_FOR_EXT
    Fulfillment ok{rig.orderId, FulfillmentStatus::Ordered, "SUP-77", ""};
    CHECK(unwrap(applyFulfillment(*rig.db, ok, 200)));
    CHECK_EQ(rig.status(), std::string("ORDERED"));
    CHECK_EQ(unwrap(rig.db->query("SELECT supplier_order_id FROM orders")).textAt(0, "supplier_order_id"), std::string("SUP-77"));
    CHECK(!unwrap(applyFulfillment(*rig.db, ok, 300)));  // повтор ничего не меняет
    // Запоздавший отказ не откатывает выкупленный заказ.
    CHECK(!unwrap(applyFulfillment(*rig.db, {rig.orderId, FulfillmentStatus::Failed, "", "late"}, 400)));
    CHECK(!unwrap(applyFulfillment(*rig.db, {rig.orderId, FulfillmentStatus::NeedsManual, "", "late"}, 400)));
    CHECK_EQ(rig.status(), std::string("ORDERED"));
}

TEST(fulfillment_validates_input_and_handles_failures) {
    Rig rig;
    CHECK(!applyFulfillment(*rig.db, {rig.orderId, FulfillmentStatus::Ordered, "", ""}, 1).has_value());
    CHECK(!applyFulfillment(*rig.db, {9999, FulfillmentStatus::Failed, "", ""}, 1).has_value());
    CHECK(!applyFulfillment(*rig.db, {rig.orderId, FulfillmentStatus::Ordered, std::string(201, 'x'), ""}, 1).has_value());
    rig.run();
    CHECK(unwrap(applyFulfillment(*rig.db, {rig.orderId, FulfillmentStatus::Failed, "", "card declined"}, 5)));
    CHECK_EQ(rig.status(), std::string("ERROR"));
    CHECK_EQ(unwrap(rig.db->query("SELECT error_message FROM orders")).textAt(0, "error_message"), std::string("card declined"));
    // Человек выкупил вручную и сообщил номер: ERROR -> ORDERED допустимо.
    CHECK(unwrap(applyFulfillment(*rig.db, {rig.orderId, FulfillmentStatus::Ordered, "MANUAL-1", ""}, 6)));
    CHECK_EQ(rig.status(), std::string("ORDERED"));
}

#include <thread>
#include "orders/OrderService.hpp"

TEST(order_service_thread_sends_the_command_and_stops_quickly) {
    Rig rig;
    const auto started = std::chrono::steady_clock::now();
    {
        OrderService service(*rig.db, rig.link, nullptr, [] { return UnixSeconds{100}; });
        for (int i = 0; i < 200 && scalarInt(*rig.db, "SELECT COUNT(*) FROM orders WHERE status = 'QUEUED_FOR_EXT'") == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_EQ(rig.link.commands.size(), std::size_t{1});
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(2500));
}
