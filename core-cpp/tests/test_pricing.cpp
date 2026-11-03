#include <cmath>
#include <limits>

#include "Harness.hpp"
#include "pricing/PricingEngine.hpp"

using namespace dropship;
using namespace dropship::pricing;

namespace {

template <class T>
T unwrap(Result<T>&& r) {
    if (!r) {
        ::testing::fail(__FILE__, __LINE__, std::string("unexpected error: ") + std::string(toString(r.error())));
        throw ::testing::Abort{};
    }
    return std::move(*r);
}

PricingRule makeRule(RuleSpec spec) { return unwrap(PricingRule::fromSpec(spec)); }

RuleSpec plain(double marginPercent, double feePercent, std::optional<int> rounding) {
    RuleSpec s;
    s.marginPercent = marginPercent;
    s.marketplaceFeePercent = feePercent;
    s.roundingCents = rounding;
    return s;
}

Cents retail(const PricingRule& rule, Cents supplier, Cents shipping = 0) {
    return unwrap(quote(rule, {supplier, shipping})).retailCents;
}

bool rejects(const RuleSpec& spec) {
    const auto r = PricingRule::fromSpec(spec);
    return !r && r.error() == PricingError::InvalidRule;
}

}  // namespace

TEST(pricing_formula_with_margin_and_fee) {
    // (10000 * 1.30) / 0.90 = 14444.44 -> вверх до 14445
    CHECK_EQ(retail(makeRule(plain(30, 10, std::nullopt)), 10000), 14445);
}

TEST(pricing_psychological_rounding_after_formula) {
    CHECK_EQ(retail(makeRule(plain(30, 10, 99)), 10000), 14499);
}

TEST(pricing_shipping_and_fixed_markup_are_part_of_cost_base) {
    RuleSpec s = plain(20, 0, std::nullopt);
    s.fixedMarkupCents = 300;
    // (5000 + 500 + 300) * 1.20
    CHECK_EQ(retail(makeRule(s), 5000, 500), 6960);
}

TEST(pricing_fee_only_is_grossed_up) {
    CHECK_EQ(retail(makeRule(plain(0, 15, std::nullopt)), 8500), 10000);
}

TEST(pricing_percent_conversion_has_no_float_drift) {
    CHECK_EQ(retail(makeRule(plain(12.34, 0, std::nullopt)), 10000), 11234);
    CHECK_EQ(retail(makeRule(plain(0.07, 0, std::nullopt)), 10000), 10007);
}

TEST(pricing_rounding_picks_smallest_price_not_below_formula) {
    struct Row { Cents supplier; std::optional<int> rounding; Cents expected; };
    const Row rows[] = {
        {1000, 99, 1099}, {1099, 99, 1099}, {1100, 99, 1199}, {1, 99, 99},
        {1250, 0, 1300},  {1300, 0, 1300},  {1295, 90, 1390}, {1290, 90, 1290},
        {1000, std::nullopt, 1000},
    };
    for (const auto& row : rows) {
        const auto rule = makeRule(plain(0, 0, row.rounding));
        CHECK_EQ(retail(rule, row.supplier), row.expected);
    }
}

TEST(pricing_net_profit_deducts_fee_cost_and_shipping) {
    const auto rule = makeRule(plain(30, 10, 99));
    const auto q = unwrap(quote(rule, {10000, 0}));
    CHECK_EQ(q.retailCents, 14499);
    // комиссия ceil(1449.9) = 1450; 14499 - 1450 - 10000
    CHECK_EQ(q.netProfitCents, 3049);
}

TEST(pricing_fee_is_rounded_up_in_profit) {
    const auto rule = makeRule(plain(0, 10, std::nullopt));
    CHECK_EQ(unwrap(netProfit(rule, 1999, 1500)), 299);  // 1999 - 200 - 1500
    CHECK_EQ(unwrap(netProfit(rule, 20000, 15000)), 3000);
}

TEST(pricing_min_profit_flag_is_inclusive) {
    RuleSpec s = plain(30, 10, 99);
    s.minProfitCents = 3049;
    CHECK(unwrap(quote(makeRule(s), {10000, 0})).meetsMinProfit);
    s.minProfitCents = 3050;
    CHECK(!unwrap(quote(makeRule(s), {10000, 0})).meetsMinProfit);
}

