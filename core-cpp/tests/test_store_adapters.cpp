#include "FakeTransport.hpp"
#include "Harness.hpp"
#include "store/AvitoAdapter.hpp"
#include "store/OzonAdapter.hpp"

using namespace dropship;
using namespace dropship::store;

namespace {

std::string header(const net::HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers) if (k == name) return v;
    return {};
}

struct OzonRig {
    testfakes::FakeTransport transport;
    TokenBucketRateLimiter limiter;
    double now = 1000;
    explicit OzonRig(double rate = 100.0, double burst = 100.0) : limiter(rate, burst) {}
    OzonAdapter adapter{OzonConfig{"cid", "key", 77, "https://ozon.test"}, transport, limiter, [this] { return now; }};
};

struct AvitoRig {
    testfakes::FakeTransport transport;
    TokenBucketRateLimiter limiter{100.0, 100.0};
    double now = 1000;
    AvitoAdapter adapter{AvitoConfig{"id with space", "sec&ret", "https://avito.test"}, transport, limiter, [this] { return now; }};
    void tokenReply(int lifetime = 3600) { transport.reply(200, R"({"access_token":"T1","expires_in":)" + std::to_string(lifetime) + "}"); }
};

}  // namespace

TEST(ozon_sends_stock_and_price_with_credentials_and_rounds_price_up) {
    OzonRig rig;
    rig.transport.reply(200, R"({"result":[{"offer_id":"A","updated":true,"errors":[]}]})");
    rig.transport.reply(200, R"({"result":[{"offer_id":"A","updated":true,"errors":[]}]})");
    const VariantUpdate update{5, "A", 15601, 7};  // 156.01 -> 157 ₽
    const auto r = rig.adapter.updateVariantPriceAndStock(std::span(&update, 1));
    REQUIRE(r.has_value());
    CHECK((*r)[0].ok && (*r)[0].stockSent && (*r)[0].priceSent);
    REQUIRE(rig.transport.requests.size() == 2);
    const auto& stock = rig.transport.requests[0];
    CHECK_EQ(stock.url, std::string("https://ozon.test/v2/products/stocks"));
    CHECK_EQ(header(stock, "Client-Id"), std::string("cid"));
    CHECK_EQ(header(stock, "Api-Key"), std::string("key"));
    const Json stockBody = parseJson(stock.body);
    CHECK_EQ(stockBody["stocks"][0]["offer_id"].get<std::string>(), std::string("A"));
    CHECK_EQ(stockBody["stocks"][0]["stock"].get<int>(), 7);
    CHECK_EQ(stockBody["stocks"][0]["warehouse_id"].get<int>(), 77);
    const Json priceBody = parseJson(rig.transport.requests[1].body);
    CHECK_EQ(rig.transport.requests[1].url, std::string("https://ozon.test/v1/product/import/prices"));
    CHECK_EQ(priceBody["prices"][0]["price"].get<std::string>(), std::string("157"));
}

TEST(ozon_never_sends_an_unpriced_variant_price_but_still_sends_stock) {
    OzonRig rig;
    rig.transport.reply(200, R"({"result":[{"offer_id":"A","updated":true}]})");
    const VariantUpdate update{1, "A", 0, 3};
    const auto r = rig.adapter.updateVariantPriceAndStock(std::span(&update, 1));
    REQUIRE(r.has_value());
    CHECK((*r)[0].ok);
    CHECK(!(*r)[0].priceSent);
    CHECK_EQ(rig.transport.requests.size(), std::size_t{1});
}

TEST(ozon_reports_unlinked_duplicate_and_rejected_items_individually) {
    OzonRig rig;
    rig.transport.reply(200, R"({"result":[{"offer_id":"OK","updated":true},{"offer_id":"BAD","updated":false,"errors":[{"code":"PRODUCT_NOT_FOUND","message":"no such offer"}]}]})");
    rig.transport.reply(200, R"({"result":[{"offer_id":"OK","updated":true}]})");
    const std::vector<VariantUpdate> updates = {{1, "OK", 10000, 1}, {2, "", 10000, 1}, {3, "BAD", 10000, 1}, {4, "OK", 10000, 1}};
    const auto r = rig.adapter.updateVariantPriceAndStock(updates);
    REQUIRE(r.has_value());
    CHECK((*r)[0].ok);
    CHECK(!(*r)[1].ok && (*r)[1].error.find("not linked") != std::string::npos);
    CHECK(!(*r)[2].ok && (*r)[2].error.find("PRODUCT_NOT_FOUND") != std::string::npos);
    CHECK(!(*r)[3].ok && (*r)[3].error.find("duplicate") != std::string::npos);
}

