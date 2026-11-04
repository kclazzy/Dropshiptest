//! Математическое ядро ценообразования.
//!
//! Все суммы — `i64` в минимальных единицах (копейки/центы), проценты внутри — базисные пункты
//! (1 % = 100 bp). Промежуточные расчёты идут в `i128` с проверкой переполнения; результат,
//! не помещающийся в `i64`, — ошибка `Overflow`, а не паника.
//!
//! Формула из ТЗ: `P = (C + L + F_pack) × (1 + M) / (1 − (fee + acquiring + tax))`.
//! Она эквивалентна требованию «чистая прибыль ≥ C·M + фиксированная наценка», где комиссии
//! считаются от розничной цены и **округляются вверх** (в пользу осторожности). Поэтому цена
//! ищется как наименьшая, при которой фактическая прибыль (с округлёнными комиссиями) не ниже цели.
//!
//! * `retail_price` — цена продажи; не ниже цены, дающей `min_profit_minor`;
//! * `min_price` — безубыточная цена (прибыль ≥ 0): ниже неё в акциях Ozon продавать нельзя;
//! * `old_price` — зачёркнутая цена: `retail / (1 − скидка)`.
//!
//! Округление (`rounding_minor`) — всегда вверх до кратного шага, прибыль от него не уменьшается.

use serde::Serialize;

#[cfg(test)]
mod tests;

/// Сумма в минимальных единицах валюты.
pub type Minor = i64;