TEST(pricing_rule_set_selects_by_supplier_price_half_open) {
    RuleSpec low = plain(50, 0, std::nullopt);
    low.priceFromCents = 0;
    low.priceToCents = 10000;
    RuleSpec mid = plain(30, 0, std::nullopt);
    mid.priceFromCents = 10000;
    mid.priceToCents = 50000;
    RuleSpec high = plain(20, 0, std::nullopt);
    high.priceFromCents = 50000;  // открытый верх
    // Порядок во входе намеренно перемешан.
    const auto set = unwrap(RuleSet::create({makeRule(high), makeRule(low), makeRule(mid)}));

    const auto marginAt = [&](Cents p) -> std::int64_t {
        const auto* r = set.find(p);
        return r ? r->marginBp() : -1;
    };
    CHECK_EQ(marginAt(0), 5000);
    CHECK_EQ(marginAt(9999), 5000);
    CHECK_EQ(marginAt(10000), 3000);
    CHECK_EQ(marginAt(49999), 3000);
    CHECK_EQ(marginAt(50000), 2000);
    CHECK_EQ(marginAt(std::numeric_limits<Cents>::max() - 1), 2000);
}

TEST(pricing_rule_set_reports_gaps_as_no_rule) {
    RuleSpec s = plain(10, 0, std::nullopt);
    s.priceFromCents = 100;
    s.priceToCents = 200;
    const auto set = unwrap(RuleSet::create({makeRule(s)}));
    CHECK(set.find(99) == nullptr);
    CHECK(set.find(100) != nullptr);
    CHECK(set.find(200) == nullptr);
}

TEST(pricing_rule_set_rejects_overlap) {
    RuleSpec a = plain(10, 0, std::nullopt);
    a.priceFromCents = 0;
    a.priceToCents = 100;
    RuleSpec b = plain(10, 0, std::nullopt);
    b.priceFromCents = 50;
    b.priceToCents = 200;
    const auto r = RuleSet::create({makeRule(a), makeRule(b)});
    REQUIRE(!r.has_value());
    CHECK(r.error() == PricingError::OverlappingRules);
}

TEST(pricing_invalid_rules_cannot_be_created) {
    CHECK(rejects(plain(10, 100, 99)));       // комиссия 100 % — деление на ноль
    CHECK(rejects(plain(10, -1, 99)));
    CHECK(rejects(plain(-0.01, 0, 99)));
    CHECK(rejects(plain(std::nan(""), 0, 99)));
    CHECK(rejects(plain(10, std::numeric_limits<double>::infinity(), 99)));
    CHECK(rejects(plain(10, 0, 100)));
    CHECK(rejects(plain(10, 0, -1)));

    RuleSpec s = plain(10, 0, 99);
    s.priceFromCents = 100;
    s.priceToCents = 100;
    CHECK(rejects(s));
    s = plain(10, 0, 99);
    s.priceFromCents = -1;
    CHECK(rejects(s));
    s = plain(10, 0, 99);
    s.minStockThreshold = -1;
    CHECK(rejects(s));
    s = plain(10, 0, 99);
    s.minProfitCents = -1;
    CHECK(rejects(s));
    s = plain(10, 0, 99);
    s.fixedMarkupCents = -1;
    CHECK(rejects(s));
}

TEST(pricing_negative_amounts_are_rejected) {
    const auto rule = makeRule(plain(10, 0, 99));
    const auto a = quote(rule, {-1, 0});
    REQUIRE(!a.has_value());
    CHECK(a.error() == PricingError::NegativeAmount);
    const auto b = quote(rule, {100, -1});
    REQUIRE(!b.has_value());
    CHECK(b.error() == PricingError::NegativeAmount);
}

TEST(pricing_overflow_is_an_error_not_undefined_behavior) {
    const auto doubling = makeRule(plain(100, 0, std::nullopt));
    const auto a = quote(doubling, {std::numeric_limits<Cents>::max() / 2, 0});
    REQUIRE(!a.has_value());
    CHECK(a.error() == PricingError::Overflow);

    const auto b = quote(doubling, {std::numeric_limits<Cents>::max(), 1});
    REQUIRE(!b.has_value());
    CHECK(b.error() == PricingError::Overflow);
}

TEST(pricing_stock_is_zeroed_below_threshold_or_when_undeliverable) {
    const auto rule = makeRule(plain(10, 0, 99));  // порог по умолчанию — 5
    CHECK_EQ(stockToPublish(rule, 4), 0);
    CHECK_EQ(stockToPublish(rule, 5), 5);
    CHECK_EQ(stockToPublish(rule, 40), 40);
    CHECK_EQ(stockToPublish(rule, 40, false), 0);
}
