#pragma once

// Ozon Seller API (https://api-seller.ozon.ru). Заголовки Client-Id / Api-Key.
// Реализовано: обновление остатков (POST /v2/products/stocks, до 100 за запрос) и цен
// (POST /v1/product/import/prices). Товар должен уже существовать в кабинете Ozon, а его offer_id —
// быть записан в product_variants.external_variant_id.
//
// ВНИМАНИЕ: адрес, заголовки, пути, лимиты остатков и форма запроса остатков подтверждены по
// вторичным источникам; форма тела цен и формат ответов — по памяти и НЕ сверялись с живым API.
// Проверьте на тестовом товаре, прежде чем включать синхронизацию для всего каталога.

#include <functional>
#include <string>

#include "core/Json.hpp"
#include "net/HttpTransport.hpp"
#include "store/RateLimiter.hpp"
#include "store/StoreAdapter.hpp"

namespace dropship::store {

struct OzonConfig {
    std::string clientId;
    std::string apiKey;
    std::int64_t warehouseId = 0;  // склад FBS, на который отправляем остатки
    std::string baseUrl = "https://api-seller.ozon.ru";
};

class OzonAdapter : public IStoreAdapter {
public:
    static constexpr std::size_t kStocksPerRequest = 100;
    static constexpr std::size_t kPricesPerRequest = 1000;

    OzonAdapter(OzonConfig config, net::IHttpTransport& transport, TokenBucketRateLimiter& limiter,
                std::function<double()> nowSeconds);

    std::string name() const override { return "ozon"; }
    Result<std::vector<ItemResult>> updateVariantPriceAndStock(std::span<const VariantUpdate> updates) override;

private:
    Result<Json> post(const std::string& path, const Json& body);

    OzonConfig config_;
    net::IHttpTransport& transport_;
    TokenBucketRateLimiter& limiter_;
    std::function<double()> now_;
};

}  // namespace dropship::store
