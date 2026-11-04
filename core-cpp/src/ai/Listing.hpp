#pragma once

// Общее для всех ИИ-провайдеров (локальных и облачных): что просим у модели,
// как проверяем ответ и какие бывают ошибки. Провайдеры отличаются только
// способом доставки запроса, поэтому промпт и проверка карточки определены здесь, в одном месте.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"

namespace dropship::ai {

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
    Transport,       // провайдер недоступен
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

// JSON Schema карточки из технического задания.
Json contentSchema();

// Обрезает строку до maxBytes, не разрывая многобайтовый символ UTF-8.
std::string truncateUtf8(std::string_view text, std::size_t maxBytes);

std::string listingSystemPrompt(const std::string& languageCode);
std::string listingUserPrompt(const ProductSource& source);
// Для режимов без схемы (json_object): словесное описание ожидаемой структуры.
std::string listingJsonInstruction();

// Проверяет и очищает JSON карточки; единственное место, где решается, что считать годным ответом.
Result<GeneratedContent> parseListing(const Json& content);

// Некоторые модели оборачивают JSON в ```json ... ```; снимает такую обёртку.
std::string stripCodeFence(std::string_view text);

}  // namespace dropship::ai
