#include "importer/ProductImport.hpp"

#include <algorithm>
#include <set>

#include "importer/UrlNormalizer.hpp"
#include "util/Sha256.hpp"

namespace dropship::importer {

namespace code = bridge::rpc_code;
using bridge::RpcError;

namespace {

constexpr std::size_t kMaxUrlBytes = 2048;
constexpr std::size_t kMaxTitleBytes = 1000;
constexpr std::size_t kMaxDescriptionBytes = 100'000;
constexpr std::size_t kMaxImages = 50;
constexpr std::size_t kMaxVariants = 200;
constexpr std::size_t kMaxSkuBytes = 200;
constexpr std::size_t kMaxAttributes = 50;
constexpr std::size_t kMaxAttributeBytes = 200;
constexpr std::int64_t kMaxMoneyCents = 1'000'000'000'000;  // 10 млрд в основной единице
constexpr std::int64_t kMaxStock = 1'000'000'000;

RpcError invalid(std::string message) { return RpcError{code::kInvalidParams, std::move(message), nullptr}; }

RpcError databaseFailure(const db::DbError& error) {
    return RpcError{code::kInternalError, "database error", Json{{"detail", error.message}}};
}

std::string trim(const std::string& text) {
    const auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end && isSpace(static_cast<unsigned char>(*begin))) ++begin;
    while (end != begin && isSpace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

std::string at(const std::string& path, const char* key) { return path.empty() ? key : path + "." + key; }

// Необязательная строка: отсутствует/null -> пустая.
Result<std::string> optionalString(const Json& object, const std::string& path, const char* key, std::size_t maxBytes) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return std::string();
    if (!it->is_string()) return unexpected(invalid(at(path, key) + " must be a string"));
    auto value = it->get<std::string>();
    if (value.size() > maxBytes) return unexpected(invalid(at(path, key) + " is too long"));
    return value;
}

Result<std::string> requiredString(const Json& object, const std::string& path, const char* key, std::size_t maxBytes) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return unexpected(invalid(at(path, key) + " is required"));
    auto value = optionalString(object, path, key, maxBytes);
    if (!value) return unexpected(value.error());
    return trim(*value);
}

Result<std::int64_t> boundedInt(const Json& object, const std::string& path, const char* key, std::int64_t min,
                                std::int64_t max, std::optional<std::int64_t> fallback) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        if (fallback) return *fallback;
        return unexpected(invalid(at(path, key) + " is required"));
    }
    const auto value = asInt64(*it);
    if (!value || *value < min || *value > max) {
        return unexpected(invalid(at(path, key) + " must be an integer in " + std::to_string(min) + ".." +
                                  std::to_string(max)));
    }
    return *value;
}

Result<std::map<std::string, std::string>> parseAttributes(const Json& variant, const std::string& path) {
    std::map<std::string, std::string> attributes;
    const auto it = variant.find("attributes");
    if (it == variant.end() || it->is_null()) return attributes;
    const std::string where = at(path, "attributes");
    if (!it->is_object() || it->size() > kMaxAttributes) {
        return unexpected(invalid(where + " must be an object with at most 50 entries"));
    }
    for (const auto& [key, value] : it->items()) {
        if (!value.is_string() || key.size() > kMaxAttributeBytes || value.get<std::string>().size() > kMaxAttributeBytes) {
            return unexpected(invalid(where + " must map short strings to short strings"));
        }
        attributes.emplace(key, value.get<std::string>());
    }
    return attributes;
}

Result<ImportVariant> parseVariant(const Json& raw, std::size_t index, std::set<std::string>& seenSkus) {
    const std::string path = "variants[" + std::to_string(index) + "]";
    if (!raw.is_object()) return unexpected(invalid(path + " must be an object"));

    ImportVariant variant;
    if (raw.contains("sku") && !raw["sku"].is_null()) {
        auto sku = requiredString(raw, path, "sku", kMaxSkuBytes);
        if (!sku) return unexpected(sku.error());
        if (sku->empty()) return unexpected(invalid(path + ".sku must not be empty"));
        variant.sku = std::move(*sku);
    } else {
        variant.sku = "default";
    }
    if (!seenSkus.insert(variant.sku).second) return unexpected(invalid(path + ".sku duplicates another variant"));

    auto price = boundedInt(raw, path, "price_cents", 0, kMaxMoneyCents, std::nullopt);
    if (!price) return unexpected(price.error());
    auto shipping = boundedInt(raw, path, "shipping_cents", 0, kMaxMoneyCents, 0);
    if (!shipping) return unexpected(shipping.error());
    auto stock = boundedInt(raw, path, "stock", 0, kMaxStock, std::nullopt);
    if (!stock) return unexpected(stock.error());
    auto attributes = parseAttributes(raw, path);
    if (!attributes) return unexpected(attributes.error());

    variant.priceCents = *price;
    variant.shippingCents = *shipping;
    variant.stock = *stock;
    variant.attributes = std::move(*attributes);
    return variant;
}

Result<std::vector<std::string>> parseImages(const Json& params) {
    std::vector<std::string> images;
    const auto it = params.find("images");
    if (it == params.end() || it->is_null()) return images;
    if (!it->is_array() || it->size() > kMaxImages) {
        return unexpected(invalid("images must be an array of at most 50 links"));
    }
    for (std::size_t i = 0; i < it->size(); ++i) {
        const Json& item = (*it)[i];
        const std::string where = "images[" + std::to_string(i) + "]";
        if (!item.is_string() || item.get<std::string>().size() > kMaxUrlBytes ||
            !normalizeUrl(item.get<std::string>())) {
            return unexpected(invalid(where + " must be an http(s) link"));
        }
        images.push_back(item.get<std::string>());  // оригинал: параметры CDN могут быть подписью
    }
    return images;
}

