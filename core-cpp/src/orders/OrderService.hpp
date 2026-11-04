#pragma once

// Фоновый поток заказов: AUTO_ORDER (команды расширению), TRACKING_SYNC (треки в магазины)
// и периодический опрос новых заказов площадок. Один поток: покупки идут строго по одной.

#include <functional>
#include <string>
#include <thread>

#include "orders/AutoOrder.hpp"
#include "store/StoreSync.hpp"

namespace dropship::orders {

class OrderService {
public:
    // provider может быть nullptr (нет ключа шифрования): тогда работают только покупки через расширение.
    OrderService(db::DatabaseManager& database, IExtensionLink& link, store::IAdapterProvider* provider,
                 std::function<UnixSeconds()> clock, std::function<void(const std::string&)> log = {},
                 std::chrono::seconds pollEvery = std::chrono::seconds(300));
    ~OrderService();
    OrderService(const OrderService&) = delete;
    OrderService& operator=(const OrderService&) = delete;

private:
    std::jthread thread_;
};

}  // namespace dropship::orders
