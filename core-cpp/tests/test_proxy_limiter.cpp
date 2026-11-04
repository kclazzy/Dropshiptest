#include <set>
#include <thread>

#include "TestDb.hpp"
#include "monitor/DomainLimiter.hpp"
#include "monitor/ProxyPool.hpp"

using namespace testdb;
using namespace dropship::monitor;

namespace {
void addProxy(DatabaseManager& db, const std::string& url) {
    unwrap(db.execute("INSERT INTO proxies(proxy_url) VALUES(?)", {url}));
}
}  // namespace

TEST(proxy_pool_without_proxies_means_direct_connection) {
    TempDir dir;
    auto db = openDb(dir);
    ProxyPool pool(*db);
    CHECK(unwrap(pool.acquire(100)).availability == ProxyAvailability::NoneConfigured);
}

TEST(proxy_pool_rotates_least_recently_used_first) {
    TempDir dir;
    auto db = openDb(dir);
    addProxy(*db, "http://p1:1");
    addProxy(*db, "http://p2:1");
    ProxyPool pool(*db);
    const auto a = unwrap(pool.acquire(100));
    const auto b = unwrap(pool.acquire(101));
    const auto c = unwrap(pool.acquire(102));
    CHECK_EQ(a.lease.url, std::string("http://p1:1"));
    CHECK_EQ(b.lease.url, std::string("http://p2:1"));
    CHECK_EQ(c.lease.url, std::string("http://p1:1"));
}

TEST(proxy_pool_failure_starts_growing_cooldown_and_success_resets_it) {
    TempDir dir;
    auto db = openDb(dir);
    addProxy(*db, "http://p1:1");
    ProxyPool pool(*db, {5, std::chrono::seconds(60), std::chrono::seconds(3600)});
    const auto id = unwrap(pool.acquire(100)).lease.id;

    unwrap(pool.reportFailure(id, 100));
    CHECK(unwrap(pool.acquire(159)).availability == ProxyAvailability::AllCoolingDown);  // нельзя идти напрямую
    CHECK(unwrap(pool.acquire(160)).availability == ProxyAvailability::Leased);

    unwrap(pool.reportFailure(id, 200));  // вторая подряд: 120 с
    CHECK(unwrap(pool.acquire(319)).availability == ProxyAvailability::AllCoolingDown);
    CHECK(unwrap(pool.acquire(320)).availability == ProxyAvailability::Leased);

    unwrap(pool.reportSuccess(id));
    unwrap(pool.reportFailure(id, 400));  // после успеха счёт идёт с начала: 60 с
    CHECK(unwrap(pool.acquire(460)).availability == ProxyAvailability::Leased);
}

TEST(proxy_pool_deactivates_a_proxy_after_too_many_failures_and_caps_cooldown) {
    TempDir dir;
    auto db = openDb(dir);
    addProxy(*db, "http://p1:1");
    ProxyPool pool(*db, {3, std::chrono::seconds(60), std::chrono::seconds(100)});
    const auto id = unwrap(pool.acquire(0)).lease.id;
    unwrap(pool.reportFailure(id, 1000));
    unwrap(pool.reportFailure(id, 1000));
    CHECK_EQ(scalarInt(*db, "SELECT cooldown_until FROM proxies"), 1100);  // 60*2=120 -> потолок 100
    unwrap(pool.reportFailure(id, 1000));
    CHECK_EQ(scalarInt(*db, "SELECT is_active FROM proxies"), 0);
    CHECK(unwrap(pool.acquire(999999)).availability == ProxyAvailability::NoneConfigured);
}

TEST(proxy_pool_never_hands_one_proxy_to_two_workers_at_once) {
    TempDir dir;
    auto db = openDb(dir);
    for (int i = 0; i < 4; ++i) addProxy(*db, "http://p" + std::to_string(i) + ":1");
    ProxyPool pool(*db);
    std::vector<std::thread> threads;
    std::mutex mu;
    std::multiset<std::int64_t> seen;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            const auto lease = pool.acquire(100);
            if (lease && lease->availability == ProxyAvailability::Leased) {
                std::lock_guard lock(mu);
                seen.insert(lease->lease.id);
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(seen.size(), std::size_t{4});
    CHECK_EQ(std::set<std::int64_t>(seen.begin(), seen.end()).size(), std::size_t{4});
}

TEST(domain_limiter_spaces_requests_per_host) {
    DomainLimiter limiter(2);
    CHECK(limiter.tryAcquire("a.example", 100));
    CHECK(!limiter.tryAcquire("a.example", 101));
    CHECK(limiter.tryAcquire("b.example", 101));  // другой домен не мешает
    CHECK(limiter.tryAcquire("a.example", 102));
    CHECK_EQ(limiter.availableAt("a.example"), UnixSeconds{104});
    CHECK_EQ(limiter.availableAt("never.example"), UnixSeconds{0});
}

TEST(domain_limiter_block_only_extends) {
    DomainLimiter limiter(2);
    limiter.blockUntil("a.example", 500);
    CHECK(!limiter.tryAcquire("a.example", 499));
    limiter.blockUntil("a.example", 300);  // более короткая блокировка не сокращает длинную
    CHECK(!limiter.tryAcquire("a.example", 499));
    CHECK(limiter.tryAcquire("a.example", 500));
}

TEST(domain_limiter_unblock_frees_the_host_immediately) {
    DomainLimiter limiter(2);
    limiter.blockUntil("a.example", 5000);
    limiter.unblock("a.example");
    CHECK(limiter.tryAcquire("a.example", 1));
}
