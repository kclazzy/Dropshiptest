#include "pricing/PricingEngine.hpp"

#include <algorithm>
#include <cmath>

namespace dropship::pricing {

namespace {

constexpr Cents kMax = std::numeric_limits<Cents>::max();
constexpr double kMaxPercent = 1'000'000.0;  // защита от переполнения при переводе в bp

// Все помощники работают только с неотрицательными числами.
std::optional<Cents> addChecked(Cents a, Cents b) noexcept {
    if (a > kMax - b) return std::nullopt;
    return a + b;
}

std::optional<Cents> mulChecked(Cents a, Cents b) noexcept {
    if (a == 0 || b == 0) return Cents{0};
    if (a > kMax / b) return std::nullopt;
    return a * b;
}

Cents ceilDiv(Cents numerator, Cents denominator) noexcept {
    return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

// Проценты из БД (double) переводятся в целые bp один раз, на границе.
Result<std::int64_t> percentToBp(double percent) {
    if (!std::isfinite(percent) || percent < 0.0 || percent > kMaxPercent) {
        return unexpected(PricingError::InvalidRule);
    }
    return static_cast<std::int64_t>(std::llround(percent * 100.0));
}

// Наименьшая цена >= price, у которой остаток от деления на 100 равен ending.
Result<Cents> applyEnding(Cents price, int ending) {
    if (price > kMax - 100) return unexpected(PricingError::Overflow);
    Cents candidate = price - price % 100 + ending;
    if (candidate < price) candidate += 100;
    return candidate;
}

}  // namespace

Result<PricingRule> PricingRule::fromSpec(const RuleSpec& spec) {
    if (spec.priceFromCents < 0 || spec.priceToCents <= spec.priceFromCents ||
        spec.fixedMarkupCents < 0 || spec.minStockThreshold < 0 || spec.minProfitCents < 0) {
        return unexpected(PricingError::InvalidRule);
    }
    if (spec.roundingCents && (*spec.roundingCents < 0 || *spec.roundingCents > 99)) {
        return unexpected(PricingError::InvalidRule);
    }

    const auto margin = percentToBp(spec.marginPercent);
    if (!margin) return unexpected(margin.error());
    const auto fee = percentToBp(spec.marketplaceFeePercent);
    if (!fee) return unexpected(fee.error());
    if (*fee >= kBasisPoints) return unexpected(PricingError::InvalidRule);

    PricingRule rule;
    rule.priceFrom_ = spec.priceFromCents;
    rule.priceTo_ = spec.priceToCents;
    rule.marginBp_ = *margin;
    rule.fixedMarkup_ = spec.fixedMarkupCents;
    rule.feeBp_ = *fee;
    rule.rounding_ = spec.roundingCents;
    rule.minStock_ = spec.minStockThreshold;
    rule.minProfit_ = spec.minProfitCents;
    return rule;
}

Result<RuleSet> RuleSet::create(std::vector<PricingRule> rules) {
    std::ranges::sort(rules, {}, &PricingRule::priceFromCents);
    for (std::size_t i = 1; i < rules.size(); ++i) {
        if (rules[i].priceFromCents() < rules[i - 1].priceToCents()) {
            return unexpected(PricingError::OverlappingRules);
        }
    }
    return RuleSet(std::move(rules));
}

const PricingRule* RuleSet::find(Cents supplierPrice) const noexcept {
    for (const auto& rule : rules_) {
        if (rule.contains(supplierPrice)) return &rule;
    }
    return nullptr;
}

Result<Cents> netProfit(const PricingRule& rule, Cents retailCents, Cents costCents) {
    if (retailCents < 0 || costCents < 0) return unexpected(PricingError::NegativeAmount);
    const auto feeNumerator = mulChecked(retailCents, rule.marketplaceFeeBp());
    if (!feeNumerator) return unexpected(PricingError::Overflow);
    const Cents fee = ceilDiv(*feeNumerator, kBasisPoints);
    return retailCents - fee - costCents;
}

Result<PriceQuote> quote(const PricingRule& rule, const PriceInput& input) {
    if (input.supplierPriceCents < 0 || input.shippingCents < 0) {
        return unexpected(PricingError::NegativeAmount);
    }

    const auto cost = addChecked(input.supplierPriceCents, input.shippingCents);
    if (!cost) return unexpected(PricingError::Overflow);
    const auto base = addChecked(*cost, rule.fixedMarkupCents());
    if (!base) return unexpected(PricingError::Overflow);

    const auto numerator = mulChecked(*base, kBasisPoints + rule.marginBp());
    if (!numerator) return unexpected(PricingError::Overflow);

    Cents retail = ceilDiv(*numerator, kBasisPoints - rule.marketplaceFeeBp());
    if (const auto ending = rule.roundingCents()) {
        const auto rounded = applyEnding(retail, *ending);
        if (!rounded) return unexpected(rounded.error());
        retail = *rounded;
    }

    const auto profit = netProfit(rule, retail, *cost);
    if (!profit) return unexpected(profit.error());
    return PriceQuote{retail, *profit, *profit >= rule.minProfitCents()};
}

}  // namespace dropship::pricing