const BP: i128 = 10_000;
/// Не больше 1000 % наценки: защита от опечатки «500» вместо «50».
const MAX_MARGIN_BP: i64 = 100_000;
/// Сколько раз можно сдвинуть цену на 1 единицу, исправляя погрешность округления комиссий.
const MAX_CORRECTION_STEPS: u32 = 1_000;

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum PricingError {
    #[error("отрицательное значение: {0}")]
    NegativeAmount(&'static str),
    #[error("себестоимость должна быть больше нуля")]
    NonPositiveCost,
    #[error("некорректный процент: {0}")]
    InvalidPercent(&'static str),
    #[error("сумма комиссий, эквайринга и налога должна быть меньше 100 %")]
    FeesTooHigh,
    #[error("некорректный диапазон цен правила")]
    InvalidRange,
    #[error("диапазоны правил пересекаются")]
    OverlappingRules,
    #[error("не задано ни одного правила")]
    EmptyRuleSet,
    #[error("число слишком большое для расчёта")]
    Overflow,
}

/// Строка правила в «сыром» виде, как она хранится в `pricing_rules` (проценты — `f64`).
#[derive(Debug, Clone, PartialEq)]
pub struct RuleSpec {
    pub price_from_minor: Minor,
    pub price_to_minor: Minor,
    pub margin_percent: f64,
    pub fixed_markup_minor: Minor,
    pub marketplace_fee_percent: f64,
    pub acquiring_and_tax_percent: f64,
    pub discount_striken_percent: f64,
    pub rounding_minor: Minor,
    pub min_stock_threshold: i64,
    pub min_profit_minor: Minor,
}

/// Проверенное правило: создать невалидное нельзя.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PricingRule {
    price_from: Minor,
    price_to: Minor,
    margin_bp: i64,
    fixed_markup: Minor,
    fee_bp: i64,
    acquiring_tax_bp: i64,
    discount_bp: i64,
    rounding: Minor,
    min_stock_threshold: i64,
    min_profit: Minor,
}

fn percent_to_bp(value: f64, name: &'static str, max_bp: i64) -> Result<i64, PricingError> {
    if !value.is_finite() || value < 0.0 {
        return Err(PricingError::InvalidPercent(name));
    }
    let bp = (value * 100.0).round();
    if bp > max_bp as f64 {
        return Err(PricingError::InvalidPercent(name));
    }
    Ok(bp as i64)
}

fn non_negative(value: Minor, name: &'static str) -> Result<Minor, PricingError> {
    if value < 0 {
        Err(PricingError::NegativeAmount(name))
    } else {
        Ok(value)
    }
}

impl PricingRule {
    pub fn new(spec: RuleSpec) -> Result<Self, PricingError> {
        if spec.price_from_minor < 0 || spec.price_to_minor <= spec.price_from_minor {
            return Err(PricingError::InvalidRange);
        }
        let margin_bp = percent_to_bp(spec.margin_percent, "margin_percent", MAX_MARGIN_BP)?;
        let fee_bp = percent_to_bp(spec.marketplace_fee_percent, "marketplace_fee_percent", 10_000)?;
        let acquiring_tax_bp =
            percent_to_bp(spec.acquiring_and_tax_percent, "acquiring_and_tax_percent", 10_000)?;
        let discount_bp = percent_to_bp(spec.discount_striken_percent, "discount_striken_percent", 9_999)?;
        if fee_bp + acquiring_tax_bp >= 10_000 {
            return Err(PricingError::FeesTooHigh);
        }
        Ok(Self {
            price_from: spec.price_from_minor,
            price_to: spec.price_to_minor,
            margin_bp,
            fixed_markup: non_negative(spec.fixed_markup_minor, "fixed_markup_minor")?,
            fee_bp,
            acquiring_tax_bp,
            discount_bp,
            rounding: non_negative(spec.rounding_minor, "rounding_minor")?,
            min_stock_threshold: spec.min_stock_threshold,
            min_profit: non_negative(spec.min_profit_minor, "min_profit_minor")?,
        })
    }

    pub fn price_from_minor(&self) -> Minor {
        self.price_from
    }
    pub fn price_to_minor(&self) -> Minor {
        self.price_to
    }
    pub fn min_stock_threshold(&self) -> i64 {
        self.min_stock_threshold
    }
    pub fn min_profit_minor(&self) -> Minor {
        self.min_profit
    }
    fn contains(&self, supplier_price: Minor) -> bool {
        supplier_price >= self.price_from && supplier_price < self.price_to
    }
}

/// Набор непересекающихся диапазонов `[от, до)` по закупочной цене одного магазина.
#[derive(Debug, Clone)]
pub struct RuleSet {
    rules: Vec<PricingRule>,
}

impl RuleSet {
    pub fn new(mut rules: Vec<PricingRule>) -> Result<Self, PricingError> {
        if rules.is_empty() {
            return Err(PricingError::EmptyRuleSet);
        }
        rules.sort_by_key(|r| r.price_from);
        if rules.windows(2).any(|w| w[0].price_to > w[1].price_from) {
            return Err(PricingError::OverlappingRules);
        }
        Ok(Self { rules })
    }

    /// `None`, если для закупочной цены правила нет (дыра между диапазонами).
    pub fn find(&self, supplier_price: Minor) -> Option<&PricingRule> {
        self.rules.iter().find(|r| r.contains(supplier_price))
    }
}

/// Остаток для публикации: 0, если ниже порога правила или товар нельзя доставить.
pub fn stock_to_publish(rule: &PricingRule, supplier_stock: i64, deliverable: bool) -> i64 {
    if !deliverable || supplier_stock < rule.min_stock_threshold || supplier_stock < 0 {
        0
    } else {
        supplier_stock
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PriceInput {
    pub supplier_price_minor: Minor,
    pub logistics_minor: Minor,
    pub packaging_minor: Minor,
}

/// Наглядная раскладка цены — ровно то, что показывает визуальный калькулятор в интерфейсе.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct PriceBreakdown {
    pub supplier_price: Minor,
    pub logistics: Minor,
    pub packaging: Minor,
    /// Закупка + доставка + упаковка.
    pub total_cost: Minor,
    pub marketplace_fee: Minor,
    pub acquiring_and_tax: Minor,
    pub net_profit: Minor,
    pub retail_price: Minor,
    /// Безубыточная цена (для участия в акциях Ozon без убытка).
    pub min_price: Minor,
    /// Зачёркнутая цена до скидки.
    pub old_price: Minor,
    /// Чистая прибыль от розничной цены, в базисных пунктах (2633 = 26,33 %).
    pub profit_margin_bp: i64,
    /// Цена поднята выше «наценочной», чтобы выполнить минимальную прибыль.
    pub raised_to_min_profit: bool,
}

fn to_i64(value: i128) -> Result<i64, PricingError> {
    i64::try_from(value).map_err(|_| PricingError::Overflow)
}

/// ⌈a·num / den⌉ для неотрицательных `a`, `num` и положительного `den`.
fn mul_div_ceil(a: i128, num: i128, den: i128) -> Result<i128, PricingError> {
    let product = a.checked_mul(num).ok_or(PricingError::Overflow)?;
    Ok((product + den - 1) / den)
}

fn fees(price: Minor, rule: &PricingRule) -> Result<(Minor, Minor), PricingError> {
    let marketplace = to_i64(mul_div_ceil(price as i128, rule.fee_bp as i128, BP)?)?;
    let acquiring = to_i64(mul_div_ceil(price as i128, rule.acquiring_tax_bp as i128, BP)?)?;
    Ok((marketplace, acquiring))
}

/// Чистая прибыль при цене `price`: цена − комиссия − эквайринг/налог − себестоимость.
pub fn profit_at(price: Minor, total_cost: Minor, rule: &PricingRule) -> Result<Minor, PricingError> {
    let (marketplace, acquiring) = fees(price, rule)?;
    to_i64(price as i128 - marketplace as i128 - acquiring as i128 - total_cost as i128)
}

fn round_up(price: Minor, step: Minor) -> Result<Minor, PricingError> {
    if step <= 1 {
        return Ok(price);
    }
    let steps = mul_div_ceil(price as i128, 1, step as i128)?;
    to_i64(steps.checked_mul(step as i128).ok_or(PricingError::Overflow)?)
}

/// Наименьшая цена (с учётом округления вверх до `rounding`), при которой прибыль ≥ `target`.
fn price_for_profit(total_cost: Minor, target: Minor, rule: &PricingRule) -> Result<Minor, PricingError> {
    let total_bp = (rule.fee_bp + rule.acquiring_tax_bp) as i128;
    let needed = (total_cost as i128).checked_add(target as i128).ok_or(PricingError::Overflow)?;
    let mut price = to_i64(mul_div_ceil(needed, BP, BP - total_bp)?)?;
    for _ in 0..MAX_CORRECTION_STEPS {
        // Комиссии округляются вверх по отдельности, поэтому прибыль может оказаться на 1–2 единицы ниже цели.
        let candidate = round_up(price, rule.rounding)?;
        if profit_at(candidate, total_cost, rule)? >= target {
            return Ok(candidate);
        }
        price = candidate.checked_add(1).ok_or(PricingError::Overflow)?;
    }
    Err(PricingError::Overflow)
}

/// Рассчитывает розничную, минимальную и зачёркнутую цены и наглядную раскладку.
pub fn calculate(rule: &PricingRule, input: &PriceInput) -> Result<PriceBreakdown, PricingError> {
    non_negative(input.supplier_price_minor, "supplier_price_minor")?;
    non_negative(input.logistics_minor, "logistics_minor")?;
    non_negative(input.packaging_minor, "packaging_minor")?;
    let total_cost = to_i64(
        input.supplier_price_minor as i128 + input.logistics_minor as i128 + input.packaging_minor as i128,
    )?;
    if total_cost <= 0 {
        return Err(PricingError::NonPositiveCost);
    }

    let margin_amount = mul_div_ceil(total_cost as i128, rule.margin_bp as i128, BP)?;
    let markup_target = to_i64(margin_amount + rule.fixed_markup as i128)?;
    let target = markup_target.max(rule.min_profit);
    let retail_price = price_for_profit(total_cost, target, rule)?;
    let min_price = price_for_profit(total_cost, 0, rule)?;

    let old_price = if rule.discount_bp == 0 {
        retail_price
    } else {
        let raw = to_i64(mul_div_ceil(retail_price as i128, BP, BP - rule.discount_bp as i128)?)?;
        round_up(raw, rule.rounding)?
    };

    let (marketplace_fee, acquiring_and_tax) = fees(retail_price, rule)?;
    let net_profit = profit_at(retail_price, total_cost, rule)?;
    // retail_price > 0, так как себестоимость положительна.
    let profit_margin_bp = to_i64((net_profit as i128).max(0) * BP / retail_price as i128)?;

    Ok(PriceBreakdown {
        supplier_price: input.supplier_price_minor,
        logistics: input.logistics_minor,
        packaging: input.packaging_minor,
        total_cost,
        marketplace_fee,
        acquiring_and_tax,
        net_profit,
        retail_price,
        min_price,
        old_price,
        profit_margin_bp,
        raised_to_min_profit: rule.min_profit > markup_target,
    })
}
