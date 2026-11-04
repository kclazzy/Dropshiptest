#pragma once

// Правила цен магазина из таблицы pricing_rules. Единственное место, где строки БД превращаются в RuleSet.

#include <optional>

#include "db/DatabaseManager.hpp"
#include "pricing/PricingEngine.hpp"

namespace dropship::pricing {

// nullopt, если правил нет или они некорректны/пересекаются: повреждённые правила не должны
// ломать мониторинг и заказы — вызывающий код трактует это как «правила не заданы».
std::optional<RuleSet> loadRuleSet(db::Tx& tx, std::int64_t storeId);

}  // namespace dropship::pricing
