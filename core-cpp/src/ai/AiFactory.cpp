#include "ai/AiFactory.hpp"

#include <algorithm>

namespace dropship::ai {

namespace {

const ProviderPreset* findPreset(const std::string& id) {
    for (const auto& preset : providerPresets()) {
        if (preset.id == id) return &preset;
    }
    return nullptr;
}

std::string trim(const std::string& text) {
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end && isSpace(*begin)) ++begin;
    while (end != begin && isSpace(*(end - 1))) --end;
    return std::string(begin, end);
}

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0; }

std::string hostOf(const std::string& url) {
    const auto start = url.find("://") + 3;
    if (start < url.size() && url[start] == '[') {
        const auto close = url.find(']', start);
        return url.substr(start, close == std::string::npos ? std::string::npos : close - start + 1);
    }
    const auto end = url.find_first_of("/:?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool isLoopbackHost(const std::string& host) {
    return host == "localhost" || host == "[::1]" || host.rfind("127.", 0) == 0;
}

bool validHeaderName(const std::string& name) {
    if (name.empty()) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    });
}

bool validHeaderValue(const std::string& value) {
    return std::none_of(value.begin(), value.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return (u < 0x20 && u != '\t') || u == 0x7F;
    });
}

Expected<std::string, std::string> optionalSetting(const Json& section, const char* key) {
    const auto it = section.find(key);
    if (it == section.end() || it->is_null()) return std::string();
    if (!it->is_string()) return unexpected(std::string("ai.") + key + " must be a string");
    return it->get<std::string>();
}

}  // namespace

const std::vector<ProviderPreset>& providerPresets() {
    static const std::vector<ProviderPreset> kPresets = {
        {"ollama", "Ollama (локально, бесплатно)", true, ApiStyle::OpenAiCompatible, "http://127.0.0.1:11434", "",
         JsonMode::JsonSchema, "Данные не покидают компьютер."},
        {"openai", "OpenAI", false, ApiStyle::OpenAiCompatible, "https://api.openai.com/v1", "OPENAI_API_KEY",
         JsonMode::JsonSchema, ""},
        {"anthropic", "Anthropic Claude", false, ApiStyle::Anthropic, "https://api.anthropic.com", "ANTHROPIC_API_KEY",
         JsonMode::JsonSchema, "Структурный ответ через tool use."},
        {"openrouter", "OpenRouter (много моделей)", false, ApiStyle::OpenAiCompatible, "https://openrouter.ai/api/v1",
         "OPENROUTER_API_KEY", JsonMode::JsonObject, ""},
        {"groq", "Groq", false, ApiStyle::OpenAiCompatible, "https://api.groq.com/openai/v1", "GROQ_API_KEY",
         JsonMode::JsonObject, ""},
        {"together", "Together AI", false, ApiStyle::OpenAiCompatible, "https://api.together.xyz/v1", "TOGETHER_API_KEY",
         JsonMode::JsonObject, ""},
        {"mistral", "Mistral AI", false, ApiStyle::OpenAiCompatible, "https://api.mistral.ai/v1", "MISTRAL_API_KEY",
         JsonMode::JsonObject, ""},
        {"deepseek", "DeepSeek", false, ApiStyle::OpenAiCompatible, "https://api.deepseek.com", "DEEPSEEK_API_KEY",
         JsonMode::JsonObject, "Адрес и режим JSON не сверены с документацией — проверьте при подключении."},
        {"gemini", "Google Gemini", false, ApiStyle::OpenAiCompatible,
         "https://generativelanguage.googleapis.com/v1beta/openai", "GEMINI_API_KEY", JsonMode::JsonSchema,
         "Если модель отклоняет json_schema, задайте json_mode=json_object."},
        {"yandex", "Yandex AI Studio (YandexGPT)", false, ApiStyle::OpenAiCompatible, "https://llm.api.cloud.yandex.net/v1",
         "YANDEX_API_KEY", JsonMode::JsonSchema,
         "Модель: gpt://<ID каталога>/yandexgpt/latest; добавьте extra_headers {\"OpenAI-Project\": \"<ID каталога>\"}."},
        {"custom", "Свой OpenAI-совместимый сервер", false, ApiStyle::OpenAiCompatible, "", "AI_API_KEY",
         JsonMode::JsonObject, "Укажите base_url (vLLM, LM Studio, корпоративный шлюз и т. п.)."},
    };
    return kPresets;
}

