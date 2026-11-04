#pragma once

// Avito API (https://api.avito.ru), OAuth2 client_credentials.
// Реализовано: обновление остатков (PUT /stock-management/1/stocks) и цен
// (POST /core/v1/items/{item_id}/update_price). item_id — числовой id объявления, записанный в
// product_variants.external_variant_id.
//
// ВНИМАНИЕ: пути, методы и тела запросов восстановлены по памяти — официальную документацию
// (developers.avito.ru) в среде разработки открыть не удалось, вторичные источники их не приводят
// («пути надёжны, HTTP-глаголы не всегда»). Остатки через API доступны только на тарифах/аккаунтах,
// где включён Stock Management. Адаптер НЕ проверен на живом аккаунте: проверьте на одном объявлении.

#include <functional>
#include <string>

#include "core/Json.hpp"
#include "net/HttpTransport.hpp"
#include "store/RateLimiter.hpp"
#include "store/StoreAdapter.hpp"

namespace dropship::store {

struct AvitoConfig {
    std::string clientId;
    std::string clientSecret;
    std::string baseUrl = "https://api.avito.ru";
};

class AvitoAdapter : public IStoreAdapter {
public:
    AvitoAdapter(AvitoConfig config, net::IHttpTransport& transport, TokenBucketRateLimiter& limiter,
                 std::function<double()> nowSeconds);

    std::string name() const override { return "avito"; }
    Result<std::vector<ItemResult>> updateVariantPriceAndStock(std::span<const VariantUpdate> updates) override;

private:
    Result<std::string> token();
    // Вызов с токеном; при 401 токен сбрасывается и запрос повторяется один раз.
    Result<Json> call(net::HttpRequest::Method method, const std::string& path, const Json& body);

    AvitoConfig config_;
    net::IHttpTransport& transport_;
    TokenBucketRateLimiter& limiter_;
    std::function<double()> now_;
    std::string accessToken_;
    double tokenValidUntil_ = 0;
};

}  // namespace dropship::store
