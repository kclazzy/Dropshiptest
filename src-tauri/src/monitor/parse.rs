//! Разбор страницы поставщика. Универсальный источник — разметка schema.org (JSON-LD `Product`/`Offer`),
//! которую отдаёт большинство магазинов. Если её нет, цены обновляются повторным импортом через расширение.

use scraper::{Html, Selector};
use serde_json::Value;

const NOMINAL_STOCK: i64 = 99;
const MAX_PRICE_MINOR: i64 = 1_000_000_000_000;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Offer {
    pub sku: Option<String>,
    pub price_minor: i64,
    pub stock: i64,
}

/// «1 299,50 ₽» / «1,299.50» / «1299» → копейки. Без чисел с плавающей точкой.
pub fn parse_price_minor(text: &str) -> Option<i64> {
    let cleaned: String = text.chars().filter(|c| c.is_ascii_digit() || matches!(c, '.' | ',')).collect();
    if cleaned.is_empty() || !cleaned.chars().any(|c| c.is_ascii_digit()) {
        return None;
    }
    let split = cleaned.rfind(['.', ',']).filter(|&i| (1..=2).contains(&(cleaned.len() - i - 1)));
    let (int_part, frac_part) = match split {
        Some(i) => (&cleaned[..i], &cleaned[i + 1..]),
        None => (cleaned.as_str(), ""),
    };
    let int_digits: String = int_part.chars().filter(char::is_ascii_digit).collect();
    let whole: i64 = if int_digits.is_empty() { 0 } else { int_digits.parse().ok()? };
    let frac: i64 = match frac_part.len() {
        0 => 0,
        1 => frac_part.parse::<i64>().ok()? * 10,
        _ => frac_part.parse().ok()?,
    };
    let total = whole.checked_mul(100)?.checked_add(frac)?;
    (total > 0 && total <= MAX_PRICE_MINOR).then_some(total)
}

fn price_of(value: &Value) -> Option<i64> {
    match value {
        Value::String(s) => parse_price_minor(s),
        Value::Number(n) => parse_price_minor(&n.to_string()),
        _ => None,
    }
}

fn offer_from(node: &Value) -> Option<Offer> {
    let price = node.get("price").and_then(price_of).or_else(|| node.get("lowPrice").and_then(price_of))?;
    let availability = node.get("availability").and_then(Value::as_str).unwrap_or("");
    let level = node.get("inventoryLevel").and_then(|v| v.get("value").or(Some(v))).and_then(|v| v.as_i64().or_else(|| v.as_str().and_then(|s| s.trim().parse().ok())));
    let in_stock = availability.is_empty() || availability.to_ascii_lowercase().contains("instock");
    let stock = match (in_stock, level) {
        (false, _) => 0,
        (true, Some(n)) => n.clamp(0, 1_000_000),
        (true, None) => NOMINAL_STOCK,
    };
    Some(Offer { sku: node.get("sku").and_then(Value::as_str).map(str::to_string), price_minor: price, stock })
}

fn collect_offers(node: &Value, inherited_sku: Option<&str>, out: &mut Vec<Offer>) {
    match node {
        Value::Array(items) => items.iter().for_each(|n| collect_offers(n, inherited_sku, out)),
        Value::Object(map) => {
            if let Some(graph) = map.get("@graph") {
                collect_offers(graph, inherited_sku, out);
            }
            let sku = map.get("sku").and_then(Value::as_str).or(inherited_sku);
            if let Some(offers) = map.get("offers") {
                collect_offers(offers, sku, out);
            }
            if let Some(has) = map.get("hasVariant") {
                collect_offers(has, None, out);
            }
            let is_offer = map.get("@type").is_some_and(|t| t.to_string().contains("Offer"));
            if is_offer {
                if let Some(mut offer) = offer_from(node) {
                    offer.sku = offer.sku.or_else(|| sku.map(str::to_string));
                    out.push(offer);
                }
            }
        }
        _ => {}
    }
}

