#include <chrono>
#include <thread>

#include "Harness.hpp"
#include "TestDb.hpp"
#include "WsTestClient.hpp"
#include "bridge/BridgeServer.hpp"
#include "bridge/Handlers.hpp"

using namespace dropship;
using namespace dropship::bridge;
using namespace std::chrono_literals;
using testnet::WsTestClient;

namespace {

const std::string kToken(64, 'a');

struct Fixture {
    testdb::TempDir dir;
    std::unique_ptr<db::DatabaseManager> database;
    SupplierSessionStore sessions;
    std::shared_ptr<RpcRouter> router = std::make_shared<RpcRouter>();
    std::unique_ptr<BridgeServer> server;
    std::vector<std::string> updatedDomains;

    explicit Fixture(BridgeConfig config = {}) {
        database = testdb::openDb(dir);
        registerCoreHandlers(*router, CoreServices{*database, sessions, [] { return UnixSeconds{1000}; },
                                                   [this](const std::string& domain) { updatedDomains.push_back(domain); }});
        config.port = 0;
        config.authToken = kToken;
        auto started = BridgeServer::start(config, router);
        if (!started) {
            ::testing::fail(__FILE__, __LINE__, "bridge did not start: " + started.error());
            throw ::testing::Abort{};
        }
        server = std::move(*started);
    }
    std::uint16_t port() const { return server->port(); }
};

std::string authMessage(const std::string& token = kToken, std::int64_t id = 1) {
    return makeRequest(RpcId{id}, "AUTH", Json{{"token", token}});
}

Json parseReply(const std::optional<std::string>& text) {
    if (!text) return Json();
    return Json::parse(*text, nullptr, false);
}

// Подключается и проходит авторизацию.
std::unique_ptr<WsTestClient> connectAuthed(Fixture& f, std::optional<std::string> origin = std::nullopt) {
    auto client = std::make_unique<WsTestClient>(f.port(), origin);
    if (!client->connected() || !client->send(authMessage())) {
        ::testing::fail(__FILE__, __LINE__, "cannot connect: " + client->handshakeError());
        throw ::testing::Abort{};
    }
    const Json reply = parseReply(client->receive());
    if (!reply.contains("result")) {
        ::testing::fail(__FILE__, __LINE__, "auth was not accepted");
        throw ::testing::Abort{};
    }
    return client;
}

Json rpc(WsTestClient& client, const std::string& method, const Json& params, std::int64_t id = 2) {
    client.send(makeRequest(RpcId{id}, method, params));
    return parseReply(client.receive());
}

bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

Json validImport() {
    return Json::parse(R"({"url":"https://shop.example/p/1","title":"Наушники","description":"d",
        "variants":[{"sku":"A","price_cents":1000,"stock":7}]})");
}

}  // namespace

// ---- запуск ----

TEST(bridge_refuses_non_loopback_addresses) {
    testdb::TempDir dir;
    auto router = std::make_shared<RpcRouter>();
    for (const char* address : {"0.0.0.0", "192.168.1.5", "::", "example.com", ""}) {
        BridgeConfig config;
        config.bindAddress = address;
        config.port = 0;
        config.authToken = kToken;
        CHECK(!BridgeServer::start(config, router).has_value());
    }
    BridgeConfig ok;
    ok.port = 0;
    ok.authToken = kToken;
    CHECK(BridgeServer::start(ok, router).has_value());
}

TEST(bridge_requires_a_strong_token) {
    auto router = std::make_shared<RpcRouter>();
    BridgeConfig config;
    config.port = 0;
    config.authToken = "short";
    CHECK(!BridgeServer::start(config, router).has_value());
    config.authToken = "";
    CHECK(!BridgeServer::start(config, router).has_value());
}

TEST(bridge_reports_a_busy_port) {
    Fixture first;
    BridgeConfig config;
    config.port = first.port();
    config.authToken = kToken;
    const auto second = BridgeServer::start(config, first.router);
    REQUIRE(!second.has_value());
    CHECK(!second.error().empty());
}

// ---- авторизация и рукопожатие ----