TEST(ozon_batches_stock_updates_by_100) {
    OzonRig rig;
    std::vector<VariantUpdate> updates;
    for (int i = 0; i < 250; ++i) updates.push_back({i, "O" + std::to_string(i), 0, 1});
    for (int i = 0; i < 3; ++i) rig.transport.reply(200, R"({"result":[]})");
    const auto r = rig.adapter.updateVariantPriceAndStock(updates);
    REQUIRE(r.has_value());
    CHECK_EQ(rig.transport.requests.size(), std::size_t{3});
    CHECK_EQ(parseJson(rig.transport.requests[0].body)["stocks"].size(), std::size_t{100});
    CHECK_EQ(parseJson(rig.transport.requests[2].body)["stocks"].size(), std::size_t{50});
    CHECK(!(*r)[0].ok);  // Ozon не подтвердил — это не успех
}

TEST(ozon_classifies_http_failures) {
    const VariantUpdate update{1, "A", 100, 1};
    const auto run = [&](int status) {
        OzonRig rig;
        rig.transport.reply(status, R"({"message":"nope"})");
        return rig.adapter.updateVariantPriceAndStock(std::span(&update, 1));
    };
    CHECK(run(401).error().kind == StoreError::Kind::Auth);
    CHECK(run(403).error().kind == StoreError::Kind::Auth);
    CHECK(run(429).error().kind == StoreError::Kind::RateLimited);
    CHECK(run(400).error().kind == StoreError::Kind::Rejected);
    CHECK(run(503).error().kind == StoreError::Kind::Transport);
    OzonRig rig;
    rig.transport.fail("timeout");
    CHECK(rig.adapter.updateVariantPriceAndStock(std::span(&update, 1)).error().kind == StoreError::Kind::Transport);
    OzonRig garbage;
    garbage.transport.reply(200, "<html>");
    CHECK(garbage.adapter.updateVariantPriceAndStock(std::span(&update, 1)).error().retryable());
}

TEST(ozon_respects_the_local_rate_limit_without_calling_the_api) {
    OzonRig rig(1.0 / 60.0, 1.0);  // один запрос в минуту
    rig.transport.reply(200, R"({"result":[]})");
    const VariantUpdate update{1, "A", 0, 1};
    REQUIRE(rig.adapter.updateVariantPriceAndStock(std::span(&update, 1)).has_value());
    const auto r = rig.adapter.updateVariantPriceAndStock(std::span(&update, 1));
    REQUIRE(!r.has_value());
    CHECK(r.error().kind == StoreError::Kind::RateLimited);
    CHECK(r.error().retryAfterSeconds >= 59);
    CHECK_EQ(rig.transport.requests.size(), std::size_t{1});
}

TEST(ozon_requires_credentials_and_warehouse) {
    testfakes::FakeTransport transport;
    TokenBucketRateLimiter limiter(10, 10);
    const VariantUpdate update{1, "A", 100, 1};
    OzonAdapter noWarehouse(OzonConfig{"c", "k", 0, "https://x"}, transport, limiter, [] { return 0.0; });
    CHECK(noWarehouse.updateVariantPriceAndStock(std::span(&update, 1)).error().kind == StoreError::Kind::Config);
    OzonAdapter noKey(OzonConfig{"", "", 1, "https://x"}, transport, limiter, [] { return 0.0; });
    CHECK(noKey.updateVariantPriceAndStock(std::span(&update, 1)).error().kind == StoreError::Kind::Config);
    CHECK(transport.requests.empty());
}

TEST(ozon_unsupported_operations_say_so) {
    OzonRig rig;
    CHECK(rig.adapter.publishProduct({}).error().kind == StoreError::Kind::Unsupported);
    CHECK(rig.adapter.fetchNewOrders(0).error().kind == StoreError::Kind::Unsupported);
    CHECK(rig.adapter.submitTrackingNumber("1", "2").error().kind == StoreError::Kind::Unsupported);
}