/// Каскад (от надёжного к грубому): 1) JSON-состояние страницы (`__NEXT_DATA__`, `application/json`),
/// 2) schema.org JSON-LD, 3) разметка `meta`/`itemprop`. Следующий шаг работает, только если предыдущий ничего не нашёл.
pub fn parse_offers(html: &str) -> Vec<Offer> {
    let document = Html::parse_document(html);
    for step in [from_json_state, from_json_ld, from_meta] {
        let offers = step(&document);
        if !offers.is_empty() {
            return offers;
        }
    }
    Vec::new()
}

fn scripts(document: &Html, css: &str) -> Vec<Value> {
    let Ok(selector) = Selector::parse(css) else { return Vec::new() };
    document.select(&selector).filter_map(|s| serde_json::from_str::<Value>(s.text().collect::<String>().trim()).ok()).collect()
}

fn from_json_ld(document: &Html) -> Vec<Offer> {
    let mut offers = Vec::new();
    for value in scripts(document, r#"script[type="application/ld+json"]"#) {
        collect_offers(&value, None, &mut offers);
    }
    offers
}

/// Ищет в произвольном JSON ключи `offers` с ценой (так отдают данные Next.js и подобные сайты).
fn collect_state_offers(node: &Value, out: &mut Vec<Offer>, depth: usize) {
    if depth > 24 {
        return;
    }
    match node {
        Value::Array(items) => items.iter().for_each(|n| collect_state_offers(n, out, depth + 1)),
        Value::Object(map) => {
            if let Some(offers) = map.get("offers") {
                let list: Vec<&Value> = match offers {
                    Value::Array(items) => items.iter().collect(),
                    other => vec![other],
                };
                out.extend(list.into_iter().filter(|n| n.is_object()).filter_map(offer_from));
            }
            map.iter().filter(|(k, _)| k.as_str() != "offers").for_each(|(_, v)| collect_state_offers(v, out, depth + 1));
        }
        _ => {}
    }
}

fn from_json_state(document: &Html) -> Vec<Offer> {
    let mut offers = Vec::new();
    for value in scripts(document, r#"script#__NEXT_DATA__, script[type="application/json"]"#) {
        collect_state_offers(&value, &mut offers, 0);
    }
    offers
}

fn attr_or_text(document: &Html, css: &str) -> Option<String> {
    let selector = Selector::parse(css).ok()?;
    let el = document.select(&selector).next()?;
    let v = el.value();
    let text = v.attr("content").or_else(|| v.attr("href")).map(str::to_string).unwrap_or_else(|| el.text().collect::<String>());
    Some(text.trim().to_string()).filter(|t| !t.is_empty())
}

fn from_meta(document: &Html) -> Vec<Offer> {
    let price = ["meta[property=\"product:price:amount\"]", "meta[property=\"og:price:amount\"]", "[itemprop=\"price\"]"]
        .iter()
        .find_map(|css| attr_or_text(document, css))
        .and_then(|t| parse_price_minor(&t));
    let Some(price_minor) = price else { return Vec::new() };
    let availability = ["meta[property=\"product:availability\"]", "[itemprop=\"availability\"]"].iter().find_map(|css| attr_or_text(document, css)).unwrap_or_default().to_ascii_lowercase();
    let sold_out = availability.contains("outofstock") || availability.contains("out of stock") || availability.contains("oos") || availability.contains("soldout");
    vec![Offer { sku: None, price_minor, stock: if sold_out { 0 } else { NOMINAL_STOCK } }]
}

#[cfg(test)]
mod tests {
    use super::*;