Result<std::vector<ImportVariant>> parseVariants(const Json& params) {
    const auto it = params.find("variants");
    if (it == params.end() || !it->is_array() || it->empty() || it->size() > kMaxVariants) {
        return unexpected(invalid("variants must be an array of 1..200 items"));
    }
    std::set<std::string> seenSkus;
    std::vector<ImportVariant> variants;
    for (std::size_t i = 0; i < it->size(); ++i) {
        auto variant = parseVariant((*it)[i], i, seenSkus);
        if (!variant) return unexpected(variant.error());
        variants.push_back(std::move(*variant));
    }
    return variants;
}

Result<ImportOutcome> failure(RpcError error) { return unexpected(std::move(error)); }

}  // namespace

std::string variantStateHash(Cents priceCents, Cents shippingCents, std::int64_t stock) {
    return sha256Hex(std::to_string(priceCents) + "|" + std::to_string(shippingCents) + "|" + std::to_string(stock));
}

Result<ImportPayload> parseImportPayload(const Json& params) {
    if (!params.is_object()) return unexpected(invalid("params must be an object"));

    ImportPayload payload;
    auto url = requiredString(params, "", "url", kMaxUrlBytes);
    if (!url) return unexpected(url.error());
    const auto normalized = normalizeUrl(*url);
    if (!normalized) return unexpected(invalid("url must be a valid http(s) link"));
    payload.supplierUrl = *normalized;

    auto title = requiredString(params, "", "title", kMaxTitleBytes);
    if (!title) return unexpected(title.error());
    if (title->empty()) return unexpected(invalid("title is required"));
    payload.title = std::move(*title);

    auto description = optionalString(params, "", "description", kMaxDescriptionBytes);
    if (!description) return unexpected(description.error());
    payload.description = std::move(*description);

    auto productId = optionalString(params, "", "supplier_product_id", kMaxSkuBytes);
    if (!productId) return unexpected(productId.error());
    payload.supplierProductId = trim(*productId);
    if (payload.supplierProductId.empty()) payload.supplierProductId = sha256Hex(payload.supplierUrl).substr(0, 16);

    auto images = parseImages(params);
    if (!images) return unexpected(images.error());
    payload.imageUrls = std::move(*images);

    auto variants = parseVariants(params);
    if (!variants) return unexpected(variants.error());
    payload.variants = std::move(*variants);

    if (params.contains("store_id") && !params["store_id"].is_null()) {
        auto storeId = boundedInt(params, "", "store_id", 1, std::numeric_limits<std::int64_t>::max(), std::nullopt);
        if (!storeId) return unexpected(storeId.error());
        payload.storeId = *storeId;
    }
    return payload;
}

Result<ImportOutcome> importProduct(db::DatabaseManager& database, const ImportPayload& payload, UnixSeconds now) {
    const std::string urlHash = sha256Hex(payload.supplierUrl);
    ImportOutcome outcome;
    std::optional<RpcError> businessError;

    const auto done = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        auto existing = tx.query("SELECT id, (SELECT COUNT(*) FROM product_variants WHERE product_id = products.id) AS n "
                                 "FROM products WHERE url_sha256 = ?",
                                 {urlHash});
        if (!existing) return unexpected(existing.error());
        if (!existing->rows.empty()) {
            outcome = {existing->intAt(0, "id"), true, static_cast<std::size_t>(existing->intAt(0, "n"))};
            return {};
        }

        // Магазин: явно указанный (должен быть активным) или первый активный.
        auto store = payload.storeId
                         ? tx.query("SELECT id FROM stores WHERE id = ? AND is_active = 1", {*payload.storeId})
                         : tx.query("SELECT id FROM stores WHERE is_active = 1 ORDER BY id LIMIT 1");
        if (!store) return unexpected(store.error());
        if (store->rows.empty()) {
            businessError = payload.storeId
                                ? RpcError{code::kStoreNotFound, "store not found or inactive", nullptr}
                                : RpcError{code::kNoStore, "no active store configured", nullptr};
            return {};  // до этого момента ничего не записано
        }

        auto product = tx.execute(
            "INSERT INTO products(store_id, supplier_url, supplier_product_id, url_sha256, raw_title, raw_description, "
            "images_json, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
            {store->intAt(0, "id"), payload.supplierUrl, payload.supplierProductId, urlHash, payload.title,
             payload.description, dumpJson(Json(payload.imageUrls)), now});
        if (!product) return unexpected(product.error());
        const std::int64_t productId = product->lastInsertRowId;

        for (const auto& variant : payload.variants) {
            auto inserted = tx.execute(
                "INSERT INTO product_variants(product_id, supplier_sku, attributes_json, supplier_price_cents, "
                "shipping_price_cents, retail_price_cents, supplier_stock, state_hash, updated_at) "
                "VALUES(?, ?, ?, ?, ?, 0, ?, ?, ?)",
                {productId, variant.sku, dumpJson(Json(variant.attributes)), variant.priceCents, variant.shippingCents,
                 variant.stock, variantStateHash(variant.priceCents, variant.shippingCents, variant.stock), now});
            if (!inserted) return unexpected(inserted.error());
        }

        auto queued = db::DatabaseManager::enqueueTaskIn(tx, TaskType::AiRewrite, 5,
                                                         dumpJson(Json{{"product_id", productId}}), now);
        if (!queued) return unexpected(queued.error());

        outcome = {productId, false, payload.variants.size()};
        return {};
    });

    if (!done) return failure(databaseFailure(done.error()));
    if (businessError) return failure(std::move(*businessError));
    return outcome;
}

}  // namespace dropship::importer
