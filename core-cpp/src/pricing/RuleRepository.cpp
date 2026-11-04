#include "pricing/RuleRepository.hpp"

#include <variant>

namespace dropship::pricing {

// Правила магазина. Повреждённые строки не должны ломать мониторинг: набор считается пустым,
// цена остаётся прежней (это видно по счётчику `unpriced`).
std::optional<RuleSet> loadRuleSet(db::Tx& tx, std::int64_t storeId) {
    const auto rows = tx.query(
        "SELECT price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, marketplace_fee_percent, "
        "rounding_cents, min_stock_threshold, min_profit_cents FROM pricing_rules WHERE store_id = ?",
        {storeId});
    if (!rows || rows->rows.empty()) return std::nullopt;
    std::vector<PricingRule> rules;
    for (std::size_t i = 0; i < rows->rows.size(); ++i) {
        RuleSpec spec;
        spec.priceFromCents = rows->intAt(i, "price_from_cents");
        spec.priceToCents = rows->intAt(i, "price_to_cents");
        spec.marginPercent = std::get<double>(rows->at(i, "margin_percent"));
        spec.fixedMarkupCents = rows->intAt(i, "fixed_markup_cents");
        const auto& fee = rows->at(i, "marketplace_fee_percent");
        spec.marketplaceFeePercent = std::holds_alternative<double>(fee) ? std::get<double>(fee) : 0.0;
        if (!rows->isNull(i, "rounding_cents")) spec.roundingCents = static_cast<int>(rows->intAt(i, "rounding_cents"));
        else spec.roundingCents = std::nullopt;
        spec.minStockThreshold = rows->intAt(i, "min_stock_threshold");
        spec.minProfitCents = rows->intAt(i, "min_profit_cents");
        auto rule = PricingRule::fromSpec(spec);
        if (!rule) return std::nullopt;
        rules.push_back(*rule);
    }
    auto set = RuleSet::create(std::move(rules));
    if (!set) return std::nullopt;
    return std::move(*set);
}

}  // namespace dropship::pricing