TEST(bridge_accepts_a_valid_token_and_answers_ping) {
    Fixture f;
    CHECK(!f.server->extensionConnected());
    WsTestClient client(f.port());
    REQUIRE(client.connected());
    REQUIRE(client.send(authMessage()));
    const Json auth = parseReply(client.receive());
    CHECK(auth["result"]["ok"].get<bool>());
    CHECK_EQ(auth["result"]["protocol"].get<int>(), kProtocolVersion);
    CHECK_EQ(auth["id"].get<int>(), 1);

    const Json pong = rpc(client, "PING", nullptr);
    CHECK(pong["result"]["pong"].get<bool>());
    CHECK_EQ(pong["result"]["time"].get<std::int64_t>(), std::int64_t{1000});
    CHECK(eventually([&] { return f.server->extensionConnected(); }));
}

TEST(bridge_closes_the_connection_on_a_wrong_token) {
    Fixture f;
    WsTestClient client(f.port());
    REQUIRE(client.connected());
    client.send(authMessage(std::string(64, 'b')));
    const auto reply = client.receive();
    CHECK(!reply.has_value() || !parseReply(reply).contains("result"));
    CHECK(client.waitClosed());
    CHECK(!f.server->extensionConnected());
}

TEST(bridge_requires_auth_as_the_first_message) {
    Fixture f;
    WsTestClient client(f.port());
    REQUIRE(client.connected());
    client.send(makeRequest(RpcId{std::int64_t{1}}, "PING", nullptr));
    const auto reply = client.receive();
    CHECK(!reply.has_value() || !parseReply(reply).contains("result"));
    CHECK(client.waitClosed());
}

TEST(bridge_closes_connections_that_never_authenticate) {
    BridgeConfig config;
    config.authTimeout = 200ms;
    Fixture f(config);
    WsTestClient client(f.port());
    REQUIRE(client.connected());
    CHECK(client.waitClosed(2s));
}

TEST(bridge_rejects_pages_with_a_web_origin) {
    Fixture f;
    WsTestClient evil(f.port(), std::string("https://evil.example"));
    CHECK(!evil.connected());
    CHECK_EQ(evil.httpStatus(), 403);
    WsTestClient local(f.port(), std::string("http://localhost:3000"));
    CHECK(!local.connected());

    WsTestClient extension(f.port(), std::string("chrome-extension://abcdefghijklmnop"));
    CHECK(extension.connected());
    WsTestClient firefox(f.port(), std::string("moz-extension://1234-5678"));
    CHECK(firefox.connected());
    WsTestClient noOrigin(f.port());
    CHECK(noOrigin.connected());  // не браузер (скрипты, тесты) — защищает токен
}

// ---- протокол ----

TEST(bridge_replies_to_garbage_and_unknown_methods_but_not_notifications) {
    Fixture f;
    auto client = connectAuthed(f);

    client->send("this is not json");
    const Json parseError = parseReply(client->receive());
    CHECK_EQ(parseError["error"]["code"].get<int>(), rpc_code::kParseError);
    CHECK(parseError["id"].is_null());

    const Json unknown = rpc(*client, "NO_SUCH_METHOD", nullptr, 5);
    CHECK_EQ(unknown["error"]["code"].get<int>(), rpc_code::kMethodNotFound);
    CHECK_EQ(unknown["id"].get<int>(), 5);

    // Notification не получает ответа: следующим придёт ответ на PING.
    client->send(R"({"jsonrpc":"2.0","method":"NO_SUCH_METHOD"})");
    const Json pong = rpc(*client, "PING", nullptr, 6);
    CHECK_EQ(pong["id"].get<int>(), 6);
    CHECK(pong["result"]["pong"].get<bool>());
}

TEST(bridge_closes_on_binary_frames) {
    Fixture f;
    auto client = connectAuthed(f);
    client->sendBinary("binary");
    CHECK(client->waitClosed());
}

TEST(bridge_closes_on_oversized_messages) {
    BridgeConfig config;
    config.maxMessageBytes = 1024;
    Fixture f(config);
    auto client = connectAuthed(f);
    client->send(std::string(5000, 'x'));
    CHECK(client->waitClosed());
    CHECK(eventually([&] { return !f.server->extensionConnected(); }));
}

