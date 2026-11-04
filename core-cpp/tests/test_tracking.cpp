#include "TestDb.hpp"
#include "orders/Orders.hpp"
#include "orders/Tracking.hpp"

using namespace testdb;
using namespace dropship::orders;
using namespace dropship::store;

namespace {

class FakeAdapter : public IStoreAdapter {
public:
    std::string name() const override { return "fake"; }
    store::Result<std::vector<ItemResult>> updateVariantPriceAndStock(std::span<const VariantUpdate>) override { return std::vector<ItemResult>{}; }
    store::Result<void> submitTrackingNumber(const std::string& order, const std::string& number) override {
        sent.emplace_back(order, number);
        if (trackingError) return unexpected(*trackingError);
        return {};
    }
    store::Result<std::vector<StoreOrder>> fetchNewOrders(UnixSeconds since) override {
        polledSince.push_back(since);
        if (ordersError) return unexpected(*ordersError);
        return incoming;
    }
    std::vector<std::pair<std::string, std::string>> sent;
    std::vector<UnixSeconds> polledSince;
    std::vector<StoreOrder> incoming;
    std::optional<StoreError> trackingError, ordersError;
};

class Provider : public IAdapterProvider {
public:
    store::Result<IStoreAdapter*> adapterFor(std::int64_t) override { return &adapter; }
    FakeAdapter adapter;
};

struct Rig {
    TempDir dir;
    std::unique_ptr<DatabaseManager> db = openDb(dir);
    Fixture f = insertFixture(*db);
    Provider provider;
    std::int64_t orderId = 0;

    Rig() {
        unwrap(db->execute("UPDATE product_variants SET external_variant_id = 'OFFER-1' WHERE id = ?", {f.variantId}));
        unwrap(db->execute(
            "INSERT INTO pricing_rules(store_id, price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, "
            "marketplace_fee_percent, rounding_cents, min_stock_threshold, min_profit_cents) "
            "VALUES(?, 0, 9000000000000, 0, 0, 10.0, NULL, 1, 200)",
            {f.storeId}));
        orderId = unwrap(ingestOrder(*db, {f.storeId, "ORD-1", "OFFER-1", 1, 14499, "{}"}, 100)).orderId;
    }
    void ordered() { unwrap(db->execute("UPDATE orders SET status = 'ORDERED', supplier_order_id = 'S1'")); }
    std::string orderField(const char* column) {
        const auto rs = unwrap(db->query(std::string("SELECT ") + column + " AS v FROM orders"));
        if (rs.isNull(0, "v")) return "<null>";
        const auto& v = rs.at(0, "v");
        return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::to_string(std::get<std::int64_t>(v));
    }
    TrackingOutcome run(UnixSeconds now = 200) { return unwrap(processNextTrackingSync(*db, provider, now)); }
};

}  // namespace

TEST(tracking_marks_shipped_and_queues_one_sync_idempotently) {
    Rig rig;
    CHECK(!applyTracking(*rig.db, rig.orderId, "TRK1", "", 150).has_value());  // ещё не куплен
    rig.ordered();
    CHECK(unwrap(applyTracking(*rig.db, rig.orderId, "TRK1", "CDEK", 150)));
    CHECK_EQ(rig.orderField("status"), std::string("SHIPPED"));
    CHECK_EQ(rig.orderField("tracking_carrier"), std::string("CDEK"));
    CHECK(!unwrap(applyTracking(*rig.db, rig.orderId, "TRK1", "CDEK", 160)));
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue WHERE task_type = 'TRACKING_SYNC'"), 1);
    CHECK(unwrap(applyTracking(*rig.db, rig.orderId, "TRK2", "CDEK", 170)));  // номер исправили
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM task_queue WHERE task_type = 'TRACKING_SYNC'"), 2);
}

TEST(tracking_validates_input) {
    Rig rig;
    rig.ordered();
    CHECK(!applyTracking(*rig.db, rig.orderId, "", "", 1).has_value());
    CHECK(!applyTracking(*rig.db, rig.orderId, std::string(101, '1'), "", 1).has_value());
    CHECK(!applyTracking(*rig.db, rig.orderId, "A\nB", "", 1).has_value());
    CHECK(!applyTracking(*rig.db, 9999, "T", "", 1).has_value());
    CHECK_EQ(rig.orderField("status"), std::string("ORDERED"));
}