TEST(avito_fetches_a_token_once_and_updates_stock_and_price) {
    AvitoRig rig;
    rig.tokenReply();
    rig.transport.reply(200, R"({"stocks":[{"item_id":123,"success":true,"errors":[]}]})");
    rig.transport.reply(200, R"({"result":{"success":true}})");
    const VariantUpdate update{9, "123", 9901, 4};  // 99.01 -> 100 ₽
    const auto r = rig.adapter.updateVariantPriceAndStock(std::span(&update, 1));
    REQUIRE(r.has_value());
    CHECK((*r)[0].ok && (*r)[0].priceSent && (*r)[0].stockSent);
    REQUIRE(rig.transport.requests.size() == 3);
    const auto& tokenReq = rig.transport.requests[0];
    CHECK_EQ(tokenReq.url, std::string("https://avito.test/token"));
    CHECK_EQ(tokenReq.contentType, std::string("application/x-www-form-urlencoded"));
    CHECK_EQ(tokenReq.body, std::string("grant_type=client_credentials&client_id=id%20with%20space&client_secret=sec%26ret"));
    const auto& stockReq = rig.transport.requests[1];
    CHECK(stockReq.method == net::HttpRequest::Method::Put);
    CHECK_EQ(stockReq.url, std::string("https://avito.test/stock-management/1/stocks"));
    CHECK_EQ(header(stockReq, "Authorization"), std::string("Bearer T1"));
    CHECK_EQ(parseJson(stockReq.body)["stocks"][0]["quantity"].get<int>(), 4);
    CHECK_EQ(rig.transport.requests[2].url, std::string("https://avito.test/core/v1/items/123/update_price"));
    CHECK_EQ(parseJson(rig.transport.requests[2].body)["price"].get<int>(), 100);

    // Второй вызов использует тот же токен.
    rig.transport.reply(200, R"({"stocks":[{"item_id":123,"success":true}]})");
    const VariantUpdate noPrice{9, "123", 0, 4};
    REQUIRE(rig.adapter.updateVariantPriceAndStock(std::span(&noPrice, 1)).has_value());
    CHECK_EQ(rig.transport.requests.size(), std::size_t{4});
}

TEST(avito_refreshes_an_expired_token_and_retries_once_after_401) {
    AvitoRig rig;
    rig.tokenReply(100);  // живёт 100 с, обновляется за минуту до конца
    rig.transport.reply(200, R"({"stocks":[{"item_id":1,"success":true}]})");
    const VariantUpdate update{1, "1", 0, 1};
    REQUIRE(rig.adapter.updateVariantPriceAndStock(std::span(&update, 1)).has_value());
    rig.now += 50;  // токен ещё считается просроченным (запас 60 с)
    rig.tokenReply();
    rig.transport.reply(401, "");           // токен отозван
    rig.tokenReply();
    rig.transport.reply(200, R"({"stocks":[{"item_id":1,"success":true}]})");
    const auto r = rig.adapter.updateVariantPriceAndStock(std::span(&update, 1));
    REQUIRE(r.has_value());
    CHECK((*r)[0].ok);
}

TEST(avito_reports_bad_ids_item_errors_and_auth_failures) {
    AvitoRig rig;
    rig.tokenReply();
    rig.transport.reply(200, R"({"stocks":[{"item_id":5,"success":false,"errors":["item not found"]}]})");
    const std::vector<VariantUpdate> updates = {{1, "abc", 100, 1}, {2, "5", 100, 1}, {3, "-4", 100, 1}};
    const auto r = rig.adapter.updateVariantPriceAndStock(updates);
    REQUIRE(r.has_value());
    CHECK((*r)[0].error.find("numeric") != std::string::npos);
    CHECK((*r)[1].error.find("item not found") != std::string::npos);
    CHECK(!(*r)[2].ok);
    CHECK_EQ(rig.transport.requests.size(), std::size_t{2});  // цена для объявления с ошибкой остатка не отправляется

    AvitoRig badAuth;
    badAuth.transport.reply(401, "");
    const VariantUpdate update{1, "1", 1, 1};
    CHECK(badAuth.adapter.updateVariantPriceAndStock(std::span(&update, 1)).error().kind == StoreError::Kind::Auth);

    AvitoRig twice;
    twice.tokenReply();
    twice.transport.reply(401, "");
    twice.tokenReply();
    twice.transport.reply(401, "");
    CHECK(twice.adapter.updateVariantPriceAndStock(std::span(&update, 1)).error().kind == StoreError::Kind::Auth);
}

TEST(avito_rejected_price_is_an_item_error_not_a_batch_failure) {
    AvitoRig rig;
    rig.tokenReply();
    rig.transport.reply(200, R"({"stocks":[{"item_id":1,"success":true},{"item_id":2,"success":true}]})");
    rig.transport.reply(400, R"({"error":"bad price"})");
    rig.transport.reply(200, "{}");
    const std::vector<VariantUpdate> updates = {{1, "1", 500, 1}, {2, "2", 500, 1}};
    const auto r = rig.adapter.updateVariantPriceAndStock(updates);
    REQUIRE(r.has_value());
    CHECK(!(*r)[0].ok);
    CHECK((*r)[1].ok);
}
