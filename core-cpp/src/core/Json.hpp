#pragma once

// Единая точка работы с JSON: исключения библиотеки наружу не выходят.

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace dropship {

using Json = nlohmann::json;

// Внешние данные могут содержать невалидный UTF-8: заменяем его, а не бросаем исключение.
inline std::string dumpJson(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// is_discarded() == true, если текст не является валидным JSON.
inline Json parseJson(std::string_view text) {
    return Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
}

// Целое число в диапазоне int64 (не float, не строка, не uint64 больше INT64_MAX).
inline std::optional<std::int64_t> asInt64(const Json& value) {
    if (!value.is_number_integer()) return std::nullopt;
    if (value.is_number_unsigned()) {
        const auto u = value.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
        return static_cast<std::int64_t>(u);
    }
    return value.get<std::int64_t>();
}

}  // namespace dropship
