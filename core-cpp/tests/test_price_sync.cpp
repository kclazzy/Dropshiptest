#include <atomic>

#include "FakeTransport.hpp"
#include "TestDb.hpp"
#include "monitor/PriceSync.hpp"

using namespace testdb;
using namespace dropship::monitor;

namespace {

const char* kPage =
    R"(<html><head><script type="application/ld+json">{"@type":"Product","offers":{"sku":"SKU-1","price":"120.00"}}</script></head></html>)";

struct Rig {
    TempDir dir;
    std::unique_ptr<DatabaseManager> db = openDb(dir);
    testfakes::FakeTransport transport;
    dropship::bridge::SupplierSessionStore sessions;
    ProxyPool proxies{*db};
    DomainLimiter limiter{0};  // в тестах интервал выключен, если не нужен
    Fixture f = insertFixture(*db);
    MonitorContext context{*db, transport, sessions, proxies, limiter, {}};

    Rig() {
        unwrap(db->execute("UPDATE products SET supplier_url = 'https://supplier.example/p/1' WHERE id = ?", {f.productId}));
    }
    void queue(int attempts = 3) {
        unwrap(db->enqueueTask(TaskType::PriceSync, 5, "{\"product_id\":" + std::to_string(f.productId) + "}", 0, attempts));
    }
    SyncOutcome run(UnixSeconds now = 1000) { return unwrap(processNextPriceSync(context, now)); }
    std::string monitorStatus() {
        return unwrap(db->query("SELECT monitor_status FROM products WHERE id = ?", {f.productId})).textAt(0, "monitor_status");
    }
    std::string taskStatus() { return unwrap(db->query("SELECT status FROM task_queue")).textAt(0, "status"); }
    std::int64_t price() {
        return scalarInt(*db, "SELECT supplier_price_cents FROM product_variants WHERE id = " + std::to_string(f.variantId));
    }
};

}  // namespace

TEST(sync_no_task_when_queue_is_empty) {
    Rig rig;
    CHECK(rig.run() == SyncOutcome::NoTask);
    CHECK(rig.transport.requests.empty());
}

TEST(sync_applies_a_new_price_and_completes_the_task) {
    Rig rig;
    rig.queue();
    rig.transport.reply(200, kPage);
    CHECK(rig.run() == SyncOutcome::Updated);
    CHECK_EQ(rig.price(), std::int64_t{12000});
    CHECK_EQ(rig.taskStatus(), std::string("COMPLETED"));
    CHECK_EQ(rig.transport.requests[0].url, std::string("https://supplier.example/p/1"));
}

TEST(sync_reports_unchanged_state) {
    Rig rig;
    rig.queue();
    rig.transport.reply(200, kPage);
    rig.run();
    rig.queue();
    rig.transport.reply(200, kPage);
    CHECK(rig.run(2000) == SyncOutcome::Unchanged);
}

TEST(sync_sends_the_browser_session_cookies_and_user_agent) {
    Rig rig;
    REQUIRE(rig.sessions.update("supplier.example", {{"sid", "abc"}, {"cf", "xyz"}}, "UA-from-browser", 1).has_value());
    rig.queue();
    rig.transport.reply(200, kPage);
    rig.run();
    const auto& req = rig.transport.requests[0];
    const auto header = [&](const std::string& name) {
        for (const auto& [k, v] : req.headers) if (k == name) return v;
        return std::string();
    };
    CHECK_EQ(header("Cookie"), std::string("sid=abc; cf=xyz"));
    CHECK_EQ(header("User-Agent"), std::string("UA-from-browser"));
}

TEST(sync_follows_same_host_redirects_with_cookies_but_drops_them_for_other_hosts) {
    Rig rig;
    REQUIRE(rig.sessions.update("supplier.example", {{"sid", "abc"}}, "UA", 1).has_value());
    rig.queue();
    rig.transport.reply(301, "", "https://supplier.example/moved");
    rig.transport.reply(302, "", "https://evil.example/steal");
    rig.transport.reply(200, kPage);
    CHECK(rig.run() == SyncOutcome::Updated);
    REQUIRE(rig.transport.requests.size() == 3);
    const auto hasCookie = [](const dropship::net::HttpRequest& r) {
        for (const auto& [k, v] : r.headers) if (k == "Cookie") return true;
        return false;
    };
    CHECK(hasCookie(rig.transport.requests[1]));
    CHECK(!hasCookie(rig.transport.requests[2]));
}

TEST(sync_gives_up_on_redirect_loops_and_non_http_targets) {
    Rig rig;
    rig.queue(1);
    for (int i = 0; i < 6; ++i) rig.transport.reply(302, "", "https://supplier.example/loop");
    CHECK(rig.run() == SyncOutcome::Failed);
    Rig other;
    other.queue(1);
    other.transport.reply(302, "", "file:///etc/passwd");
    CHECK(other.run() == SyncOutcome::Failed);
    CHECK_EQ(other.transport.requests.size(), std::size_t{1});
}