Expected<AiSettings, std::string> parseAiSettings(const Json& root) {
    const Json* section = &root;
    if (root.is_object() && root.contains("ai")) {
        if (!root["ai"].is_object()) return unexpected(std::string("ai must be an object"));
        section = &root["ai"];
    }
    if (!section->is_object()) return unexpected(std::string("settings must be a JSON object"));

    AiSettings settings;
    for (const auto& [key, target] : std::initializer_list<std::pair<const char*, std::string*>>{
             {"provider", &settings.provider}, {"model", &settings.model}, {"base_url", &settings.baseUrl},
             {"api_key_env", &settings.apiKeyEnv}, {"language", &settings.language}}) {
        auto value = optionalSetting(*section, key);
        if (!value) return unexpected(value.error());
        if (!value->empty()) *target = std::move(*value);
    }

    if (const auto mode = section->find("json_mode"); mode != section->end() && !mode->is_null()) {
        if (*mode == Json("json_schema")) {
            settings.jsonMode = JsonMode::JsonSchema;
        } else if (*mode == Json("json_object")) {
            settings.jsonMode = JsonMode::JsonObject;
        } else {
            return unexpected(std::string("ai.json_mode must be \"json_schema\" or \"json_object\""));
        }
    }

    if (const auto headers = section->find("extra_headers"); headers != section->end() && !headers->is_null()) {
        if (!headers->is_object()) return unexpected(std::string("ai.extra_headers must be an object of strings"));
        for (const auto& [name, value] : headers->items()) {
            if (!value.is_string()) return unexpected(std::string("ai.extra_headers must be an object of strings"));
            settings.extraHeaders.emplace_back(name, value.get<std::string>());
        }
    }
    return settings;
}

Expected<std::unique_ptr<IContentGenerator>, std::string> makeContentGenerator(
    const AiSettings& settings, std::shared_ptr<net::IHttpTransport> transport, const ApiKeyLookup& lookupKey) {
    const ProviderPreset* preset = findPreset(settings.provider);
    if (preset == nullptr) {
        std::string known;
        for (const auto& p : providerPresets()) known += (known.empty() ? "" : ", ") + p.id;
        return unexpected("unknown AI provider '" + settings.provider + "'. Known providers: " + known);
    }
    for (const auto& [name, value] : settings.extraHeaders) {
        if (!validHeaderName(name) || !validHeaderValue(value)) {
            return unexpected(std::string("ai.extra_headers contains an invalid header"));
        }
    }

    const std::string baseUrl = settings.baseUrl.empty() ? preset->baseUrl : settings.baseUrl;

    if (preset->local) {
        if (!isHttpUrl(baseUrl)) return unexpected(std::string("base_url must start with http:// or https://"));
        OllamaConfig config;
        config.baseUrl = baseUrl;
        if (!settings.model.empty()) config.model = settings.model;
        config.outputLanguage = settings.language;
        return std::unique_ptr<IContentGenerator>(std::make_unique<OllamaClient>(std::move(config), std::move(transport)));
    }

    if (settings.model.empty()) {
        return unexpected("model is required for provider '" + preset->id + "' (set ai.model in settings.json)");
    }
    if (baseUrl.empty()) return unexpected("base_url is required for provider '" + preset->id + "'");
    if (!isHttpUrl(baseUrl)) return unexpected(std::string("base_url must start with http:// or https://"));
    if (baseUrl.rfind("http://", 0) == 0 && !isLoopbackHost(hostOf(baseUrl))) {
        return unexpected(std::string("the API key must not be sent over plain http: use https:// (or a loopback address)"));
    }

    const std::string keyName = settings.apiKeyEnv.empty() ? preset->apiKeyEnv : settings.apiKeyEnv;
    const auto secret = lookupKey(keyName);
    const std::string apiKey = secret ? trim(*secret) : std::string();
    if (apiKey.empty()) {
        return unexpected("API key not found: set the environment variable " + keyName + " for provider '" + preset->id + "'");
    }

    CloudConfig config;
    config.style = preset->style;
    config.provider = preset->id;
    config.baseUrl = baseUrl;
    config.model = settings.model;
    config.apiKey = apiKey;
    config.jsonMode = settings.jsonMode.value_or(preset->jsonMode);
    config.outputLanguage = settings.language;
    config.extraHeaders = settings.extraHeaders;
    return std::unique_ptr<IContentGenerator>(std::make_unique<CloudClient>(std::move(config), std::move(transport)));
}

}  // namespace dropship::ai
