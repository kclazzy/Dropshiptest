use super::*;

fn spec() -> RuleSpec {
    RuleSpec {
        price_from_minor: 0,
        price_to_minor: i64::MAX,
        margin_percent: 50.0,
        fixed_markup_minor: 0,
        marketplace_fee_percent: 15.0,
        acquiring_and_tax_percent: 6.0,
        discount_striken_percent: 30.0,
        rounding_minor: 0,
        min_stock_threshold: 5,
        min_profit_minor: 0,
    }
}

fn rule(f: impl FnOnce(&mut RuleSpec)) -> PricingRule {
    let mut s = spec();
    f(&mut s);
    PricingRule::new(s).expect("valid rule")
}

fn input(cost: i64) -> PriceInput {
    PriceInput { supplier_price_minor: cost, logistics_minor: 0, packaging_minor: 0 }
}

#[test]
fn standard_rule_gives_exact_prices() {
    // Закупка 1000 ₽, наценка 50 %, комиссии 15 % + 6 %.
    let b = calculate(&rule(|_| {}), &input(100_000)).unwrap();
    assert_eq!(b.retail_price, 189_875);
    assert_eq!(b.net_profit, 50_000);
    assert_eq!(b.marketplace_fee, 28_482);
    assert_eq!(b.acquiring_and_tax, 11_393);
    assert_eq!(b.min_price, 126_583); // безубыточность
    assert_eq!(b.old_price, 271_250); // 189 875 / (1 − 30 %)
    assert_eq!(b.profit_margin_bp, 2_633);
    assert!(!b.raised_to_min_profit);
}

#[test]
fn logistics_and_packaging_are_part_of_the_cost() {
    let with_extras = calculate(
        &rule(|_| {}),
        &PriceInput { supplier_price_minor: 80_000, logistics_minor: 15_000, packaging_minor: 5_000 },
    )
    .unwrap();
    let plain = calculate(&rule(|_| {}), &input(100_000)).unwrap();
    assert_eq!(with_extras.total_cost, 100_000);
    assert_eq!(with_extras.retail_price, plain.retail_price);
}

#[test]
fn fixed_markup_is_added_and_one_unit_corrections_keep_profit() {
    let b = calculate(&rule(|s| { s.margin_percent = 0.0; s.fixed_markup_minor = 10_000; }), &input(100_000)).unwrap();
    assert_eq!(b.retail_price, 139_242);
    assert_eq!(b.net_profit, 10_000);
}

#[test]
fn min_profit_raises_the_price_and_says_so() {
    let b = calculate(&rule(|s| s.min_profit_minor = 60_000), &input(100_000)).unwrap();
    assert_eq!(b.retail_price, 202_532);
    assert_eq!(b.net_profit, 60_000);
    assert!(b.raised_to_min_profit);
}

#[test]
fn rounding_goes_up_and_never_loses_profit() {
    let b = calculate(&rule(|s| s.rounding_minor = 1_000), &input(100_000)).unwrap();
    assert_eq!(b.retail_price, 190_000);
    assert_eq!(b.net_profit, 50_100);
    assert_eq!(b.min_price % 1_000, 0);
    assert_eq!(b.old_price % 1_000, 0);
}

#[test]
fn zero_discount_keeps_old_price_equal_to_retail() {
    let b = calculate(&rule(|s| s.discount_striken_percent = 0.0), &input(100_000)).unwrap();
    assert_eq!(b.old_price, b.retail_price);
}

#[test]
fn invariants_hold_over_a_wide_sweep() {
    for margin in [0.0, 0.5, 12.5, 30.0, 99.9, 300.0] {
        for fee in [0.0, 1.0, 15.0, 33.3, 60.0] {
            for step in [0, 1, 99, 1_000] {
                for min_profit in [0, 1, 30_000] {
                    let r = rule(|s| {
                        s.margin_percent = margin;
                        s.marketplace_fee_percent = fee;
                        s.rounding_minor = step;
                        s.min_profit_minor = min_profit;
                    });
                    for cost in [1, 7, 99, 1_000, 12_345, 99_999, 1_234_567, 987_654_321] {
                        let b = calculate(&r, &input(cost)).unwrap();
                        let target = (cost * (margin * 100.0).round() as i64 + 9_999) / 10_000;
                        assert!(b.net_profit >= min_profit.max(target), "profit {b:?}");
                        assert!(b.min_price <= b.retail_price, "{b:?}");
                        assert!(b.retail_price <= b.old_price, "{b:?}");
                        assert_eq!(b.net_profit, b.retail_price - b.marketplace_fee - b.acquiring_and_tax - b.total_cost);
                        if step == 0 {
                            // Цена минимальна: на копейку дешевле цель уже не достигается.
                            let lower = profit_at(b.retail_price - 1, b.total_cost, &r).unwrap();
                            assert!(lower < min_profit.max(target), "not minimal {b:?}");
                        }
                    }
                }
            }
        }
    }
}

