#include <atomic>
#include <deque>

#include "FakeTransport.hpp"
#include "TestDb.hpp"
#include "store/StoreSync.hpp"

using namespace testdb;
using namespace dropship::store;

namespace {

class FakeAdapter : public IStoreAdapter {
public:
    std::string name() const override { return "fake"; }
    store::Result<std::vector<ItemResult>> updateVariantPriceAndStock(std::span<const VariantUpdate> updates) override {
        calls.emplace_back(updates.begin(), updates.end());
        if (!scripted.empty()) {
            auto next = std::move(scripted.front());
            scripted.pop_front();
            return next;
        }
        std::vector<ItemResult> ok;
        for (const auto& u : updates) ok.push_back({u.variantId, true, u.retailCents > 0, true, ""});
        return ok;
    }
    std::deque<store::Result<std::vector<ItemResult>>> scripted;
    std::vector<std::vector<VariantUpdate>> calls;
};

class FakeProvider : public IAdapterProvider {
public:
    store::Result<IStoreAdapter*> adapterFor(std::int64_t) override {
        if (error) return unexpected(*error);
        return &adapter;
    }
    FakeAdapter adapter;
    std::optional<StoreError> error;
};

struct Rig {
    TempDir dir;
    std::unique_ptr<DatabaseManager> db = openDb(dir);
    FakeProvider provider;
    Fixture f = insertFixture(*db);  // магазин "ozon", вариант SKU-1: закупка 10000, розница 14499, остаток 10

    Rig() { link("OFFER-1"); }
    void link(const std::string& external) {
        unwrap(db->execute("UPDATE product_variants SET external_variant_id = ? WHERE id = ?", {external, f.variantId}));
    }
    int schedule(UnixSeconds now = 1000) { return unwrap(scheduleStoreSync(*db, now)); }
    StoreSyncOutcome run(UnixSeconds now = 1000) { return unwrap(processNextStorePublish(*db, provider, now)); }
    std::string publishedHash() {
        const auto rs = unwrap(db->query("SELECT published_hash FROM product_variants WHERE id = ?", {f.variantId}));
        return rs.isNull(0, "published_hash") ? std::string("<null>") : rs.textAt(0, "published_hash");
    }
    std::string stateHash() {
        return unwrap(db->query("SELECT state_hash FROM product_variants WHERE id = ?", {f.variantId})).textAt(0, "state_hash");
    }
    std::string taskStatus() { return unwrap(db->query("SELECT status FROM task_queue ORDER BY id DESC LIMIT 1")).textAt(0, "status"); }
    std::string lastError() {
        const auto rs = unwrap(db->query("SELECT last_error FROM stores"));
        return rs.isNull(0, "last_error") ? std::string("<null>") : rs.textAt(0, "last_error");
    }
};

StoreError err(StoreError::Kind kind, int retryAfter = 0) { return makeStoreError(kind, "boom", 0, retryAfter); }

}  // namespace

TEST(storesync_publishes_linked_changes_once_and_remembers_it) {
    Rig rig;
    CHECK_EQ(rig.schedule(), 1);
    CHECK_EQ(rig.schedule(), 0);  // уже в очереди
    CHECK(rig.run() == StoreSyncOutcome::Published);
    REQUIRE(rig.provider.adapter.calls.size() == 1);
    const auto& u = rig.provider.adapter.calls[0][0];
    CHECK_EQ(u.externalId, std::string("OFFER-1"));
    CHECK_EQ(u.retailCents, Cents{14499});
    CHECK_EQ(u.stock, std::int64_t{10});
    CHECK_EQ(rig.publishedHash(), rig.stateHash());
    CHECK_EQ(rig.taskStatus(), std::string("COMPLETED"));
    CHECK_EQ(rig.schedule(2000), 0);  // ничего не менялось — новой задачи нет
}

TEST(storesync_sends_zero_stock_for_variants_below_the_threshold) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE product_variants SET supplier_stock = 3, is_in_stock = 0 WHERE id = ?", {rig.f.variantId}));
    rig.schedule();
    rig.run();
    CHECK_EQ(rig.provider.adapter.calls[0][0].stock, std::int64_t{0});
}

TEST(storesync_republishes_when_the_supplier_state_changes) {
    Rig rig;
    rig.schedule();
    rig.run(1000);
    unwrap(rig.db->execute("UPDATE product_variants SET supplier_price_cents = 12000, state_hash = 'new', updated_at = 1500 WHERE id = ?", {rig.f.variantId}));
    CHECK_EQ(rig.schedule(2000), 1);
    CHECK(rig.run(2000) == StoreSyncOutcome::Published);
    CHECK_EQ(rig.provider.adapter.calls.size(), std::size_t{2});
}

TEST(storesync_ignores_unlinked_variants_local_stores_and_inactive_stores) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE product_variants SET external_variant_id = NULL"));
    CHECK_EQ(rig.schedule(), 0);
    rig.link("X");
    unwrap(rig.db->execute("UPDATE stores SET platform_type = 'local'"));
    CHECK_EQ(rig.schedule(), 0);
    unwrap(rig.db->execute("UPDATE stores SET platform_type = 'ozon', is_active = 0"));
    CHECK_EQ(rig.schedule(), 0);
}

TEST(storesync_skips_a_task_for_a_store_that_became_local) {
    Rig rig;
    rig.schedule();
    unwrap(rig.db->execute("UPDATE stores SET platform_type = 'local'"));
    CHECK(rig.run() == StoreSyncOutcome::Skipped);
    CHECK(rig.provider.adapter.calls.empty());
}

