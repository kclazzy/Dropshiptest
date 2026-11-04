#include <set>

#include "FakeTransport.hpp"
#include "Harness.hpp"
#include "ai/AiFactory.hpp"

using namespace dropship;
using namespace dropship::ai;

namespace {

ApiKeyLookup keys(std::map<std::string, std::string> values) {
    return [values = std::move(values)](const std::string& name) -> std::optional<std::string> {
        const auto it = values.find(name);
        if (it == values.end()) return std::nullopt;
        return it->second;
    };
}

AiSettings settings(const std::string& provider, const std::string& model = "m") {
    AiSettings s;
    s.provider = provider;
    s.model = model;
    return s;
}

using Generator = Expected<std::unique_ptr<IContentGenerator>, std::string>;

Generator make(const AiSettings& s, const ApiKeyLookup& lookup = keys({}),
               std::shared_ptr<net::IHttpTransport> transport = std::make_shared<testfakes::FakeTransport>()) {
    return makeContentGenerator(s, std::move(transport), lookup);
}

bool failsMentioning(const Generator& g, const std::string& text) {
    return !g.has_value() && g.error().find(text) != std::string::npos;
}

Json listing() {
    return Json{{"seo_title", "T"}, {"html_description", "<p>d</p>"}, {"bullet_points", Json::array()}, {"search_tags", Json::array()}};
}

}  // namespace

// ---- пресеты ----

TEST(ai_presets_cover_the_common_providers_with_safe_urls) {
    std::set<std::string> ids;
    for (const auto& preset : providerPresets()) {
        CHECK(ids.insert(preset.id).second);  // идентификаторы уникальны
        if (!preset.local && !preset.baseUrl.empty()) CHECK(preset.baseUrl.rfind("https://", 0) == 0);
        if (!preset.local) CHECK(!preset.apiKeyEnv.empty());
    }
    for (const char* id : {"ollama", "openai", "anthropic", "openrouter", "groq", "deepseek", "gemini", "mistral",
                           "together", "yandex", "custom"}) {
        CHECK(ids.count(id) == 1);
    }
}

// ---- настройки ----

TEST(ai_settings_default_to_local_ollama) {
    const auto s = parseAiSettings(Json::object());
    REQUIRE(s.has_value());
    CHECK_EQ(s->provider, std::string("ollama"));
    CHECK_EQ(s->language, std::string("ru"));
    CHECK(!s->jsonMode.has_value());
    CHECK(parseAiSettings(Json{{"other", 1}}).has_value());
}

TEST(ai_settings_read_the_ai_section) {
    const Json root = Json::parse(R"({"ai":{"provider":"yandex","model":"gpt://f/yandexgpt/latest","language":"en",
        "base_url":"https://x.example/v1","api_key_env":"MY_KEY","json_mode":"json_object",
        "extra_headers":{"OpenAI-Project":"f"}}})");
    const auto s = parseAiSettings(root);
    REQUIRE(s.has_value());
    CHECK_EQ(s->provider, std::string("yandex"));
    CHECK_EQ(s->model, std::string("gpt://f/yandexgpt/latest"));
    CHECK_EQ(s->language, std::string("en"));
    CHECK_EQ(s->baseUrl, std::string("https://x.example/v1"));
    CHECK_EQ(s->apiKeyEnv, std::string("MY_KEY"));
    REQUIRE(s->jsonMode.has_value());
    CHECK(*s->jsonMode == JsonMode::JsonObject);
    REQUIRE(s->extraHeaders.size() == 1);
    CHECK_EQ(s->extraHeaders[0].first, std::string("OpenAI-Project"));
}

TEST(ai_settings_reject_wrong_types_and_name_the_field) {
    const auto bad = [](const char* json, const std::string& field) {
        const auto s = parseAiSettings(Json::parse(json));
        return !s.has_value() && s.error().find(field) != std::string::npos;
    };
    CHECK(bad(R"({"ai":{"provider":5}})", "provider"));
    CHECK(bad(R"({"ai":{"model":[]}})", "model"));
    CHECK(bad(R"({"ai":{"json_mode":"xml"}})", "json_mode"));
    CHECK(bad(R"({"ai":{"extra_headers":{"a":1}}})", "extra_headers"));
    CHECK(bad(R"({"ai":{"extra_headers":"x"}})", "extra_headers"));
    CHECK(bad(R"({"ai":"x"})", "ai"));
}

// ---- фабрика ----

TEST(ai_factory_builds_local_ollama_without_a_key) {
    auto g = make(AiSettings{});
    REQUIRE(g.has_value());
    CHECK_EQ((*g)->name(), std::string("ollama:qwen3.5:9b-q4_K_M"));

    AiSettings custom;
    custom.model = "llama3:8b";
    custom.baseUrl = "http://127.0.0.1:11500";
    auto g2 = make(custom);
    REQUIRE(g2.has_value());
    CHECK_EQ((*g2)->name(), std::string("ollama:llama3:8b"));
}

TEST(ai_factory_requires_a_key_and_names_the_variable) {
    CHECK(failsMentioning(make(settings("openai")), "OPENAI_API_KEY"));
    CHECK(failsMentioning(make(settings("anthropic")), "ANTHROPIC_API_KEY"));
    CHECK(make(settings("openai"), keys({{"OPENAI_API_KEY", "sk-1"}})).has_value());

    AiSettings overridden = settings("openai");
    overridden.apiKeyEnv = "MY_OWN_VAR";
    CHECK(failsMentioning(make(overridden, keys({{"OPENAI_API_KEY", "sk-1"}})), "MY_OWN_VAR"));
    CHECK(make(overridden, keys({{"MY_OWN_VAR", "sk-1"}})).has_value());

    CHECK(failsMentioning(make(settings("openai"), keys({{"OPENAI_API_KEY", "   "}})), "OPENAI_API_KEY"));
}

