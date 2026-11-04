#include <memory>

#include "FakeTransport.hpp"
#include "Harness.hpp"
#include "TestHttpServer.hpp"
#include "ai/AiFactory.hpp"
#include "ai/CloudClient.hpp"
#include "net/CurlTransport.hpp"

using namespace dropship;
using namespace dropship::ai;

namespace {

ProductSource sample() { return {"Беспроводные наушники X1", "Bluetooth 5.3", {{"Цвет", "Чёрный"}}}; }

Json listing() {
    return Json{{"seo_title", "Наушники X1"},
                {"html_description", "<p>30 часов работы</p>"},
                {"bullet_points", Json::array({"Bluetooth 5.3"})},
                {"search_tags", Json::array({"наушники"})}};
}

std::string openAiBody(const Json& content, const std::string& finish = "stop") {
    return dumpJson(Json{{"choices", Json::array({Json{{"index", 0},
                                                       {"finish_reason", finish},
                                                       {"message", {{"role", "assistant"}, {"content", dumpJson(content)}}}}})}});
}

std::string anthropicBody(const Json& input, const std::string& stop = "tool_use") {
    return dumpJson(Json{{"type", "message"},
                         {"stop_reason", stop},
                         {"content", Json::array({Json{{"type", "text"}, {"text", "ok"}},
                                                  Json{{"type", "tool_use"}, {"name", "submit_listing"}, {"input", input}}})}});
}

CloudConfig openAi() {
    CloudConfig c;
    c.provider = "openai";
    c.baseUrl = "https://api.openai.com/v1";
    c.model = "some-model";
    c.apiKey = "sk-secret";
    return c;
}

CloudConfig anthropic() {
    CloudConfig c;
    c.style = ApiStyle::Anthropic;
    c.provider = "anthropic";
    c.baseUrl = "https://api.anthropic.com";
    c.model = "claude-haiku-5-5";
    c.apiKey = "ak-secret";
    return c;
}

std::string headerValue(const net::HttpRequest& r, const std::string& name) {
    for (const auto& [key, value] : r.headers) {
        if (key == name) return value;
    }
    return "<missing>";
}

}  // namespace

// ---- OpenAI-совместимый протокол ----

TEST(cloud_openai_request_uses_strict_json_schema) {
    const auto r = buildOpenAiRequest(openAi(), sample());
    CHECK(r.method == net::HttpRequest::Method::Post);
    CHECK_EQ(r.url, std::string("https://api.openai.com/v1/chat/completions"));
    CHECK_EQ(headerValue(r, "Authorization"), std::string("Bearer sk-secret"));

    const Json body = Json::parse(r.body);
    CHECK_EQ(body["model"].get<std::string>(), std::string("some-model"));
    REQUIRE(body["messages"].size() == 2);
    CHECK_EQ(body["messages"][0]["role"].get<std::string>(), std::string("system"));
    CHECK(body["messages"][1]["content"].get<std::string>().find("Беспроводные наушники X1") != std::string::npos);
    CHECK_EQ(body["response_format"]["type"].get<std::string>(), std::string("json_schema"));
    CHECK(body["response_format"]["json_schema"]["strict"].get<bool>());
    CHECK(body["response_format"]["json_schema"]["schema"] == strictContentSchema());
    CHECK(!body.contains("temperature"));  // не задана — не отправляется
}

TEST(cloud_strict_schema_forbids_extra_properties) {
    const Json strict = strictContentSchema();
    CHECK(strict["additionalProperties"] == false);
    CHECK(strict["required"] == contentSchema()["required"]);
    CHECK(strict["properties"] == contentSchema()["properties"]);
}

TEST(cloud_openai_json_object_mode_describes_the_structure_in_the_prompt) {
    CloudConfig c = openAi();
    c.jsonMode = JsonMode::JsonObject;
    c.temperature = 0.3;
    const Json body = Json::parse(buildOpenAiRequest(c, sample()).body);
    CHECK_EQ(body["response_format"]["type"].get<std::string>(), std::string("json_object"));
    CHECK(body["messages"][0]["content"].get<std::string>().find("seo_title") != std::string::npos);
    CHECK_EQ(body["temperature"].get<double>(), 0.3);
}

