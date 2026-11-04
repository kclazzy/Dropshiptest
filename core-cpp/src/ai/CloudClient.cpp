#include "ai/CloudClient.hpp"

namespace dropship::ai {

namespace {

constexpr const char* kAnthropicVersion = "2023-06-01";
constexpr const char* kAnthropicToolName = "submit_listing";

std::string joinUrl(std::string base, std::string_view path) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + std::string(path);
}

// Поддерживает {"error":{"message":...}} (OpenAI, Anthropic) и {"error":"..."}.
std::string errorText(int status, std::string_view body) {
    const Json parsed = parseJson(body);
    if (parsed.is_object()) {
        const auto error = parsed.find("error");
        if (error != parsed.end()) {
            if (error->is_string()) return error->get<std::string>();
            if (error->is_object() && error->contains("message") && (*error)["message"].is_string()) {
                return (*error)["message"].get<std::string>();
            }
        }
    }
    return "HTTP " + std::to_string(status);
}

AiError httpError(int status, std::string_view body) { return AiError{AiErrorKind::Http, status, errorText(status, body)}; }
AiError malformed(std::string message) { return AiError{AiErrorKind::Malformed, 200, std::move(message)}; }

net::HttpRequest baseRequest(const CloudConfig& config, std::string_view path) {
    net::HttpRequest request;
    request.method = net::HttpRequest::Method::Post;
    request.url = joinUrl(config.baseUrl, path);
    request.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(config.timeout);
    return request;
}

std::string redact(std::string text, const std::string& secret) {
    if (secret.empty()) return text;
    for (auto pos = text.find(secret); pos != std::string::npos; pos = text.find(secret, pos + 3)) {
        text.replace(pos, secret.size(), "***");
    }
    return text;
}

}  // namespace

Json strictContentSchema() {
    Json schema = contentSchema();
    schema["additionalProperties"] = false;
    return schema;
}

net::HttpRequest buildOpenAiRequest(const CloudConfig& config, const ProductSource& source) {
    std::string system = listingSystemPrompt(config.outputLanguage);
    if (config.jsonMode == JsonMode::JsonObject) system += "\n" + listingJsonInstruction();

    Json body = Json::object();
    body["model"] = config.model;
    body["messages"] = Json::array({Json{{"role", "system"}, {"content", system}},
                                    Json{{"role", "user"}, {"content", listingUserPrompt(source)}}});
    if (config.temperature) body["temperature"] = *config.temperature;
    if (config.jsonMode == JsonMode::JsonSchema) {
        body["response_format"] = Json{{"type", "json_schema"},
                                       {"json_schema", {{"name", "product_listing"}, {"strict", true}, {"schema", strictContentSchema()}}}};
    } else {
        body["response_format"] = Json{{"type", "json_object"}};
    }

    auto request = baseRequest(config, "/chat/completions");
    request.body = dumpJson(body);
    request.headers.emplace_back("Authorization", "Bearer " + config.apiKey);
    for (const auto& header : config.extraHeaders) request.headers.push_back(header);
    return request;
}

net::HttpRequest buildAnthropicRequest(const CloudConfig& config, const ProductSource& source) {
    Json body = Json::object();
    body["model"] = config.model;
    body["max_tokens"] = config.maxOutputTokens;
    body["system"] = listingSystemPrompt(config.outputLanguage);
    body["messages"] = Json::array({Json{{"role", "user"}, {"content", listingUserPrompt(source)}}});
    body["tools"] = Json::array({Json{{"name", kAnthropicToolName},
                                      {"description", "Submit the finished product listing."},
                                      {"input_schema", contentSchema()}}});
    body["tool_choice"] = Json{{"type", "tool"}, {"name", kAnthropicToolName}};
    if (config.temperature) body["temperature"] = *config.temperature;

    auto request = baseRequest(config, "/v1/messages");
    request.body = dumpJson(body);
    request.headers.emplace_back("x-api-key", config.apiKey);
    request.headers.emplace_back("anthropic-version", kAnthropicVersion);
    for (const auto& header : config.extraHeaders) request.headers.push_back(header);
    return request;
}

Result<GeneratedContent> parseOpenAiResponse(int httpStatus, std::string_view body) {
    if (httpStatus != 200) return unexpected(httpError(httpStatus, body));

    const Json root = parseJson(body);
    if (!root.is_object()) return unexpected(malformed("response is not a JSON object"));
    const auto choices = root.find("choices");
    if (choices == root.end() || !choices->is_array() || choices->empty() || !(*choices)[0].is_object()) {
        return unexpected(malformed("response has no choices"));
    }
    const Json& choice = (*choices)[0];

    if (choice.contains("finish_reason") && choice["finish_reason"] == Json("length")) {
        return unexpected(malformed("output truncated (finish_reason=length); raise the output limit"));
    }
    if (!choice.contains("message") || !choice["message"].is_object()) return unexpected(malformed("choice has no message"));
    const Json& message = choice["message"];

    if (message.contains("refusal") && message["refusal"].is_string() && !message["refusal"].get<std::string>().empty()) {
        return unexpected(AiError{AiErrorKind::InvalidContent, 200, "model refused: " + message["refusal"].get<std::string>()});
    }
    if (!message.contains("content") || !message["content"].is_string()) {
        return unexpected(malformed("message has no text content"));
    }
    const Json content = parseJson(stripCodeFence(message["content"].get<std::string>()));
    if (!content.is_object()) return unexpected(malformed("message.content is not a JSON object"));
    return parseListing(content);
}

Result<GeneratedContent> parseAnthropicResponse(int httpStatus, std::string_view body) {
    if (httpStatus != 200) return unexpected(httpError(httpStatus, body));

    const Json root = parseJson(body);
    if (!root.is_object()) return unexpected(malformed("response is not a JSON object"));
    if (root.contains("stop_reason") && root["stop_reason"] == Json("max_tokens")) {
        return unexpected(malformed("output truncated (stop_reason=max_tokens); raise the output limit"));
    }
    if (root.contains("stop_reason") && root["stop_reason"] == Json("refusal")) {
        return unexpected(AiError{AiErrorKind::InvalidContent, 200, "model refused the request"});
    }
    const auto content = root.find("content");
    if (content == root.end() || !content->is_array()) return unexpected(malformed("response has no content blocks"));

    for (const auto& block : *content) {
        if (block.is_object() && block.contains("type") && block["type"] == Json("tool_use") && block.contains("input") &&
            block["input"].is_object()) {
            return parseListing(block["input"]);
        }
    }
    return unexpected(malformed("response has no tool_use block"));
}

CloudClient::CloudClient(CloudConfig config, std::shared_ptr<net::IHttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {}

Result<GeneratedContent> CloudClient::generate(const ProductSource& source) {
    const auto request = config_.style == ApiStyle::Anthropic ? buildAnthropicRequest(config_, source)
                                                              : buildOpenAiRequest(config_, source);
    const auto response = transport_->send(request);
    if (!response) {
        return unexpected(AiError{AiErrorKind::Transport, 0, redact(response.error().message, config_.apiKey)});
    }
    return config_.style == ApiStyle::Anthropic ? parseAnthropicResponse(response->status, response->body)
                                                : parseOpenAiResponse(response->status, response->body);
}

}  // namespace dropship::ai
