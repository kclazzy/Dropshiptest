#pragma once

#include <functional>

#include "bridge/RpcRouter.hpp"
#include "bridge/SupplierSessionStore.hpp"
#include "core/Domain.hpp"
#include "db/DatabaseManager.hpp"

namespace dropship::bridge {

struct CoreServices {
    db::DatabaseManager& db;
    SupplierSessionStore& sessions;
    std::function<UnixSeconds()> clock;  // подменяется в тестах
    // Вызывается после успешного сохранения сессии домена (например, чтобы возобновить мониторинг).
    std::function<void(const std::string& domain)> onSessionUpdated;
};

// Методы расширение -> ядро:
//   PING                      -> {"pong":true,"time":<unix>}
//   IMPORT_PRODUCT_PAYLOAD    -> {"product_id","duplicate","variants"}
//   SYNC_SUPPLIER_COOKIES     -> {"stored":<число кук>}
//   ORDER_FULFILLMENT_RESULT  {order_id,status:ordered|failed|needs_manual,supplier_order_id?,message?} -> {"changed"}
//   ORDER_TRACKING_UPDATE     {order_id,tracking_number,carrier?} -> {"changed"}
//   LIST_ORDERS               {status?} -> {"orders":[...]} (адрес покупателя, ссылка и артикул поставщика)
// Объекты в `services` должны жить дольше router.
void registerCoreHandlers(RpcRouter& router, CoreServices services);

}  // namespace dropship::bridge