TEST(cloud_trailing_slash_in_base_url_is_harmless_and_extra_headers_are_sent) {
    CloudConfig c = openAi();
    c.baseUrl = "https://llm.api.cloud.yandex.net/v1/";
    c.extraHeaders = {{"OpenAI-Project", "b1gfolder"}};
    const auto r = buildOpenAiRequest(c, sample());
    CHECK_EQ(r.url, std::string("https://llm.api.cloud.yandex.net/v1/chat/completions"));
    CHECK_EQ(headerValue(r, "OpenAI-Project"), std::string("b1gfolder"));
    CHECK_EQ(headerValue(r, "Authorization"), std::string("Bearer sk-secret"));
}

TEST(cloud_openai_parses_a_valid_response) {
    const auto r = parseOpenAiResponse(200, openAiBody(listing()));
    REQUIRE(r.has_value());
    CHECK_EQ(r->seoTitle, std::string("Наушники X1"));
    CHECK_EQ(r->searchTags.size(), std::size_t{1});
}

TEST(cloud_openai_accepts_json_wrapped_in_a_code_fence) {
    const std::string wrapped = "```json\n" + dumpJson(listing()) + "\n```";
    const std::string body = dumpJson(Json{{"choices", Json::array({Json{{"finish_reason", "stop"},
                                                                         {"message", {{"content", wrapped}}}}})}});
    CHECK(parseOpenAiResponse(200, body).has_value());
}

TEST(cloud_openai_rejects_truncated_refused_and_empty_responses) {
    const auto truncated = parseOpenAiResponse(200, openAiBody(listing(), "length"));
    REQUIRE(!truncated.has_value());
    CHECK(truncated.error().kind == AiErrorKind::Malformed);
    CHECK(truncated.error().message.find("truncated") != std::string::npos);

    const std::string refusal = dumpJson(Json{{"choices", Json::array({Json{{"finish_reason", "stop"},
                                                                            {"message", {{"content", nullptr}, {"refusal", "I cannot help"}}}}})}});
    const auto refused = parseOpenAiResponse(200, refusal);
    REQUIRE(!refused.has_value());
    CHECK(refused.error().kind == AiErrorKind::InvalidContent);

    CHECK(parseOpenAiResponse(200, R"({"choices":[]})").error().kind == AiErrorKind::Malformed);
    CHECK(parseOpenAiResponse(200, "<html>").error().kind == AiErrorKind::Malformed);
    CHECK(parseOpenAiResponse(200, openAiBody(Json::array())).error().kind == AiErrorKind::Malformed);

    Json missing = listing();
    missing.erase("search_tags");
    CHECK(parseOpenAiResponse(200, openAiBody(missing)).error().kind == AiErrorKind::InvalidContent);
}

TEST(cloud_openai_classifies_http_errors) {
    const auto unauthorized = parseOpenAiResponse(401, R"({"error":{"message":"Incorrect API key","type":"invalid_request_error"}})");
    REQUIRE(!unauthorized.has_value());
    CHECK(unauthorized.error().kind == AiErrorKind::Http);
    CHECK_EQ(unauthorized.error().httpStatus, 401);
    CHECK(unauthorized.error().message.find("Incorrect API key") != std::string::npos);
    CHECK(!unauthorized.error().retryable());

    CHECK(parseOpenAiResponse(429, R"({"error":{"message":"rate limited"}})").error().retryable());
    CHECK(parseOpenAiResponse(503, "overloaded").error().retryable());
    CHECK(parseOpenAiResponse(400, R"({"error":"plain string error"})").error().message.find("plain string error") !=
          std::string::npos);
}

// ---- Anthropic ----

TEST(cloud_anthropic_request_forces_the_listing_tool) {
    const auto r = buildAnthropicRequest(anthropic(), sample());
    CHECK_EQ(r.url, std::string("https://api.anthropic.com/v1/messages"));
    CHECK_EQ(headerValue(r, "x-api-key"), std::string("ak-secret"));
    CHECK_EQ(headerValue(r, "anthropic-version"), std::string("2023-06-01"));
    CHECK_EQ(headerValue(r, "Authorization"), std::string("<missing>"));

    const Json body = Json::parse(r.body);
    CHECK_EQ(body["model"].get<std::string>(), std::string("claude-haiku-5-5"));
    CHECK_EQ(body["max_tokens"].get<int>(), 2048);
    CHECK(body["system"].is_string());
    REQUIRE(body["messages"].size() == 1);
    CHECK_EQ(body["messages"][0]["role"].get<std::string>(), std::string("user"));
    REQUIRE(body["tools"].size() == 1);
    CHECK_EQ(body["tools"][0]["name"].get<std::string>(), std::string("submit_listing"));
    CHECK(body["tools"][0]["input_schema"] == contentSchema());
    CHECK_EQ(body["tool_choice"]["type"].get<std::string>(), std::string("tool"));
    CHECK_EQ(body["tool_choice"]["name"].get<std::string>(), std::string("submit_listing"));
    CHECK(!body.contains("temperature"));
}