// ---- ядро -> расширение ----

TEST(bridge_call_round_trips_through_the_extension) {
    Fixture f;
    auto client = connectAuthed(f);
    REQUIRE(eventually([&] { return f.server->extensionConnected(); }));

    auto pending = f.server->call("EXECUTE_AUTO_ORDER", Json{{"qty", 2}}, 3s);
    const Json request = parseReply(client->receive());
    CHECK_EQ(request["method"].get<std::string>(), std::string("EXECUTE_AUTO_ORDER"));
    CHECK_EQ(request["params"]["qty"].get<int>(), 2);
    client->send(makeResult(RpcId{request["id"].get<std::int64_t>()}, Json{{"supplier_order_id", "S-1"}}));

    REQUIRE(pending.wait_for(3s) == std::future_status::ready);
    const auto result = pending.get();
    REQUIRE(result.has_value());
    CHECK_EQ((*result)["supplier_order_id"].get<std::string>(), std::string("S-1"));
}

TEST(bridge_call_returns_errors_sent_by_the_extension) {
    Fixture f;
    auto client = connectAuthed(f);
    auto pending = f.server->call("SCRAPE_DYNAMIC_PAGE", Json{{"url", "https://x.example"}}, 3s);
    const Json request = parseReply(client->receive());
    client->send(makeError(RpcId{request["id"].get<std::int64_t>()}, RpcError{-1, "captcha", nullptr}));
    REQUIRE(pending.wait_for(3s) == std::future_status::ready);
    const auto result = pending.get();
    REQUIRE(!result.has_value());
    CHECK_EQ(result.error().message, std::string("captcha"));
}

TEST(bridge_call_times_out_and_ignores_a_late_answer) {
    Fixture f;
    auto client = connectAuthed(f);
    auto pending = f.server->call("SLOW", nullptr, 200ms);
    const Json request = parseReply(client->receive());
    REQUIRE(pending.wait_for(3s) == std::future_status::ready);
    const auto result = pending.get();
    REQUIRE(!result.has_value());
    CHECK_EQ(result.error().code, rpc_code::kTimeout);

    client->send(makeResult(RpcId{request["id"].get<std::int64_t>()}, Json{{"late", true}}));  // не должно ничего сломать
    CHECK(rpc(*client, "PING", nullptr, 9)["result"]["pong"].get<bool>());
}

TEST(bridge_call_fails_fast_without_an_extension) {
    Fixture f;
    auto pending = f.server->call("EXECUTE_AUTO_ORDER", nullptr, 5s);
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    const auto result = pending.get();
    REQUIRE(!result.has_value());
    CHECK_EQ(result.error().code, rpc_code::kExtensionNotConnected);
}

TEST(bridge_call_fails_when_the_connection_drops) {
    Fixture f;
    auto client = connectAuthed(f);
    REQUIRE(eventually([&] { return f.server->extensionConnected(); }));
    auto pending = f.server->call("EXECUTE_AUTO_ORDER", nullptr, 10s);
    client->receive();  // запрос дошёл
    client.reset();     // расширение «пропало»
    REQUIRE(pending.wait_for(3s) == std::future_status::ready);
    const auto result = pending.get();
    REQUIRE(!result.has_value());
    CHECK_EQ(result.error().code, rpc_code::kExtensionNotConnected);
    CHECK(eventually([&] { return !f.server->extensionConnected(); }));
}

TEST(bridge_new_connection_replaces_the_old_one) {
    Fixture f;
    auto first = connectAuthed(f);
    auto second = connectAuthed(f);
    CHECK(first->waitClosed());
    CHECK(f.server->extensionConnected());

    auto pending = f.server->call("PING_EXT", nullptr, 3s);
    const Json request = parseReply(second->receive());
    CHECK_EQ(request["method"].get<std::string>(), std::string("PING_EXT"));
    second->send(makeResult(RpcId{request["id"].get<std::int64_t>()}, Json{{"ok", true}}));
    REQUIRE(pending.wait_for(3s) == std::future_status::ready);
    CHECK(pending.get().has_value());
}

