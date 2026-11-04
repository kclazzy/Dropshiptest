#include "ai/OllamaClient.hpp"

namespace dropship::ai {

namespace {

constexpr std::chrono::milliseconds kStatusTimeout{5000};

std::string joinUrl(std::string base, std::string_view path) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + std::string(path);
}

std::string errorText(int status, std::string_view body) {
    const Json parsed = parseJson(body);
    if (parsed.is_object()) {
        const auto it = parsed.find("error");
        if (it != parsed.end() && it->is_string()) return it->get<std::string>();
    }
    return "HTTP " + std::to_string(status);
}

AiError malformed(std::string message) { return AiError{AiErrorKind::Malformed, 200, std::move(message)}; }

}  // namespace

Json buildChatRequest(const OllamaConfig& config, const ProductSource& source) {
    Json request = Json::object();
    request["model"] = config.model;
    request["messages"] = Json::array({Json{{"role", "system"}, {"content", listingSystemPrompt(config.outputLanguage)}},
                                       Json{{"role", "user"}, {"content", listingUserPrompt(source)}}});
    request["stream"] = false;
    request["format"] = contentSchema();
    request["keep_alive"] = config.keepAlive;
    request["options"] = Json{{"temperature", config.temperature}, {"num_ctx", config.contextTokens}};
    if (config.think) request["think"] = *config.think;
    return request;
}

Result<GeneratedContent> parseChatResponse(int httpStatus, std::string_view body) {
    if (httpStatus != 200) {
        return unexpected(AiError{AiErrorKind::Http, httpStatus, errorText(httpStatus, body)});
    }

    const Json root = parseJson(body);
    if (!root.is_object()) return unexpected(malformed("response is not a JSON object"));
    const auto done = root.find("done");
    if (done != root.end() && done->is_boolean() && !done->get<bool>()) {
        return unexpected(malformed("response is incomplete (done=false)"));
    }
    const auto message = root.find("message");
    if (message == root.end() || !message->is_object() || !message->contains("content") ||
        !(*message)["content"].is_string()) {
        return unexpected(malformed("response has no message.content"));
    }

    const Json content = parseJson((*message)["content"].get<std::string>());
    if (!content.is_object()) return unexpected(malformed("message.content is not a JSON object"));
    return parseListing(content);
}

OllamaClient::OllamaClient(OllamaConfig config, std::shared_ptr<net::IHttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {}

Result<GeneratedContent> OllamaClient::generate(const ProductSource& source) {
    net::HttpRequest request;
    request.method = net::HttpRequest::Method::Post;
    request.url = joinUrl(config_.baseUrl, "/api/chat");
    request.body = dumpJson(buildChatRequest(config_, source));
    request.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(config_.timeout);

    const auto response = transport_->send(request);
    if (!response) return unexpected(AiError{AiErrorKind::Transport, 0, response.error().message});
    return parseChatResponse(response->status, response->body);
}

OllamaStatus OllamaClient::checkStatus() {
    net::HttpRequest request;
    request.method = net::HttpRequest::Method::Get;
    request.url = joinUrl(config_.baseUrl, "/api/tags");
    request.timeout = kStatusTimeout;

    const auto response = transport_->send(request);
    if (!response) return OllamaStatus::NotRunning;
    if (response->status != 200) return OllamaStatus::Unexpected;

    const Json root = parseJson(response->body);
    if (!root.is_object() || !root.contains("models") || !root["models"].is_array()) return OllamaStatus::Unexpected;

    const std::string withLatest = config_.model.find(':') == std::string::npos ? config_.model + ":latest" : "";
    const auto matches = [&](const Json& entry, const char* key) {
        if (!entry.contains(key) || !entry[key].is_string()) return false;
        const auto name = entry[key].get<std::string>();
        return name == config_.model || (!withLatest.empty() && name == withLatest);
    };
    for (const auto& entry : root["models"]) {
        if (entry.is_object() && (matches(entry, "name") || matches(entry, "model"))) return OllamaStatus::Ready;
    }
    return OllamaStatus::ModelMissing;
}

}  // namespace dropship::ai
