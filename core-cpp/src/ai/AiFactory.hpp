#pragma once

// Выбор ИИ-провайдера по настройкам пользователя (settings.json, раздел "ai").
// Ключи API в настройки не пишутся: берутся из переменной окружения (или файла-секрета,
// это решает ApiKeyLookup), поэтому settings.json можно безопасно показывать и копировать.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ai/CloudClient.hpp"
#include "ai/OllamaClient.hpp"

namespace dropship::ai {

struct ProviderPreset {
    std::string id;           // значение "provider" в настройках
    std::string displayName;
    bool local = false;       // Ollama: ключ не нужен
    ApiStyle style = ApiStyle::OpenAiCompatible;
    std::string baseUrl;
    std::string apiKeyEnv;    // имя переменной окружения по умолчанию
    JsonMode jsonMode = JsonMode::JsonSchema;
    std::string note;
};

const std::vector<ProviderPreset>& providerPresets();

struct AiSettings {
    std::string provider = "ollama";
    std::string model;       // для ollama пусто = модель по умолчанию; для облака обязательна
    std::string baseUrl;     // переопределяет адрес пресета; обязателен для provider="custom"
    std::string apiKeyEnv;   // переопределяет имя переменной с ключом
    std::string language = "ru";
    std::optional<JsonMode> jsonMode;
    std::vector<std::pair<std::string, std::string>> extraHeaders;
};

// Читает объект "ai" из settings.json (или весь корень, если "ai" нет). Пустые настройки = локальная Ollama.
Expected<AiSettings, std::string> parseAiSettings(const Json& root);

// Значение секрета по имени переменной; nullopt, если не задано.
using ApiKeyLookup = std::function<std::optional<std::string>(const std::string& name)>;

Expected<std::unique_ptr<IContentGenerator>, std::string> makeContentGenerator(
    const AiSettings& settings, std::shared_ptr<net::IHttpTransport> transport, const ApiKeyLookup& lookupKey);

}  // namespace dropship::ai
