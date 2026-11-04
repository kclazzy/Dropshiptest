#include "monitor/PageParser.hpp"

#include <algorithm>
#include <cctype>

#include "core/Json.hpp"

namespace dropship::monitor {

namespace {

constexpr std::size_t kMaxJsonLdBlocks = 20;
constexpr std::size_t kMaxJsonLdBytes = 1 << 20;
constexpr int kMaxNodeDepth = 32;

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Содержимое всех <script type="application/ld+json"> (без обработки вложенных тегов — их там нет).
std::vector<std::string_view> jsonLdBlocks(std::string_view html, const std::string& lowered) {
    std::vector<std::string_view> blocks;
    std::size_t pos = 0;
    while (blocks.size() < kMaxJsonLdBlocks) {
        const auto open = lowered.find("<script", pos);
        if (open == std::string::npos) break;
        const auto tagEnd = lowered.find('>', open);
        if (tagEnd == std::string::npos) break;
        const auto closeTag = lowered.find("</script", tagEnd);
        if (closeTag == std::string::npos) break;
        const std::string_view tag(lowered.data() + open, tagEnd - open);
        if (tag.find("application/ld+json") != std::string_view::npos && closeTag - tagEnd - 1 <= kMaxJsonLdBytes) {
            blocks.push_back(html.substr(tagEnd + 1, closeTag - tagEnd - 1));
        }
        pos = closeTag + 1;
    }
    return blocks;
}

bool hasType(const Json& node, std::string_view type) {
    const auto it = node.find("@type");
    if (it == node.end()) return false;
    const auto matches = [&](const Json& t) {
        if (!t.is_string()) return false;
        const auto s = t.get<std::string>();
        return s == type || (s.size() > type.size() && s.compare(s.size() - type.size(), type.size(), type) == 0 &&
                             s[s.size() - type.size() - 1] == '/');
    };
    if (it->is_array()) return std::any_of(it->begin(), it->end(), matches);
    return matches(*it);
}

void collectProducts(const Json& node, std::vector<const Json*>& out, int depth) {
    if (depth > kMaxNodeDepth) return;
    if (node.is_array()) {
        for (const auto& child : node) collectProducts(child, out, depth + 1);
    } else if (node.is_object()) {
        if (hasType(node, "Product")) out.push_back(&node);
        if (const auto g = node.find("@graph"); g != node.end()) collectProducts(*g, out, depth + 1);
    }
}

std::string textOf(const Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    return {};
}

std::optional<Cents> priceOf(const Json& offer) {
    for (const char* key : {"price", "lowPrice"}) {
        const auto it = offer.find(key);
        if (it == offer.end()) continue;
        if (it->is_string()) {
            if (const auto p = parsePriceToCents(it->get<std::string>())) return p;
        } else if (it->is_number()) {
            const double v = it->get<double>();
            if (v >= 0 && v < 9e15) return static_cast<Cents>(v * 100 + 0.5);
        }
    }
    if (const auto spec = offer.find("priceSpecification"); spec != offer.end() && spec->is_object()) {
        return priceOf(*spec);
    }
    return std::nullopt;
}

std::int64_t stockOf(const Json& offer) {
    if (const auto level = offer.find("inventoryLevel"); level != offer.end()) {
        const Json& v = level->is_object() && level->contains("value") ? (*level)["value"] : *level;
        if (v.is_number_integer() && v.get<std::int64_t>() >= 0) return v.get<std::int64_t>();
        if (v.is_string()) {
            try {
                const auto n = std::stoll(v.get<std::string>());
                if (n >= 0) return n;
            } catch (...) {
            }
        }
    }
    const auto availability = lower(textOf(offer, "availability"));
    for (const char* bad : {"outofstock", "soldout", "discontinued"}) {
        if (availability.find(bad) != std::string::npos) return 0;
    }
    return kDefaultInStockQuantity;
}

void collectOffers(const Json& offers, std::vector<const Json*>& out, int depth) {
    if (depth > 4) return;
    if (offers.is_array()) {
        for (const auto& o : offers) collectOffers(o, out, depth + 1);
    } else if (offers.is_object()) {
        if (const auto nested = offers.find("offers"); nested != offers.end()) collectOffers(*nested, out, depth + 1);
        else out.push_back(&offers);
    }
}

// content="..." для <meta ... name|property|itemprop="<name>" ...>
std::string metaContent(std::string_view html, const std::string& lowered, std::string_view name) {
    std::size_t pos = 0;
    while ((pos = lowered.find("<meta", pos)) != std::string::npos) {
        const auto end = lowered.find('>', pos);
        if (end == std::string::npos) break;
        const std::string_view tag(lowered.data() + pos, end - pos);
        const auto hit = [&](std::string_view attr) {
            return tag.find(std::string(attr) + "=\"" + std::string(name) + "\"") != std::string_view::npos ||
                   tag.find(std::string(attr) + "='" + std::string(name) + "'") != std::string_view::npos;
        };
        if (hit("property") || hit("name") || hit("itemprop")) {
            const auto c = tag.find("content=");
            if (c != std::string_view::npos && c + 8 < tag.size()) {
                const char quote = tag[c + 8];
                if (quote == '"' || quote == '\'') {
                    const auto close = tag.find(quote, c + 9);
                    if (close != std::string_view::npos) return std::string(html.substr(pos + c + 9, close - c - 9));
                }
            }
        }
        pos = end;
    }
    return {};
}

}  // namespace

std::optional<Cents> parsePriceToCents(std::string_view raw) {
    std::string s;
    for (const char c : raw) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ',') s += c;
    }
    if (s.empty() || !std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::nullopt;
    }
    std::string whole = s, frac;
    const auto sep = s.find_last_of(".,");
    if (sep != std::string::npos) {
        const auto tail = s.substr(sep + 1);
        if (tail.size() >= 1 && tail.size() <= 2) {
            whole = s.substr(0, sep);
            frac = tail;
        }
    }
    whole.erase(std::remove_if(whole.begin(), whole.end(), [](char c) { return c == '.' || c == ','; }), whole.end());
    if (whole.empty()) whole = "0";
    if (whole.size() > 15) return std::nullopt;  // не помещается в int64 после умножения на 100
    while (frac.size() < 2) frac += '0';
    return std::stoll(whole) * 100 + std::stoll(frac);
}