TEST(cloud_anthropic_parses_the_tool_result) {
    const auto r = parseAnthropicResponse(200, anthropicBody(listing()));
    REQUIRE(r.has_value());
    CHECK_EQ(r->seoTitle, std::string("Наушники X1"));
}

TEST(cloud_anthropic_rejects_truncated_and_toolless_responses) {
    CHECK(parseAnthropicResponse(200, anthropicBody(listing(), "max_tokens")).error().kind == AiErrorKind::Malformed);
    CHECK(parseAnthropicResponse(200, R"({"stop_reason":"end_turn","content":[{"type":"text","text":"hi"}]})").error().kind ==
          AiErrorKind::Malformed);
    CHECK(parseAnthropicResponse(200, R"({"content":"oops"})").error().kind == AiErrorKind::Malformed);
    Json bad = listing();
    bad["seo_title"] = 5;
    CHECK(parseAnthropicResponse(200, anthropicBody(bad)).error().kind == AiErrorKind::InvalidContent);
}

TEST(cloud_anthropic_classifies_http_errors) {
    const auto r = parseAnthropicResponse(
        401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    REQUIRE(!r.has_value());
    CHECK_EQ(r.error().httpStatus, 401);
    CHECK(r.error().message.find("invalid x-api-key") != std::string::npos);
    CHECK(!r.error().retryable());
    CHECK(parseAnthropicResponse(529, R"({"type":"error","error":{"type":"overloaded_error","message":"busy"}})").error().retryable());
    CHECK(parseAnthropicResponse(429, "{}").error().retryable());
}

// ---- клиент ----

TEST(cloud_client_sends_through_the_transport_and_never_exposes_the_key) {
    auto transport = std::make_shared<testfakes::FakeTransport>();
    transport->reply(200, openAiBody(listing()));
    CloudClient client(openAi(), transport);

    const auto r = client.generate(sample());
    REQUIRE(r.has_value());
    REQUIRE(transport->requests.size() == 1);
    CHECK_EQ(transport->requests[0].url, std::string("https://api.openai.com/v1/chat/completions"));
    CHECK_EQ(client.name(), std::string("openai:some-model"));
    CHECK(client.name().find("sk-secret") == std::string::npos);
}

TEST(cloud_client_uses_the_anthropic_protocol_when_configured) {
    auto transport = std::make_shared<testfakes::FakeTransport>();
    transport->reply(200, anthropicBody(listing()));
    CloudClient client(anthropic(), transport);
    REQUIRE(client.generate(sample()).has_value());
    CHECK_EQ(transport->requests[0].url, std::string("https://api.anthropic.com/v1/messages"));
}

TEST(cloud_client_reports_transport_failures_without_leaking_the_key) {
    auto transport = std::make_shared<testfakes::FakeTransport>();
    transport->fail("could not resolve host");
    CloudClient client(openAi(), transport);
    const auto r = client.generate(sample());
    REQUIRE(!r.has_value());
    CHECK(r.error().kind == AiErrorKind::Transport);
    CHECK(r.error().message.find("sk-secret") == std::string::npos);
}

TEST(cloud_client_works_over_real_http) {
    testnet::TestHttpServer server;
    server.responseBody = openAiBody(listing());
    CloudConfig c = openAi();
    c.baseUrl = server.url("/v1");
    CloudClient client(c, std::make_shared<net::CurlTransport>());

    const auto r = client.generate(sample());
    REQUIRE(r.has_value());
    REQUIRE(server.requests().size() == 1);
    CHECK_EQ(server.requests()[0].target, std::string("/v1/chat/completions"));
    CHECK_EQ(server.requests()[0].authorization, std::string("Bearer sk-secret"));
}

TEST(stripCodeFence_handles_plain_fenced_and_unclosed_text) {
    CHECK_EQ(stripCodeFence("{\"a\":1}"), std::string("{\"a\":1}"));
    CHECK_EQ(stripCodeFence("```json\n{\"a\":1}\n```"), std::string("{\"a\":1}"));
    CHECK_EQ(stripCodeFence("```\n{\"a\":1}```"), std::string("{\"a\":1}"));
    CHECK_EQ(stripCodeFence("  {\"a\":1}  "), std::string("{\"a\":1}"));
}
