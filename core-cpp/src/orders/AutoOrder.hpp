#pragma once

// Воркер AUTO_ORDER: отправляет расширению команду выкупить заказ у поставщика и принимает результат.
//
// Правило безопасности: покупку НЕЛЬЗЯ повторять автоматически, если неизвестно, началась ли она.
//  * до отправки заказ переводится NEW -> QUEUED_FOR_EXT условным UPDATE (двойной отправки нет);
//  * явный отказ расширения («не принято») возвращает заказ в NEW и задачу в очередь;
//  * таймаут или обрыв связи после отправки -> MANUAL_REVIEW, решает человек;
//  * расширение не подключено -> задача откладывается без расхода попытки.

#include <chrono>
#include <string>

#include "bridge/JsonRpc.hpp"
#include "db/DatabaseManager.hpp"

namespace dropship::orders {

class IExtensionLink {
public:
    virtual ~IExtensionLink() = default;
    virtual bool connected() const = 0;
    // Блокирует до ответа/таймаута. Ошибка — RpcError (в т.ч. kTimeout, kExtensionNotConnected).
    virtual Expected<Json, bridge::RpcError> call(const std::string& method, const Json& params,
                                                  std::chrono::milliseconds timeout) = 0;
};

enum class OrderTaskOutcome {
    NoTask,
    Sent,       // команда принята расширением, ждём ORDER_FULFILLMENT_RESULT
    Skipped,    // заказ уже не в статусе NEW (обработан человеком/другой задачей)
    Deferred,   // расширение не подключено, попробуем позже
    Rejected,   // расширение отказалось, заказ вернётся в очередь
    Review,     // нужен человек (невыгодно, исход неизвестен или попытки закончились)
    Failed,     // задача повреждена
};

db::Result<OrderTaskOutcome> processNextOrderTask(db::DatabaseManager& database, IExtensionLink& link, UnixSeconds now,
                                                  std::chrono::seconds retryDelay = std::chrono::seconds(60),
                                                  std::chrono::milliseconds callTimeout = std::chrono::seconds(15));

enum class FulfillmentStatus { Ordered, Failed, NeedsManual };

struct Fulfillment {
    std::int64_t orderId = 0;
    FulfillmentStatus status = FulfillmentStatus::Ordered;
    std::string supplierOrderId;  // обязателен для Ordered
    std::string message;
};

// Идемпотентно. true — состояние заказа изменилось. Ошибка — неизвестный заказ или некорректные данные.
db::Result<bool> applyFulfillment(db::DatabaseManager& database, const Fulfillment& result, UnixSeconds now);

}  // namespace dropship::orders