TEST(ai_factory_requires_a_model_for_cloud_providers) {
    CHECK(failsMentioning(make(settings("openai", ""), keys({{"OPENAI_API_KEY", "k"}})), "model"));
}

TEST(ai_factory_rejects_unknown_providers) {
    CHECK(failsMentioning(make(settings("skynet")), "skynet"));
}

TEST(ai_factory_custom_provider_needs_a_base_url) {
    AiSettings s = settings("custom");
    CHECK(failsMentioning(make(s, keys({{"AI_API_KEY", "k"}})), "base_url"));
    s.baseUrl = "https://llm.example.com/v1";
    CHECK(make(s, keys({{"AI_API_KEY", "k"}})).has_value());
}

TEST(ai_factory_never_sends_an_api_key_over_plain_http_to_a_remote_host) {
    AiSettings s = settings("custom");
    s.baseUrl = "http://llm.example.com/v1";
    CHECK(failsMentioning(make(s, keys({{"AI_API_KEY", "k"}})), "https"));

    s.baseUrl = "http://127.0.0.1:1234/v1";  // локальный vLLM / LM Studio
    CHECK(make(s, keys({{"AI_API_KEY", "k"}})).has_value());
    s.baseUrl = "http://localhost:1234/v1";
    CHECK(make(s, keys({{"AI_API_KEY", "k"}})).has_value());

    s.baseUrl = "ftp://llm.example.com";
    CHECK(!make(s, keys({{"AI_API_KEY", "k"}})).has_value());
    s.baseUrl = "not a url";
    CHECK(!make(s, keys({{"AI_API_KEY", "k"}})).has_value());
}

TEST(ai_factory_rejects_header_injection_in_extra_headers) {
    AiSettings s = settings("yandex");
    s.extraHeaders = {{"OpenAI-Project", "f\r\nX-Evil: 1"}};
    CHECK(!make(s, keys({{"YANDEX_API_KEY", "k"}})).has_value());
    s.extraHeaders = {{"Bad Name", "v"}};
    CHECK(!make(s, keys({{"YANDEX_API_KEY", "k"}})).has_value());
}

TEST(ai_factory_error_messages_never_contain_the_key) {
    AiSettings s = settings("custom");
    s.baseUrl = "http://llm.example.com/v1";
    const auto g = make(s, keys({{"AI_API_KEY", "super-secret-key"}}));
    REQUIRE(!g.has_value());
    CHECK(g.error().find("super-secret-key") == std::string::npos);
}

TEST(ai_factory_yandex_sends_bearer_key_and_project_header) {
    auto transport = std::make_shared<testfakes::FakeTransport>();
    transport->reply(200, dumpJson(Json{{"choices", Json::array({Json{{"finish_reason", "stop"},
                                                                      {"message", {{"content", dumpJson(listing())}}}}})}}));
    AiSettings s = settings("yandex", "gpt://b1gfolder/yandexgpt/latest");
    s.extraHeaders = {{"OpenAI-Project", "b1gfolder"}};
    auto g = make(s, keys({{"YANDEX_API_KEY", "yk"}}), transport);
    REQUIRE(g.has_value());
    REQUIRE((*g)->generate({"t", "d", {}}).has_value());

    const auto& request = transport->requests[0];
    CHECK_EQ(request.url, std::string("https://llm.api.cloud.yandex.net/v1/chat/completions"));
    bool bearer = false;
    bool project = false;
    for (const auto& [key, value] : request.headers) {
        if (key == "Authorization" && value == "Bearer yk") bearer = true;
        if (key == "OpenAI-Project" && value == "b1gfolder") project = true;
    }
    CHECK(bearer);
    CHECK(project);
}

TEST(ai_factory_anthropic_and_json_mode_override) {
    auto transport = std::make_shared<testfakes::FakeTransport>();
    transport->reply(200, dumpJson(Json{{"stop_reason", "tool_use"},
                                        {"content", Json::array({Json{{"type", "tool_use"}, {"input", listing()}}})}}));
    auto g = make(settings("anthropic", "claude-haiku-5-5"), keys({{"ANTHROPIC_API_KEY", "ak"}}), transport);
    REQUIRE(g.has_value());
    CHECK_EQ((*g)->name(), std::string("anthropic:claude-haiku-5-5"));
    REQUIRE((*g)->generate({"t", "d", {}}).has_value());
    CHECK_EQ(transport->requests[0].url, std::string("https://api.anthropic.com/v1/messages"));

    auto t2 = std::make_shared<testfakes::FakeTransport>();
    t2->reply(500, "x");
    AiSettings s = settings("gemini", "some-gemini-model");
    s.jsonMode = JsonMode::JsonObject;
    auto g2 = make(s, keys({{"GEMINI_API_KEY", "gk"}}), t2);
    REQUIRE(g2.has_value());
    (void)(*g2)->generate({"t", "d", {}});
    CHECK(Json::parse(t2->requests[0].body)["response_format"]["type"] == "json_object");
}
