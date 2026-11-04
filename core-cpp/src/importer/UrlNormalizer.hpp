#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace dropship::importer {

// Приводит ссылку на товар поставщика к виду, по которому товары дедуплицируются:
// схема и хост в нижнем регистре, без порта по умолчанию, без #фрагмента, без
// трекинговых параметров (utm_*, gclid, fbclid, ...), параметры отсортированы,
// хвостовой "/" в пути убран. nullopt — если это не http(s)-ссылка или она
// содержит пробелы/управляющие символы/user-info (user@host скрывает настоящий хост).
std::optional<std::string> normalizeUrl(std::string_view url);

}  // namespace dropship::importer
