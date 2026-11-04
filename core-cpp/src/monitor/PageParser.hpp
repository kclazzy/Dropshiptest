#pragma once

// Разбор страницы поставщика в текущее состояние товара (цена, остаток) — без сети.
// Логика зеркалит расширение (extension-mv3/src/shared/extract.ts): JSON-LD Product -> meta-теги.
// Дублирование осознанное: расширение работает в браузере, ядро — без него.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Domain.hpp"
#include "core/Expected.hpp"

namespace dropship::monitor {

inline constexpr std::int64_t kDefaultInStockQuantity = 10;  // «в наличии» без количества

struct ObservedVariant {
    std::string sku;  // может быть пустым: тогда вариант сопоставляется по порядку/единственности
    Cents priceCents = 0;
    std::int64_t stock = 0;
};

// "1 234,56 ₽" | "1,234.56" -> копейки. nullopt, если цены нет.
std::optional<Cents> parsePriceToCents(std::string_view raw);

// Тело HTML -> варианты. Ошибка — если на странице не найдено ни одной цены.
Expected<std::vector<ObservedVariant>, std::string> parseProductPage(std::string_view html);

}  // namespace dropship::monitor