#[test]
fn bad_input_is_an_error_not_a_panic() {
    let r = rule(|_| {});
    assert_eq!(calculate(&r, &input(0)), Err(PricingError::NonPositiveCost));
    assert_eq!(calculate(&r, &input(-5)), Err(PricingError::NegativeAmount("supplier_price_minor")));
    assert_eq!(
        calculate(&r, &PriceInput { supplier_price_minor: 1, logistics_minor: -1, packaging_minor: 0 }),
        Err(PricingError::NegativeAmount("logistics_minor"))
    );
    assert_eq!(calculate(&r, &input(i64::MAX)), Err(PricingError::Overflow));
    assert_eq!(
        calculate(&r, &PriceInput { supplier_price_minor: i64::MAX, logistics_minor: 1, packaging_minor: 0 }),
        Err(PricingError::Overflow)
    );
}

#[test]
fn invalid_rules_are_rejected() {
    let bad = |f: fn(&mut RuleSpec)| {
        let mut s = spec();
        f(&mut s);
        PricingRule::new(s)
    };
    assert_eq!(bad(|s| s.marketplace_fee_percent = 95.0).unwrap_err(), PricingError::FeesTooHigh);
    assert_eq!(bad(|s| s.marketplace_fee_percent = -1.0).unwrap_err(), PricingError::InvalidPercent("marketplace_fee_percent"));
    assert_eq!(bad(|s| s.margin_percent = f64::NAN).unwrap_err(), PricingError::InvalidPercent("margin_percent"));
    assert_eq!(bad(|s| s.margin_percent = f64::INFINITY).unwrap_err(), PricingError::InvalidPercent("margin_percent"));
    assert_eq!(bad(|s| s.discount_striken_percent = 100.0).unwrap_err(), PricingError::InvalidPercent("discount_striken_percent"));
    assert_eq!(bad(|s| s.rounding_minor = -1).unwrap_err(), PricingError::NegativeAmount("rounding_minor"));
    assert_eq!(bad(|s| s.fixed_markup_minor = -1).unwrap_err(), PricingError::NegativeAmount("fixed_markup_minor"));
    assert_eq!(bad(|s| s.min_profit_minor = -1).unwrap_err(), PricingError::NegativeAmount("min_profit_minor"));
    assert_eq!(bad(|s| { s.price_from_minor = 10; s.price_to_minor = 10; }).unwrap_err(), PricingError::InvalidRange);
    assert_eq!(bad(|s| s.price_from_minor = -1).unwrap_err(), PricingError::InvalidRange);
}

#[test]
fn rule_set_finds_ranges_and_rejects_overlaps() {
    let low = rule(|s| { s.price_from_minor = 0; s.price_to_minor = 100_000; s.margin_percent = 80.0; });
    let high = rule(|s| { s.price_from_minor = 100_000; s.price_to_minor = i64::MAX; s.margin_percent = 40.0; });
    let set = RuleSet::new(vec![high.clone(), low.clone()]).unwrap(); // порядок не важен
    assert_eq!(set.find(0), Some(&low));
    assert_eq!(set.find(99_999), Some(&low));
    assert_eq!(set.find(100_000), Some(&high)); // верхняя граница исключается
    assert_eq!(set.find(-1), None);

    let overlapping = rule(|s| { s.price_from_minor = 50_000; s.price_to_minor = 150_000; });
    assert_eq!(RuleSet::new(vec![low.clone(), overlapping]).unwrap_err(), PricingError::OverlappingRules);
    let gap_low = rule(|s| s.price_to_minor = 10);
    let gap_high = rule(|s| s.price_from_minor = 20);
    assert_eq!(RuleSet::new(vec![gap_low, gap_high]).unwrap().find(15), None); // дыра — правила нет
    assert_eq!(RuleSet::new(vec![]).unwrap_err(), PricingError::EmptyRuleSet);
}

#[test]
fn stock_protection_zeroes_low_or_undeliverable_stock() {
    let r = rule(|s| s.min_stock_threshold = 5);
    assert_eq!(stock_to_publish(&r, 4, true), 0);
    assert_eq!(stock_to_publish(&r, 5, true), 5);
    assert_eq!(stock_to_publish(&r, 50, false), 0);
    assert_eq!(stock_to_publish(&r, -3, true), 0);
}
