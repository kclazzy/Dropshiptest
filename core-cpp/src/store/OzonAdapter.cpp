#include "store/OzonAdapter.hpp"

#include <map>

namespace dropship::store {

namespace {

using Kind = StoreError::Kind;

// Рубли с округлением вверх: цена никогда не опускается ниже рассчитанной.
std::int64_t wholeRubles(Cents cents) { return (cents + 99) / 100; }

std::string errorText(const Json& item) {
    std::string text;
    if (const auto it = item.find("errors"); it != item.end() && it->is_array()) {
        for (const auto& e : *it) {
            if (!text.empty()) text += "; ";
            text += e.value("code", std::string{}) + (e.contains("message") ? ": " + e.value("message", std::string{}) : "");
        }
    }
    return text.empty() ? "rejected by Ozon" : text;
}

}  // namespace

OzonAdapter::OzonAdapter(OzonConfig config, net::IHttpTransport& transport, TokenBucketRateLimiter& limiter,
                         std::function<double()> nowSeconds)
    : config_(std::move(config)), transport_(transport), limiter_(limiter), now_(std::move(nowSeconds)) {}

Result<Json> OzonAdapter::post(const std::string& path, const Json& body) {
    const double now = now_();
    if (!limiter_.tryAcquire(now)) {
        const int wait = static_cast<int>(limiter_.secondsUntilAvailable(now)) + 1;
        return unexpected(makeStoreError(Kind::RateLimited, "local rate limit", 0, wait));
    }
    net::HttpRequest request;
    request.method = net::HttpRequest::Method::Post;
    request.url = config_.baseUrl + path;
    request.body = dumpJson(body);
    request.headers = {{"Client-Id", config_.clientId}, {"Api-Key", config_.apiKey}};
    request.timeout = std::chrono::seconds(30);
    const auto response = transport_.send(request);
    if (!response) return unexpected(makeStoreError(Kind::Transport, "no connection to Ozon: " + response.error().message));
    const int status = response->status;
    if (status == 401 || status == 403) {
        return unexpected(makeStoreError(Kind::Auth, "Ozon rejected the Client-Id / Api-Key", status));
    }
    if (status == 429) return unexpected(makeStoreError(Kind::RateLimited, "Ozon rate limit", status, 60));
    if (status >= 500) return unexpected(makeStoreError(Kind::Transport, "Ozon server error", status));
    Json parsed = parseJson(response->body);
    if (status != 200) {
        std::string message = parsed.is_object() ? parsed.value("message", std::string{}) : std::string{};
        return unexpected(makeStoreError(Kind::Rejected, "Ozon refused the request" + (message.empty() ? "" : ": " + message), status));
    }
    if (parsed.is_discarded() || !parsed.is_object()) {
        return unexpected(makeStoreError(Kind::Transport, "Ozon returned an unreadable response", status));
    }
    return parsed;
}

Result<std::vector<ItemResult>> OzonAdapter::updateVariantPriceAndStock(std::span<const VariantUpdate> updates) {
    if (config_.clientId.empty() || config_.apiKey.empty()) {
        return unexpected(makeStoreError(Kind::Config, "Ozon: client_id and api_key are required"));
    }
    if (config_.warehouseId <= 0) return unexpected(makeStoreError(Kind::Config, "Ozon: warehouse_id is required"));

    std::vector<ItemResult> results;
    std::map<std::string, std::size_t> byOffer;  // offer_id -> индекс в results
    for (const auto& u : updates) {
        ItemResult item;
        item.variantId = u.variantId;
        if (u.externalId.empty()) item.error = "not linked: set external_variant_id to the Ozon offer_id";
        else if (byOffer.count(u.externalId)) item.error = "duplicate offer_id in one batch";
        else byOffer[u.externalId] = results.size();
        results.push_back(std::move(item));
    }

    // Остатки
    std::vector<std::size_t> stockIdx;
    for (std::size_t i = 0; i < updates.size(); ++i) {
        if (results[i].error.empty()) stockIdx.push_back(i);
    }
    for (std::size_t from = 0; from < stockIdx.size(); from += kStocksPerRequest) {
        Json stocks = Json::array();
        const auto to = std::min(stockIdx.size(), from + kStocksPerRequest);
        for (auto k = from; k < to; ++k) {
            const auto& u = updates[stockIdx[k]];
            stocks.push_back({{"offer_id", u.externalId}, {"stock", std::max<std::int64_t>(u.stock, 0)}, {"warehouse_id", config_.warehouseId}});
        }
        const auto reply = post("/v2/products/stocks", Json{{"stocks", stocks}});
        if (!reply) return unexpected(reply.error());
        for (const auto& row : (*reply).value("result", Json::array())) {
            const auto it = byOffer.find(row.value("offer_id", std::string{}));
            if (it == byOffer.end()) continue;
            if (row.value("updated", false)) results[it->second].stockSent = true;
            else results[it->second].error = "stock: " + errorText(row);
        }
    }

    // Цены (только рассчитанные: 0 означает «правило не применилось»)
    std::vector<std::size_t> priceIdx;
    for (std::size_t i = 0; i < updates.size(); ++i) {
        if (!results[i].error.empty() || !byOffer.count(updates[i].externalId)) continue;
        if (updates[i].retailCents <= 0) continue;
        priceIdx.push_back(i);
    }
    for (std::size_t from = 0; from < priceIdx.size(); from += kPricesPerRequest) {
        Json prices = Json::array();
        const auto to = std::min(priceIdx.size(), from + kPricesPerRequest);
        for (auto k = from; k < to; ++k) {
            const auto& u = updates[priceIdx[k]];
            prices.push_back({{"offer_id", u.externalId},
                              {"price", std::to_string(wholeRubles(u.retailCents))},
                              {"old_price", "0"},
                              {"currency_code", "RUB"},
                              {"auto_action_enabled", "UNKNOWN"}});
        }
        const auto reply = post("/v1/product/import/prices", Json{{"prices", prices}});
        if (!reply) return unexpected(reply.error());
        for (const auto& row : (*reply).value("result", Json::array())) {
            const auto it = byOffer.find(row.value("offer_id", std::string{}));
            if (it == byOffer.end()) continue;
            if (row.value("updated", false)) results[it->second].priceSent = true;
            else results[it->second].error += (results[it->second].error.empty() ? "" : "; ") + ("price: " + errorText(row));
        }
    }

    for (std::size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        if (r.error.empty() && !r.stockSent) r.error = "no confirmation from Ozon";
        r.ok = r.error.empty();
    }
    return results;
}

}  // namespace dropship::store