TEST(sync_403_without_proxy_blocks_the_card_and_the_domain) {
    Rig rig;
    rig.queue();
    rig.transport.reply(403, "captcha");
    CHECK(rig.run(1000) == SyncOutcome::Blocked);
    CHECK_EQ(rig.monitorStatus(), std::string("CAPTCHA_BLOCKED"));
    CHECK_EQ(rig.taskStatus(), std::string("COMPLETED"));
    CHECK(rig.limiter.availableAt("supplier.example") >= 1000 + 1800);
}

TEST(sync_429_with_a_proxy_penalises_it_and_retries_later_without_spending_an_attempt) {
    Rig rig;
    unwrap(rig.db->execute("INSERT INTO proxies(proxy_url) VALUES('http://p1:8080')"));
    rig.queue(1);
    rig.transport.reply(429, "slow down");
    CHECK(rig.run(1000) == SyncOutcome::Deferred);
    CHECK_EQ(rig.transport.requests[0].proxy, std::string("http://p1:8080"));
    CHECK_EQ(scalarInt(*rig.db, "SELECT fail_count FROM proxies"), 1);
    CHECK_EQ(rig.monitorStatus(), std::string("ACTIVE"));
    CHECK_EQ(rig.taskStatus(), std::string("PENDING"));
    CHECK_EQ(scalarInt(*rig.db, "SELECT attempts FROM task_queue"), 0);
}

TEST(sync_waits_when_all_proxies_are_cooling_down_and_never_goes_direct) {
    Rig rig;
    unwrap(rig.db->execute("INSERT INTO proxies(proxy_url, cooldown_until) VALUES('http://p1:8080', 99999)"));
    rig.queue();
    CHECK(rig.run(1000) == SyncOutcome::Deferred);
    CHECK(rig.transport.requests.empty());
}

TEST(sync_defers_when_the_domain_is_rate_limited) {
    Rig rig;
    rig.limiter.blockUntil("supplier.example", 5000);
    rig.queue();
    CHECK(rig.run(1000) == SyncOutcome::Deferred);
    CHECK(rig.transport.requests.empty());
    CHECK_EQ(scalarInt(*rig.db, "SELECT scheduled_at FROM task_queue"), 5000);
}

TEST(sync_404_marks_the_product_gone) {
    Rig rig;
    rig.queue();
    rig.transport.reply(404, "");
    CHECK(rig.run() == SyncOutcome::Gone);
    CHECK_EQ(rig.monitorStatus(), std::string("PAUSED"));
    CHECK_EQ(scalarInt(*rig.db, "SELECT supplier_stock FROM product_variants"), 0);
}

TEST(sync_network_errors_retry_then_fail_the_card) {
    Rig rig;
    rig.queue(2);
    rig.transport.fail("timeout");
    rig.transport.fail("timeout");
    CHECK(rig.run(1000) == SyncOutcome::Retrying);
    CHECK_EQ(rig.monitorStatus(), std::string("ACTIVE"));
    CHECK(rig.run(1000 + 299) == SyncOutcome::NoTask);
    CHECK(rig.run(1000 + 300) == SyncOutcome::Failed);
    CHECK_EQ(rig.monitorStatus(), std::string("ERROR"));
}

TEST(sync_unparseable_pages_and_server_errors_are_temporary) {
    Rig rig;
    rig.queue();
    rig.transport.reply(200, "<html>no price here</html>");
    CHECK(rig.run() == SyncOutcome::Retrying);
    Rig server;
    server.queue();
    server.transport.reply(503, "");
    CHECK(server.run() == SyncOutcome::Retrying);
}

TEST(sync_skips_paused_products_without_any_request) {
    Rig rig;
    unwrap(rig.db->execute("UPDATE products SET monitor_status = 'PAUSED'"));
    rig.queue();
    CHECK(rig.run() == SyncOutcome::Unchanged);
    CHECK(rig.transport.requests.empty());
    CHECK_EQ(rig.taskStatus(), std::string("COMPLETED"));
}

TEST(sync_fails_tasks_with_bad_payload_or_missing_product) {
    Rig rig;
    unwrap(rig.db->enqueueTask(TaskType::PriceSync, 5, "garbage", 0));
    unwrap(rig.db->enqueueTask(TaskType::PriceSync, 5, "{\"product_id\":9999}", 0));
    CHECK(rig.run() == SyncOutcome::Failed);
    CHECK(rig.run() == SyncOutcome::Failed);
    CHECK(rig.transport.requests.empty());
}

TEST(monitor_service_runs_scheduled_checks_in_background) {
    Rig rig;
    rig.transport.reply(200, kPage);
    std::atomic<int> logged{0};
    {
        MonitorService service(rig.context, [] { return UnixSeconds{1000}; }, 2, [&](const std::string&) { ++logged; });
        for (int i = 0; i < 200 && rig.price() != 12000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK_EQ(rig.price(), std::int64_t{12000});
}
