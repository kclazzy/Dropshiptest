#pragma once

// Заказы покупателей: приём, проверка прибыли, статусы. Деньги — int64 в минимальных единицах.
//
// Статусы (orders.status):
//   NEW            принят, проверка пройдена, ожидает отправки в расширение (есть задача AUTO_ORDER)
//   QUEUED_FOR_EXT команда отправлена расширению, ждём результат выкупа
//   ORDERED        выкуплен у поставщика (есть supplier_order_id)
//   SHIPPED        известен трек-номер
//   MANUAL_REVIEW  нужен человек: нет прибыли/остатка/правила или исход выкупа неизвестен
//   ERROR          выкуп не удался окончательно
// Главный принцип: автоматически выкуп НИКОГДА не повторяется, если неизвестно, начался ли он.

#include <optional>
#include <string>

#include "db/DatabaseManager.hpp"

namespace dropship::orders {

inline constexpr std::size_t kMaxAddressBytes = 4096;
inline constexpr std::int64_t kMaxQuantity = 10'000;

struct NewOrder {
    std::int64_t storeId = 0;
    std::string externalOrderId;
    std::string externalVariantId;  // offer_id / item_id из витрины
    std::int64_t quantity = 1;
    Cents retailTotalCents = 0;     // сколько заплатил покупатель за все единицы
    std::string buyerAddressJson;   // {"name": ..., "phone": ..., "city": ..., ...} — как пришло с витрины
};

struct Evaluation {
    bool ok = false;
    std::string reason;  // пусто, если ok
    Cents costCents = 0;
    Cents netProfitCents = 0;
};

// Можно ли выкупать этот заказ по ТЕКУЩИМ данным поставщика. Вызывается и при приёме, и перед отправкой команды.
Evaluation evaluateOrder(db::Tx& tx, std::int64_t storeId, std::int64_t variantId, std::int64_t quantity,
                         Cents retailTotalCents);

enum class IngestStatus { Queued, NeedsReview, Duplicate };

struct IngestResult {
    std::int64_t orderId = 0;
    IngestStatus status = IngestStatus::Queued;
    std::string reason;
};

// Идемпотентно по (external_order_id): повторный приём того же заказа ничего не меняет.
db::Result<IngestResult> ingestOrder(db::DatabaseManager& database, const NewOrder& order, UnixSeconds now);

// Вернуть заказ из MANUAL_REVIEW/ERROR в очередь на повторную проверку и выкуп (решение человека).
db::Result<bool> retryOrder(db::DatabaseManager& database, std::int64_t orderId, UnixSeconds now);

}  // namespace dropship::orders