Expected<std::vector<ObservedVariant>, std::string> parseProductPage(std::string_view html) {
    const std::string lowered = lower(html);
    std::vector<ObservedVariant> variants;

    for (const auto block : jsonLdBlocks(html, lowered)) {
        const Json parsed = parseJson(block);
        if (parsed.is_discarded()) continue;
        std::vector<const Json*> products;
        collectProducts(parsed, products, 0);
        for (const Json* product : products) {
            const auto offersIt = product->find("offers");
            if (offersIt == product->end()) continue;
            std::vector<const Json*> offers;
            collectOffers(*offersIt, offers, 0);
            for (const Json* offer : offers) {
                const auto price = priceOf(*offer);
                if (!price) continue;
                std::string sku = textOf(*offer, "sku");
                if (sku.empty()) sku = textOf(*product, "sku");
                variants.push_back({std::move(sku), *price, stockOf(*offer)});
            }
            if (!variants.empty()) return variants;
        }
    }

    for (const char* key : {"product:price:amount", "og:price:amount", "price"}) {
        if (const auto price = parsePriceToCents(metaContent(html, lowered, key))) {
            std::int64_t stock = kDefaultInStockQuantity;
            const auto availability = lower(metaContent(html, lowered, "product:availability"));
            if (availability.find("out of stock") != std::string::npos ||
                availability.find("oos") != std::string::npos || availability.find("outofstock") != std::string::npos) {
                stock = 0;
            }
            return std::vector<ObservedVariant>{{"", *price, stock}};
        }
    }
    return unexpected(std::string("price not found on the page"));
}

}  // namespace dropship::monitor
