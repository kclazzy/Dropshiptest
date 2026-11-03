#pragma once

// Математическое ядро ценообразования. Только чистые функции, без ввода-вывода.
// Все суммы — int64_t в минимальных единицах валюты, проценты — в базисных пунктах
// (1 bp = 0.01 %), поэтому float/double нигде не участвуют в расчёте цены.
//
//   P = ceil( (C + S + F) * (10000 + margin_bp) / (10000 - fee_bp) )
//
// C — закупка, S — доставка, F — фиксированная наценка. Затем цена поднимается
// до ближайшего значения, оканчивающегося на заданные копейки (например .99).

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "core/Domain.hpp"
#include "core/Expected.hpp"

namespace dropship::pricing {

inline constexpr std::int64_t kBasisPoints = 10'000;
inline constexpr Cents kUnboundedPrice = std::numeric_limits<Cents>::max();

enum class PricingError {
    InvalidRule,       // значения правила вне допустимых границ
    OverlappingRules,  // диапазоны цен пересекаются
    NegativeAmount,    // отрицательная закупка/доставка/цена
    Overflow,          // результат не помещается в int64_t
};

constexpr std::string_view toString(PricingError e) noexcept {
    switch (e) {
        case PricingError::InvalidRule: return "invalid pricing rule";
        case PricingError::OverlappingRules: return "overlapping pricing rules";
        case PricingError::NegativeAmount: return "negative amount";
        case PricingError::Overflow: return "arithmetic overflow";
    }
    return "unknown pricing error";
}

template <class T>
using Result = Expected<T, PricingError>;

// Сырые значения строки pricing_rules (проценты как в БД — double).
struct RuleSpec {
    Cents priceFromCents = 0;          // включительно, по закупочной цене
    Cents priceToCents = kUnboundedPrice;  // исключительно
    double marginPercent = 0.0;
    Cents fixedMarkupCents = 0;
    double marketplaceFeePercent = 0.0;
    std::optional<int> roundingCents = 99;  // 0..99; nullopt — без округления
    std::int64_t minStockThreshold = 5;
    Cents minProfitCents = 200;
};

// Проверенное правило: невалидное правило создать невозможно.
class PricingRule {
public:
    static Result<PricingRule> fromSpec(const RuleSpec& spec);

    Cents priceFromCents() const noexcept { return priceFrom_; }
    Cents priceToCents() const noexcept { return priceTo_; }
    std::int64_t marginBp() const noexcept { return marginBp_; }
    Cents fixedMarkupCents() const noexcept { return fixedMarkup_; }
    std::int64_t marketplaceFeeBp() const noexcept { return feeBp_; }
    std::optional<int> roundingCents() const noexcept { return rounding_; }
    std::int64_t minStockThreshold() const noexcept { return minStock_; }
    Cents minProfitCents() const noexcept { return minProfit_; }

    bool contains(Cents supplierPrice) const noexcept {
        return supplierPrice >= priceFrom_ && supplierPrice < priceTo_;
    }

private:
    PricingRule() = default;

    Cents priceFrom_ = 0;
    Cents priceTo_ = kUnboundedPrice;
    std::int64_t marginBp_ = 0;
    Cents fixedMarkup_ = 0;
    std::int64_t feeBp_ = 0;
    std::optional<int> rounding_;
    std::int64_t minStock_ = 0;
    Cents minProfit_ = 0;
};

// Набор непересекающихся диапазонов одного магазина.
class RuleSet {
public:
    static Result<RuleSet> create(std::vector<PricingRule> rules);

    // nullptr, если для закупочной цены нет правила (дыра между диапазонами).
    const PricingRule* find(Cents supplierPrice) const noexcept;

private:
    explicit RuleSet(std::vector<PricingRule> rules) : rules_(std::move(rules)) {}
    std::vector<PricingRule> rules_;
};

struct PriceInput {
    Cents supplierPriceCents = 0;
    Cents shippingCents = 0;
};

struct PriceQuote {
    Cents retailCents = 0;
    Cents netProfitCents = 0;  // после комиссии площадки, закупки и доставки
    bool meetsMinProfit = false;
};

Result<PriceQuote> quote(const PricingRule& rule, const PriceInput& input);

// Чистая прибыль продажи: retail - комиссия площадки (округляется вверх) - cost.
Result<Cents> netProfit(const PricingRule& rule, Cents retailCents, Cents costCents);

// Остаток для публикации: 0, если ниже порога или товар недоступен к доставке.
constexpr std::int64_t stockToPublish(const PricingRule& rule, std::int64_t supplierStock,
                                      bool deliverable = true) noexcept {
    return (!deliverable || supplierStock < rule.minStockThreshold()) ? 0 : supplierStock;
}

}  // namespace dropship::pricing