TEST(storesync_partial_failure_is_recorded_and_not_retried_until_something_changes) {
    Rig rig;
    rig.provider.adapter.scripted.push_back(std::vector<ItemResult>{{rig.f.variantId, false, false, false, "PRODUCT_NOT_FOUND"}});
    rig.schedule(1000);
    CHECK(rig.run(1000) == StoreSyncOutcome::PartiallyPublished);
    CHECK(rig.lastError().find("PRODUCT_NOT_FOUND") != std::string::npos);
    CHECK_EQ(rig.publishedHash(), std::string("<null>"));
    CHECK_EQ(rig.schedule(5000), 0);  // отклонённый вариант не долбим каждые 2 секунды
    unwrap(rig.db->execute("UPDATE product_variants SET state_hash = 'changed', updated_at = 5000"));
    CHECK_EQ(rig.schedule(5000), 1);  // изменилось состояние — пробуем снова, даже в ту же секунду
}

TEST(storesync_relinking_a_variant_triggers_a_fresh_attempt) {
    Rig rig;
    rig.provider.adapter.scripted.push_back(std::vector<ItemResult>{{rig.f.variantId, false, false, false, "no such offer"}});
    rig.schedule(1000);
    rig.run(1000);
    CHECK_EQ(rig.schedule(5000), 0);
    rig.link("OFFER-FIXED");  // пользователь исправил привязку
    CHECK_EQ(rig.schedule(5000), 1);
    CHECK(rig.run(5000) == StoreSyncOutcome::Published);
    CHECK_EQ(rig.provider.adapter.calls.back()[0].externalId, std::string("OFFER-FIXED"));
    CHECK_EQ(rig.lastError(), std::string("<null>"));  // успешная отправка снимает прежнюю ошибку
}

TEST(storesync_rate_limit_defers_without_spending_an_attempt) {
    Rig rig;
    rig.provider.adapter.scripted.push_back(unexpected(err(StoreError::Kind::RateLimited, 90)));
    rig.schedule();
    CHECK(rig.run(1000) == StoreSyncOutcome::Deferred);
    CHECK_EQ(scalarInt(*rig.db, "SELECT scheduled_at FROM task_queue"), 1090);
    CHECK_EQ(scalarInt(*rig.db, "SELECT attempts FROM task_queue"), 0);
    CHECK(rig.run(1089) == StoreSyncOutcome::NoTask);
    CHECK(rig.run(1090) == StoreSyncOutcome::Published);
}

TEST(storesync_auth_and_config_errors_fail_at_once_and_tell_the_user) {
    for (auto kind : {StoreError::Kind::Auth, StoreError::Kind::Config, StoreError::Kind::Rejected}) {
        Rig rig;
        rig.provider.adapter.scripted.push_back(unexpected(err(kind)));
        rig.schedule();
        CHECK(rig.run() == StoreSyncOutcome::Failed);
        CHECK_EQ(rig.taskStatus(), std::string("FAILED"));
        CHECK_EQ(rig.lastError(), std::string("boom"));
    }
}

TEST(storesync_transport_errors_retry_later) {
    Rig rig;
    rig.provider.adapter.scripted.push_back(unexpected(err(StoreError::Kind::Transport)));
    rig.schedule();
    CHECK(rig.run(1000) == StoreSyncOutcome::Retrying);
    CHECK_EQ(rig.taskStatus(), std::string("PENDING"));
    CHECK(rig.run(1000 + 299) == StoreSyncOutcome::NoTask);
    CHECK(rig.run(1000 + 300) == StoreSyncOutcome::Published);
}

TEST(storesync_provider_failures_are_reported) {
    Rig rig;
    rig.provider.error = err(StoreError::Kind::Config);
    rig.schedule();
    CHECK(rig.run() == StoreSyncOutcome::Failed);
}

TEST(storesync_does_not_mark_published_if_the_state_changed_during_the_request) {
    Rig rig;
    rig.schedule();
    struct Racing : FakeAdapter {
        DatabaseManager* db = nullptr;
        store::Result<std::vector<ItemResult>> updateVariantPriceAndStock(std::span<const VariantUpdate> u) override {
            unwrap(db->execute("UPDATE product_variants SET state_hash = 'changed-meanwhile', updated_at = 2000"));
            return FakeAdapter::updateVariantPriceAndStock(u);
        }
    };
    struct RacingProvider : IAdapterProvider {
        Racing adapter;
        store::Result<IStoreAdapter*> adapterFor(std::int64_t) override { return &adapter; }
    } provider;
    provider.adapter.db = rig.db.get();
    CHECK(unwrap(processNextStorePublish(*rig.db, provider, 1000)) == StoreSyncOutcome::Published);
    CHECK_EQ(rig.publishedHash(), std::string("<null>"));  // новое состояние ещё не отправлено
    CHECK_EQ(rig.schedule(3000), 1);
}

TEST(storesync_service_publishes_in_the_background) {
    Rig rig;
    std::atomic<int> logged{0};
    {
        StoreSyncService service(*rig.db, rig.provider, [] { return UnixSeconds{1000}; }, [&](const std::string&) { ++logged; });
        for (int i = 0; i < 200 && rig.publishedHash() == "<null>"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK_EQ(rig.publishedHash(), rig.stateHash());
}

TEST(storesync_picks_up_a_change_made_in_the_same_second_as_the_previous_publish) {
    Rig rig;
    rig.schedule(1000);
    rig.run(1000);
    unwrap(rig.db->execute("UPDATE product_variants SET state_hash = 'same-second', updated_at = 1000"));
    CHECK_EQ(rig.schedule(1000), 1);  // регрессия: раньше такое изменение терялось до следующей правки
}
