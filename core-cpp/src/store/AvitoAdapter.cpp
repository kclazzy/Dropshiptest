#include "store/AvitoAdapter.hpp"

#include <charconv>
#include <map>

#include <curl/curl.h>

namespace dropship::store {

namespace {

using Kind = StoreError::Kind;
constexpr std::size_t kStocksPerRequest = 200;

std::optional<std::int64_t> numericId(const std::string& text) {
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || value <= 0) return std::nullopt;
    return value;
}

std::string urlEncode(const std::string& text) {
    char* escaped = curl_easy_escape(nullptr, text.c_str(), static_cast<int>(text.size()));
    std::string out = escaped ? escaped : "";
    curl_free(escaped);
    return out;
}

Result<Json> classify(const net::HttpResponse& response) {
    const int status = response.status;
    if (status == 401 || status == 403) return unexpected(makeStoreError(Kind::Auth, "Avito rejected the credentials or the token", status));
    if (status == 429) return unexpected(makeStoreError(Kind::RateLimited, "Avito rate limit", status, 60));
    if (status >= 500) return unexpected(makeStoreError(Kind::Transport, "Avito server error", status));
    Json parsed = parseJson(response.body);
    if (status < 200 || status >= 300) {
        return unexpected(makeStoreError(Kind::Rejected, "Avito refused the request (HTTP " + std::to_string(status) + ")", status));
    }
    if (parsed.is_discarded()) parsed = Json::object();  // успешный ответ без тела тоже допустим
    return parsed;
}

}  // namespace

AvitoAdapter::AvitoAdapter(AvitoConfig config, net::IHttpTransport& transport, TokenBucketRateLimiter& limiter,
                           std::function<double()> nowSeconds)
    : config_(std::move(config)), transport_(transport), limiter_(limiter), now_(std::move(nowSeconds)) {}

Result<std::string> AvitoAdapter::token() {
    const double now = now_();
    if (!accessToken_.empty() && now < tokenValidUntil_) return accessToken_;
    net::HttpRequest request;
    request.method = net::HttpRequest::Method::Post;
    request.url = config_.baseUrl + "/token";
    request.contentType = "application/x-www-form-urlencoded";
    request.body = "grant_type=client_credentials&client_id=" + urlEncode(config_.clientId) +
                   "&client_secret=" + urlEncode(config_.clientSecret);
    const auto response = transport_.send(request);
    if (!response) return unexpected(makeStoreError(Kind::Transport, "no connection to Avito: " + response.error().message));
    if (response->status == 400 || response->status == 401 || response->status == 403) {
        return unexpected(makeStoreError(Kind::Auth, "Avito did not accept client_id / client_secret", response->status));
    }
    if (response->status != 200) return unexpected(makeStoreError(Kind::Transport, "Avito token endpoint error", response->status));
    const Json parsed = parseJson(response->body);
    if (!parsed.is_object() || !parsed.contains("access_token") || !parsed["access_token"].is_string()) {
        return unexpected(makeStoreError(Kind::Transport, "Avito returned an unreadable token response", response->status));
    }
    accessToken_ = parsed["access_token"].get<std::string>();
    const double lifetime = parsed.value("expires_in", 3600.0);
    tokenValidUntil_ = now + std::max(lifetime - 60.0, 0.0);  // обновляем заранее
    return accessToken_;
}

Result<Json> AvitoAdapter::call(net::HttpRequest::Method method, const std::string& path, const Json& body) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto bearer = token();
        if (!bearer) return unexpected(bearer.error());
        const double now = now_();
        if (!limiter_.tryAcquire(now)) {
            return unexpected(makeStoreError(Kind::RateLimited, "local rate limit", 0, static_cast<int>(limiter_.secondsUntilAvailable(now)) + 1));
        }
        net::HttpRequest request;
        request.method = method;
        request.url = config_.baseUrl + path;
        request.body = dumpJson(body);
        request.headers = {{"Authorization", "Bearer " + *bearer}};
        request.timeout = std::chrono::seconds(30);
        const auto response = transport_.send(request);
        if (!response) return unexpected(makeStoreError(Kind::Transport, "no connection to Avito: " + response.error().message));
        if (response->status == 401 && attempt == 0) {
            accessToken_.clear();  // токен мог быть отозван — получаем новый и повторяем один раз
            continue;
        }
        return classify(*response);
    }
    return unexpected(makeStoreError(Kind::Auth, "Avito rejected the token twice", 401));
}

Result<std::vector<ItemResult>> AvitoAdapter::updateVariantPriceAndStock(std::span<const VariantUpdate> updates) {
    if (config_.clientId.empty() || config_.clientSecret.empty()) {
        return unexpected(makeStoreError(Kind::Config, "Avito: client_id and client_secret are required"));
    }
    std::vector<ItemResult> results(updates.size());
    std::map<std::int64_t, std::size_t> byItem;
    for (std::size_t i = 0; i < updates.size(); ++i) {
        results[i].variantId = updates[i].variantId;
        const auto id = numericId(updates[i].externalId);
        if (!id) results[i].error = "not linked: set external_variant_id to the numeric Avito item_id";
        else if (byItem.count(*id)) results[i].error = "duplicate item_id in one batch";
        else byItem[*id] = i;
    }

    std::vector<std::pair<std::int64_t, std::size_t>> items(byItem.begin(), byItem.end());
    for (std::size_t from = 0; from < items.size(); from += kStocksPerRequest) {
        Json stocks = Json::array();
        const auto to = std::min(items.size(), from + kStocksPerRequest);
        for (auto k = from; k < to; ++k) {
            stocks.push_back({{"item_id", items[k].first}, {"quantity", std::max<std::int64_t>(updates[items[k].second].stock, 0)}});
        }
        const auto reply = call(net::HttpRequest::Method::Put, "/stock-management/1/stocks", Json{{"stocks", stocks}});
        if (!reply) return unexpected(reply.error());
        for (const auto& row : (*reply).value("stocks", Json::array())) {
            if (!row.contains("item_id") || !row["item_id"].is_number_integer()) continue;
            const auto it = byItem.find(row["item_id"].get<std::int64_t>());
            if (it == byItem.end()) continue;
            if (row.value("success", false)) {
                results[it->second].stockSent = true;
            } else {
                std::string text;
                for (const auto& e : row.value("errors", Json::array())) text += (text.empty() ? "" : "; ") + (e.is_string() ? e.get<std::string>() : dumpJson(e));
                results[it->second].error = "stock: " + (text.empty() ? std::string("rejected by Avito") : text);
            }
        }
    }

    for (const auto& [itemId, index] : items) {
        if (!results[index].error.empty() || updates[index].retailCents <= 0) continue;
        const std::int64_t rubles = (updates[index].retailCents + 99) / 100;
        const auto reply = call(net::HttpRequest::Method::Post, "/core/v1/items/" + std::to_string(itemId) + "/update_price",
                                Json{{"price", rubles}});
        if (!reply) {
            if (reply.error().kind == Kind::Rejected) {
                results[index].error = "price: " + reply.error().message;
                continue;
            }
            return unexpected(reply.error());
        }
        results[index].priceSent = true;
    }

    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].error.empty() && !results[i].stockSent) results[i].error = "no confirmation from Avito";
        results[i].ok = results[i].error.empty();
    }
    return results;
}

}  // namespace dropship::store