TEST(tracking_sync_sends_the_number_once) {
    Rig rig;
    rig.ordered();
    unwrap(applyTracking(*rig.db, rig.orderId, "TRK1", "", 150));
    CHECK(rig.run() == TrackingOutcome::Submitted);
    REQUIRE(rig.provider.adapter.sent.size() == 1);
    CHECK_EQ(rig.provider.adapter.sent[0].first, std::string("ORD-1"));
    CHECK_EQ(rig.provider.adapter.sent[0].second, std::string("TRK1"));
    CHECK_EQ(rig.orderField("tracking_submitted_at"), std::string("200"));
    CHECK(rig.run() == TrackingOutcome::NoTask);
}

TEST(tracking_sync_explains_when_the_store_cannot_accept_it) {
    Rig rig;
    rig.ordered();
    unwrap(applyTracking(*rig.db, rig.orderId, "TRK1", "", 150));
    rig.provider.adapter.trackingError = makeStoreError(StoreError::Kind::Unsupported, "not supported");
    CHECK(rig.run() == TrackingOutcome::Failed);
    CHECK_EQ(rig.orderField("status"), std::string("SHIPPED"));
    CHECK(rig.orderField("error_message").find("manually") != std::string::npos);
    CHECK_EQ(rig.provider.adapter.sent.size(), std::size_t{1});
}

TEST(tracking_sync_retries_transport_errors) {
    Rig rig;
    rig.ordered();
    unwrap(applyTracking(*rig.db, rig.orderId, "TRK1", "", 150));
    rig.provider.adapter.trackingError = makeStoreError(StoreError::Kind::Transport, "timeout");
    CHECK(rig.run(200) == TrackingOutcome::Retrying);
    CHECK(rig.run(210) == TrackingOutcome::NoTask);
    rig.provider.adapter.trackingError.reset();
    CHECK(rig.run(600) == TrackingOutcome::Submitted);
}

TEST(tracking_sync_ignores_stale_tasks_and_local_stores) {
    Rig rig;
    rig.ordered();
    unwrap(applyTracking(*rig.db, rig.orderId, "TRK1", "", 150));
    unwrap(rig.db->execute("UPDATE stores SET platform_type = 'local'"));
    CHECK(rig.run() == TrackingOutcome::Submitted);
    CHECK(rig.provider.adapter.sent.empty());
}

TEST(poll_ingests_new_orders_and_moves_the_cursor) {
    Rig rig;
    rig.provider.adapter.incoming = {{"ORD-1", "OFFER-1", 1, 14499, "{}"},
                                     {"ORD-2", "OFFER-1", 1, 14499, R"({"city":"Казань"})"}};
    const auto first = unwrap(pollStoreOrders(*rig.db, rig.provider, 1000));
    CHECK_EQ(first.ingested, 1);  // ORD-1 уже есть
    CHECK_EQ(first.stores, 1);
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM orders"), 2);
    unwrap(pollStoreOrders(*rig.db, rig.provider, 2000));
    CHECK(rig.provider.adapter.polledSince == (std::vector<UnixSeconds>{0, 1000}));
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM orders"), 2);
}

TEST(poll_tolerates_unsupported_and_records_other_errors) {
    Rig rig;
    rig.provider.adapter.ordersError = makeStoreError(StoreError::Kind::Unsupported, "no");
    CHECK_EQ(unwrap(pollStoreOrders(*rig.db, rig.provider, 1000)).stores, 0);
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM stores WHERE last_error IS NOT NULL"), 0);
    rig.provider.adapter.ordersError = makeStoreError(StoreError::Kind::Transport, "down");
    unwrap(pollStoreOrders(*rig.db, rig.provider, 2000));
    CHECK_EQ(scalarInt(*rig.db, "SELECT COUNT(*) FROM stores WHERE last_error LIKE 'orders:%'"), 1);
    CHECK_EQ(scalarInt(*rig.db, "SELECT last_orders_poll_at FROM stores"), 0);
}
