#pragma once

// Клиент локальной LLM (Ollama): запрос /api/chat со Structured Outputs
// (JSON Schema в поле `format`) и строгая проверка ответа.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "ai/ContentGenerator.hpp"
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

Json buildChatRequest(const OllamaConfig& config, const ProductSource& source);
Result<GeneratedContent> parseChatResponse(int httpStatus, std::string_view body);

enum class OllamaStatus { Ready, NotRunning, ModelMissing, Unexpected };

class OllamaClient final : public IContentGenerator {
public:
    OllamaClient(OllamaConfig config, std::shared_ptr<net::IHttpTransport> transport);

    Result<GeneratedContent> generate(const ProductSource& source) override;
    std::string name() const override { return "ollama:" + config_.model; }
    OllamaStatus checkStatus();

    const OllamaConfig& config() const noexcept { return config_; }

private:
    OllamaConfig config_;
    std::shared_ptr<net::IHttpTransport> transport_;
};

}  // namespace dropship::ai
