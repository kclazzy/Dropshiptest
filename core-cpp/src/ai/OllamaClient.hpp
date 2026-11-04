#pragma once

// Клиент локальной LLM (Ollama): запрос /api/chat со Structured Outputs
// (JSON Schema в поле `format`) и строгая проверка ответа.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "net/HttpTransport.hpp"

namespace dropship::ai {

inline constexpr std::string_view kDefaultLocalModel = "qwen3.5:9b-q4_K_M";

struct OllamaConfig {
    std::string baseUrl = "http://127.0.0.1:11434";
    std::string model{kDefaultLocalModel};
    std::string keepAlive = "30m";       // держать модель в VRAM
    std::string outputLanguage = "ru";   // язык карточки: ru, en, ...
    double temperature = 0.4;
    int contextTokens = 8192;            // options.num_ctx
    std::optional<bool> think;           // не задано — параметр не отправляется
    std::chrono::seconds timeout{300};
};

struct ProductSource {
    std::string title;
    std::string description;
    std::vector<std::pair<std::string, std::string>> attributes;
};

struct GeneratedContent {
    std::string seoTitle;
    std::string htmlDescription;  // уже очищено sanitizeHtml
    std::vector<std::string> bulletPoints;
    std::vector<std::string> searchTags;
};

enum class AiErrorKind {
    Transport,       // Ollama недоступна
    Http,            // статус != 200
    Malformed,       // ответ не разобрался как ожидается
    InvalidContent,  // JSON разобрался, но содержимое непригодно
};

struct AiError {
    AiErrorKind kind = AiErrorKind::Malformed;
    int httpStatus = 0;
    std::string message;

    // Имеет ли смысл повторить тот же запрос (сеть, 5xx/429, «плохая» генерация).
    bool retryable() const noexcept;
};

template <class T>
using Result = Expected<T, AiError>;

Json contentSchema();
Json buildChatRequest(const OllamaConfig& config, const ProductSource& source);
Result<GeneratedContent> parseChatResponse(int httpStatus, std::string_view body);

// Обрезает строку до maxBytes, не разрывая многобайтовый символ UTF-8.
std::string truncateUtf8(std::string_view text, std::size_t maxBytes);

enum class OllamaStatus { Ready, NotRunning, ModelMissing, Unexpected };

class OllamaClient {
public:
    OllamaClient(OllamaConfig config, std::shared_ptr<net::IHttpTransport> transport);

    Result<GeneratedContent> generate(const ProductSource& source);
    OllamaStatus checkStatus();

    const OllamaConfig& config() const noexcept { return config_; }

private:
    OllamaConfig config_;
    std::shared_ptr<net::IHttpTransport> transport_;
};

}  // namespace dropship::ai