    fn page(json: &str) -> String {
        format!(r#"<html><head><script type="application/ld+json">{json}</script></head><body>x</body></html>"#)
    }

    #[test]
    fn prices_are_parsed_from_human_formats_without_floats() {
        let cases = [("1299", 129_900), ("1 299,50 ₽", 129_950), ("1,299.50", 129_950), ("1.299,5", 129_950), ("1\u{a0}299", 129_900), ("0,99", 99), ("12.345", 1_234_500), ("19.9", 1_990)];
        for (text, want) in cases {
            assert_eq!(parse_price_minor(text), Some(want), "{text}");
        }
        for bad in ["", "abc", "0", "0,00", "99999999999999999999"] {
            assert_eq!(parse_price_minor(bad), None, "{bad}");
        }
    }

    #[test]
    fn single_offer_with_availability() {
        let html = page(r#"{"@type":"Product","sku":"A1","offers":{"@type":"Offer","price":"1500.00","availability":"https://schema.org/InStock"}}"#);
        assert_eq!(parse_offers(&html), vec![Offer { sku: Some("A1".into()), price_minor: 150_000, stock: 99 }]);
    }

    #[test]
    fn out_of_stock_and_inventory_level() {
        let out = page(r#"{"@type":"Product","offers":{"@type":"Offer","price":100,"availability":"http://schema.org/OutOfStock"}}"#);
        assert_eq!(parse_offers(&out)[0].stock, 0);
        let level = page(r#"{"@type":"Product","offers":{"@type":"Offer","price":100,"inventoryLevel":{"value":7}}}"#);
        assert_eq!(parse_offers(&level)[0].stock, 7);
    }

    #[test]
    fn graph_and_offer_arrays_with_per_variant_sku() {
        let html = page(r#"{"@graph":[{"@type":"Product","offers":[{"@type":"Offer","sku":"S","price":"10"},{"@type":"Offer","sku":"M","price":"12","availability":"OutOfStock"}]}]}"#);
        let offers = parse_offers(&html);
        assert_eq!(offers.iter().map(|o| (o.sku.clone().unwrap(), o.price_minor, o.stock)).collect::<Vec<_>>(), vec![("S".into(), 1000, 99), ("M".into(), 1200, 0)]);
    }

    #[test]
    fn cascade_falls_back_to_json_state_then_meta_and_prefers_the_reliable_source() {
        let state = r#"<script id="__NEXT_DATA__" type="application/json">{"props":{"pageProps":{"product":{"offers":[{"sku":"S1","price":"2 490","availability":"OutOfStock"},{"sku":"S2","price":3100,"inventoryLevel":4}]}}}}</script>"#;
        let offers = parse_offers(state);
        assert_eq!(offers.iter().map(|o| (o.sku.clone().unwrap(), o.price_minor, o.stock)).collect::<Vec<_>>(), vec![("S1".into(), 249_000, 0), ("S2".into(), 310_000, 4)]);

        let meta = r#"<meta property="product:price:amount" content="1 990,00"><meta property="product:availability" content="out of stock">"#;
        assert_eq!(parse_offers(meta), vec![Offer { sku: None, price_minor: 199_000, stock: 0 }]);
        let micro = r#"<span itemprop="price" content="499.90"></span><link itemprop="availability" href="https://schema.org/InStock">"#;
        assert_eq!(parse_offers(micro), vec![Offer { sku: None, price_minor: 49_990, stock: 99 }]);

        let both = format!("{state}{}", page(r#"{"@type":"Product","offers":{"@type":"Offer","price":"1"}}"#));
        assert_eq!(parse_offers(&both).len(), 2, "JSON-состояние приоритетнее JSON-LD");
        assert!(parse_offers(r#"<meta property="product:price:amount" content="бесплатно">"#).is_empty());
    }

    #[test]
    fn broken_or_missing_markup_yields_nothing() {
        assert!(parse_offers("<html><body>нет разметки</body></html>").is_empty());
        assert!(parse_offers(&page("{not json")).is_empty());
        assert!(parse_offers(&page(r#"{"@type":"Product","offers":{"@type":"Offer","price":"бесплатно"}}"#)).is_empty());
    }
}
