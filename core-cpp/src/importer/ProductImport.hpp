#pragma once

// Приём товара от расширения (IMPORT_PRODUCT_PAYLOAD): проверка данных на входе
// и атомарная запись в БД. Цена продажи здесь не считается — retail_price_cents = 0
// означает «ещё не оценён»; расчёт делает PricingEngine при первой проверке товара.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "bridge/JsonRpc.hpp"
#include "core/Domain.hpp"
#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "db/DatabaseManager.hpp"

namespace dropship::importer {

struct ImportVariant {
    std::string sku;
    std::map<std::string, std::string> attributes;
    Cents priceCents = 0;     // закупка
    Cents shippingCents = 0;
    std::int64_t stock = 0;   // «в наличии без количества» расширение отправляет как допущение, не как 0
};

struct ImportPayload {
    std::string supplierUrl;  // уже нормализован
    std::string supplierProductId;
    std::string title;
    std::string description;
    std::vector<std::string> imageUrls;
    std::vector<ImportVariant> variants;
    std::optional<std::int64_t> storeId;  // не задан — первый активный магазин
};

struct ImportOutcome {
    std::int64_t productId = 0;
    bool duplicate = false;
    std::size_t variantCount = 0;
};

template <class T>
using Result = Expected<T, bridge::RpcError>;

// Хэш состояния варианта (закупка|доставка|остаток): если не изменился, обновлять нечего.
// Мониторинг цен должен использовать ту же функцию, чтобы хэши были сравнимы.
std::string variantStateHash(Cents priceCents, Cents shippingCents, std::int64_t stock);

Result<ImportPayload> parseImportPayload(const Json& params);

// Один и тот же товар (по SHA-256 нормализованной ссылки) второй раз не создаётся.
// Товар, варианты и задача AI_REWRITE записываются одной транзакцией.
Result<ImportOutcome> importProduct(db::DatabaseManager& db, const ImportPayload& payload, UnixSeconds now);

}  // namespace dropship::importer
