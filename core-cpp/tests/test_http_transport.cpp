#include <atomic>

#include "Harness.hpp"
#include "TestHttpServer.hpp"
#include "net/CurlTransport.hpp"

using namespace dropship;
using namespace dropship::net;
using namespace std::chrono_literals;

namespace {

HttpRequest get(const std::string& url, std::chrono::milliseconds timeout = 5s) {
    HttpRequest r;
    r.method = HttpRequest::Method::Get;
    r.url = url;
    r.timeout = timeout;
    return r;
}

}  // namespace

TEST(curl_get_returns_status_and_body) {
    testnet::TestHttpServer server;
    server.responseBody = R"({"ok":true})";
    CurlTransport transport;
    const auto r = transport.send(get(server.url("/api/tags")));
    REQUIRE(r.has_value());
    CHECK_EQ(r->status, 200);
    CHECK_EQ(r->body, std::string(R"({"ok":true})"));
    REQUIRE(server.requests().size() == 1);
    CHECK_EQ(server.requests()[0].method, std::string("GET"));
    CHECK_EQ(server.requests()[0].target, std::string("/api/tags"));
}

TEST(curl_post_sends_json_body) {
    testnet::TestHttpServer server;
    CurlTransport transport;
    HttpRequest request;
    request.method = HttpRequest::Method::Post;
    request.url = server.url("/api/chat");
    request.body = R"({"model":"m","текст":"привет"})";
    const auto r = transport.send(request);
    REQUIRE(r.has_value());
    REQUIRE(server.requests().size() == 1);
    CHECK_EQ(server.requests()[0].method, std::string("POST"));
    CHECK_EQ(server.requests()[0].body, request.body);
    CHECK_EQ(server.requests()[0].contentType, std::string("application/json"));
}

TEST(curl_sends_custom_headers) {
    testnet::TestHttpServer server;
    CurlTransport transport;
    HttpRequest request = get(server.url("/x"));
    request.headers = {{"Authorization", "Bearer secret"}, {"x-api-key", "k1"}};
    REQUIRE(transport.send(request).has_value());
    CHECK_EQ(server.requests()[0].authorization, std::string("Bearer secret"));
    CHECK_EQ(server.requests()[0].xApiKey, std::string("k1"));
}

TEST(curl_http_error_statuses_are_responses_not_transport_errors) {
    testnet::TestHttpServer server;
    server.status = 404;
    server.responseBody = R"({"error":"model not found"})";
    CurlTransport transport;
    const auto r = transport.send(get(server.url()));
    REQUIRE(r.has_value());
    CHECK_EQ(r->status, 404);
    CHECK_EQ(r->body, std::string(R"({"error":"model not found"})"));
}

TEST(curl_reports_connection_refused) {
    unsigned short deadPort = 0;
    {
        testnet::TestHttpServer server;  // порт освобождается при выходе из области видимости
        deadPort = server.port();
    }
    CurlTransport transport;
    const auto r = transport.send(get("http://127.0.0.1:" + std::to_string(deadPort) + "/"));
    REQUIRE(!r.has_value());
    CHECK(!r.error().message.empty());
}

TEST(curl_times_out_on_a_slow_server) {
    testnet::TestHttpServer server;
    server.delay = 1500ms;
    CurlTransport transport;
    const auto started = std::chrono::steady_clock::now();
    const auto r = transport.send(get(server.url(), 300ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(!r.has_value());
    CHECK(elapsed < 1200ms);
}

TEST(curl_rejects_oversized_responses) {
    testnet::TestHttpServer server;
    server.responseBody = std::string(5000, 'x');
    CurlTransport transport(1000);
    const auto r = transport.send(get(server.url()));
    REQUIRE(!r.has_value());
    CHECK(r.error().message.find("too large") != std::string::npos);
}

TEST(curl_refuses_non_http_schemes) {
    CurlTransport transport;
    for (const char* url : {"file:///etc/passwd", "ftp://127.0.0.1/x", "gopher://127.0.0.1/", "not a url"}) {
        CHECK(!transport.send(get(url)).has_value());
    }
}

TEST(curl_does_not_follow_redirects) {
    testnet::TestHttpServer server;
    server.status = 302;
    server.responseBody = "";
    CurlTransport transport;
    const auto r = transport.send(get(server.url()));
    REQUIRE(r.has_value());
    CHECK_EQ(r->status, 302);
    CHECK_EQ(server.requests().size(), std::size_t{1});
}