TEST(bridge_stops_quickly_even_with_a_connected_client_and_a_pending_call) {
    auto fixture = std::make_unique<Fixture>();
    auto client = connectAuthed(*fixture);
    auto pending = fixture->server->call("HANG", nullptr, 60s);
    client->receive();

    const auto started = std::chrono::steady_clock::now();
    fixture.reset();
    CHECK(std::chrono::steady_clock::now() - started < 2s);

    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    const auto result = pending.get();
    REQUIRE(!result.has_value());
    CHECK_EQ(result.error().code, rpc_code::kExtensionNotConnected);
}

// ---- обработчики ядра ----

TEST(bridge_imports_a_product_end_to_end) {
    Fixture f;
    f.database->execute("INSERT INTO stores(name, platform_type, api_url, encrypted_credentials, created_at) "
                        "VALUES('Ozon','ozon','https://api',x'00',1)");
    auto client = connectAuthed(f);

    const Json first = rpc(*client, "IMPORT_PRODUCT_PAYLOAD", validImport());
    REQUIRE(first.contains("result"));
    CHECK(!first["result"]["duplicate"].get<bool>());
    CHECK_EQ(first["result"]["variants"].get<int>(), 1);
    const auto productId = first["result"]["product_id"].get<std::int64_t>();
    CHECK_EQ(testdb::scalarInt(*f.database, "SELECT COUNT(*) FROM products WHERE id = ?", {productId}), 1);

    const Json second = rpc(*client, "IMPORT_PRODUCT_PAYLOAD", validImport(), 3);
    CHECK(second["result"]["duplicate"].get<bool>());
    CHECK_EQ(second["result"]["product_id"].get<std::int64_t>(), productId);
}

TEST(bridge_import_reports_invalid_params_and_missing_store) {
    Fixture f;
    auto client = connectAuthed(f);
    const Json noStore = rpc(*client, "IMPORT_PRODUCT_PAYLOAD", validImport());
    CHECK_EQ(noStore["error"]["code"].get<int>(), rpc_code::kNoStore);

    Json bad = validImport();
    bad["variants"][0]["price_cents"] = -5;
    const Json invalid = rpc(*client, "IMPORT_PRODUCT_PAYLOAD", bad, 3);
    CHECK_EQ(invalid["error"]["code"].get<int>(), rpc_code::kInvalidParams);
    CHECK(invalid["error"]["message"].get<std::string>().find("price_cents") != std::string::npos);
}

TEST(bridge_stores_supplier_cookies) {
    Fixture f;
    auto client = connectAuthed(f);
    const Json ok = rpc(*client, "SYNC_SUPPLIER_COOKIES",
                        Json{{"domain", "shop.example"},
                             {"cookies", Json::array({Json{{"name", "sid"}, {"value", "abc"}}})},
                             {"user_agent", "Mozilla/5.0"}});
    CHECK_EQ(ok["result"]["stored"].get<int>(), 1);
    const auto session = f.sessions.lookup("www.shop.example");
    REQUIRE(session.has_value());
    CHECK_EQ(session->cookieHeader, std::string("sid=abc"));
    CHECK_EQ(session->userAgent, std::string("Mozilla/5.0"));
    CHECK_EQ(session->updatedAt, std::int64_t{1000});

    const Json bad = rpc(*client, "SYNC_SUPPLIER_COOKIES",
                         Json{{"domain", "shop.example"},
                              {"cookies", Json::array({Json{{"name", "a"}, {"value", "x;y"}}})}},
                         3);
    CHECK_EQ(bad["error"]["code"].get<int>(), rpc_code::kInvalidParams);
    CHECK(f.updatedDomains == std::vector<std::string>{"shop.example"});  // отклонённая сессия хук не вызывает

    const Json malformed = rpc(*client, "SYNC_SUPPLIER_COOKIES", Json{{"domain", 5}}, 4);
    CHECK_EQ(malformed["error"]["code"].get<int>(), rpc_code::kInvalidParams);
}
