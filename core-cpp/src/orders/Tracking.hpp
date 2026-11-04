#pragma once

// Трек-номера и опрос заказов магазинов.
//  * applyTracking: заказ -> SHIPPED, ставится задача TRACKING_SYNC (повтор того же номера ничего не делает);
//  * processNextTrackingSync: отдаёт номер площадке; если площадка не умеет — заказ помечается понятной
//    ошибкой «введите вручную», а не молча теряется;
//  * pollStoreOrders: забирает новые заказы у площадок, умеющих их отдавать.

#include <string>

#include "db/DatabaseManager.hpp"
#include "store/StoreSync.hpp"

namespace dropship::orders {

inline constexpr std::size_t kMaxTrackingLength = 100;

db::Result<bool> applyTracking(db::DatabaseManager& database, std::int64_t orderId, const std::string& trackingNumber,
                               const std::string& carrier, UnixSeconds now);

enum class TrackingOutcome { NoTask, Submitted, Skipped, Retrying, Failed };

db::Result<TrackingOutcome> processNextTrackingSync(db::DatabaseManager& database, store::IAdapterProvider& provider,
                                                    UnixSeconds now,
                                                    std::chrono::seconds retryDelay = std::chrono::seconds(300));

struct PollResult {
    int stores = 0;    // сколько магазинов опрошено успешно
    int ingested = 0;  // сколько новых заказов принято
};

// Ошибка одного магазина не мешает остальным (пишется в stores.last_error). Unsupported — не ошибка.
db::Result<PollResult> pollStoreOrders(db::DatabaseManager& database, store::IAdapterProvider& provider, UnixSeconds now);

}  // namespace dropship::orders
