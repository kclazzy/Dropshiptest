#pragma once

// Облачные ИИ-провайдеры. Два протокола покрывают практически все сервисы:
//  * OpenAI-совместимый /chat/completions — OpenAI, OpenRouter, Groq, Together, Mistral, DeepSeek,
//    Gemini, Yandex AI Studio и любой другой сервер с таким API (в том числе локальный vLLM/LM Studio);
//  * Anthropic Messages API (структурный ответ через tool use).
// Промпт и проверка карточки общие с локальной моделью (Listing.hpp).

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ai/ContentGenerator.hpp"
#include "net/HttpTransport.hpp"

namespace dropship::ai {

enum class ApiStyle { OpenAiCompatible, Anthropic };

// JsonSchema — строгая схема (response_format=json_schema); JsonObject — любой JSON,
// структура описана в промпте (поддерживается практически везде).
enum class JsonMode { JsonSchema, JsonObject };

struct CloudConfig {
    ApiStyle style = ApiStyle::OpenAiCompatible;
    std::string provider = "cloud";  // для журналов
    std::string baseUrl;
    std::string model;
    std::string apiKey;              // не попадает ни в журналы, ни в сообщения об ошибках
    JsonMode jsonMode = JsonMode::JsonSchema;
    std::string outputLanguage = "ru";
    std::optional<double> temperature;  // не задана — параметр не отправляется (часть моделей его не принимает)
    int maxOutputTokens = 2048;         // обязателен для Anthropic
    std::chrono::seconds timeout{120};
    std::vector<std::pair<std::string, std::string>> extraHeaders;  // например OpenAI-Project для Yandex
};

// Схема для строгого режима OpenAI: как contentSchema(), но с additionalProperties=false.
Json strictContentSchema();

net::HttpRequest buildOpenAiRequest(const CloudConfig& config, const ProductSource& source);
net::HttpRequest buildAnthropicRequest(const CloudConfig& config, const ProductSource& source);
Result<GeneratedContent> parseOpenAiResponse(int httpStatus, std::string_view body);
Result<GeneratedContent> parseAnthropicResponse(int httpStatus, std::string_view body);

class CloudClient final : public IContentGenerator {
public:
    CloudClient(CloudConfig config, std::shared_ptr<net::IHttpTransport> transport);

    Result<GeneratedContent> generate(const ProductSource& source) override;
    std::string name() const override { return config_.provider + ":" + config_.model; }

private:
    CloudConfig config_;
    std::shared_ptr<net::IHttpTransport> transport_;
};

}  // namespace dropship::ai
