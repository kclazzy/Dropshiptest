#include <fstream>
#include <set>
#include <sstream>

#include "Harness.hpp"
#include "TestDb.hpp"
#include "bridge/SupplierSessionStore.hpp"
#include "bridge/TokenStore.hpp"

using namespace dropship;
using namespace dropship::bridge;

namespace {

bool accepts(SupplierSessionStore& store, std::string_view domain, std::vector<Cookie> cookies,
             std::string_view ua = "UA") {
    return store.update(domain, cookies, ua, 100).has_value();
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool isHex64(const std::string& s) {
    return s.size() == kTokenHexLength && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

}  // namespace

TEST(sessions_store_builds_a_cookie_header_and_user_agent) {
    SupplierSessionStore store;
    REQUIRE(accepts(store, "Shop.Example", {{"sid", "abc"}, {"cf", "1=2"}}, "Mozilla/5.0"));
    const auto s = store.lookup("shop.example");
    REQUIRE(s.has_value());
    CHECK_EQ(s->cookieHeader, std::string("sid=abc; cf=1=2"));
    CHECK_EQ(s->userAgent, std::string("Mozilla/5.0"));
    CHECK_EQ(s->updatedAt, std::int64_t{100});
}

TEST(sessions_store_matches_subdomains_with_longest_domain_winning) {
    SupplierSessionStore store;
    REQUIRE(accepts(store, ".example.com", {{"a", "root"}}));
    REQUIRE(accepts(store, "shop.example.com", {{"a", "shop"}}));
    CHECK_EQ(store.lookup("www.example.com")->cookieHeader, std::string("a=root"));
    CHECK_EQ(store.lookup("example.com")->cookieHeader, std::string("a=root"));
    CHECK_EQ(store.lookup("m.shop.example.com")->cookieHeader, std::string("a=shop"));
    CHECK(!store.lookup("badexample.com").has_value());  // суффикс без точки — не поддомен
    CHECK(!store.lookup("other.org").has_value());
}

TEST(sessions_store_replaces_the_session_of_a_domain) {
    SupplierSessionStore store;
    REQUIRE(accepts(store, "a.example", {{"old", "1"}}));
    REQUIRE(accepts(store, "a.example", {{"new", "2"}}));
    CHECK_EQ(store.lookup("a.example")->cookieHeader, std::string("new=2"));
    CHECK_EQ(store.size(), std::size_t{1});
}

TEST(sessions_store_rejects_header_injection_and_bad_input) {
    SupplierSessionStore store;
    CHECK(!accepts(store, "a.example", {{"n", "v\r\nX-Evil: 1"}}));
    CHECK(!accepts(store, "a.example", {{"n", "v;other=1"}}));
    CHECK(!accepts(store, "a.example", {{"n;x", "v"}}));
    CHECK(!accepts(store, "a.example", {{"n=x", "v"}}));
    CHECK(!accepts(store, "a.example", {{"", "v"}}));
    CHECK(!accepts(store, "a.example", {{"n", "v"}}, "UA\r\nX: 1"));
    CHECK(!accepts(store, "", {{"n", "v"}}));
    CHECK(!accepts(store, "bad host", {{"n", "v"}}));
    CHECK(!accepts(store, "a..example", {{"n", "v"}}));
    CHECK(!accepts(store, std::string(300, 'a') + ".example", {{"n", "v"}}));
    CHECK(!accepts(store, "a.example", std::vector<Cookie>(201, Cookie{"n", "v"})));
    CHECK_EQ(store.size(), std::size_t{0});  // ничего частично не записано
}

TEST(token_is_created_once_and_then_reused) {
    testdb::TempDir dir;
    const auto file = dir.file("sub/bridge.token");
    const auto first = loadOrCreateToken(file);
    REQUIRE(first.has_value());
    CHECK(isHex64(*first));
    CHECK_EQ(readAll(file), *first);

    const auto second = loadOrCreateToken(file);
    REQUIRE(second.has_value());
    CHECK_EQ(*second, *first);
}

TEST(token_file_is_private_to_the_owner) {
    testdb::TempDir dir;
    const auto file = dir.file("bridge.token");
    REQUIRE(loadOrCreateToken(file).has_value());
#ifndef _WIN32
    const auto perms = std::filesystem::status(file).permissions();
    using P = std::filesystem::perms;
    CHECK((perms & (P::group_all | P::others_all)) == P::none);
#endif
}

TEST(token_with_damaged_content_is_replaced) {
    testdb::TempDir dir;
    const auto file = dir.file("bridge.token");
    { std::ofstream(file) << "short"; }
    const auto token = loadOrCreateToken(file);
    REQUIRE(token.has_value());
    CHECK(isHex64(*token));
    CHECK_EQ(readAll(file), *token);

    { std::ofstream(file) << *token << "\n"; }  // перевод строки после токена допустим
    CHECK_EQ(*loadOrCreateToken(file), *token);
}

TEST(tokens_are_unique) {
    testdb::TempDir dir;
    std::set<std::string> seen;
    for (int i = 0; i < 20; ++i) {
        seen.insert(*loadOrCreateToken(dir.file("t" + std::to_string(i))));
    }
    CHECK_EQ(seen.size(), std::size_t{20});
}
