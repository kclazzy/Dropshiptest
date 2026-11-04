#pragma once

// Пул прокси на таблице `proxies`: выдаёт наименее недавно использованный рабочий прокси,
// после ошибки отправляет его в кулдаун (растёт вдвое с каждой подряд ошибкой), а после
// maxFailures подряд — отключает (is_active = 0). Состояние в БД, поэтому переживает перезапуск.

#include <chrono>
#include <optional>
#include <string>

#include "db/DatabaseManager.hpp"

namespace dropship::monitor {

struct ProxyPoolConfig {
    int maxFailures = 5;
    std::chrono::seconds baseCooldown{60};
    std::chrono::seconds maxCooldown{3600};
};

struct ProxyLease {
    std::int64_t id = 0;
    std::string url;
};

enum class ProxyAvailability {
    NoneConfigured,  // прокси не заданы — запросы идут напрямую
    AllCoolingDown,  // прокси есть, но все отдыхают — напрямую идти нельзя (утечёт реальный IP)
    Leased,
};

struct ProxyAcquire {
    ProxyAvailability availability = ProxyAvailability::NoneConfigured;
    ProxyLease lease;
};

class ProxyPool {
public:
    explicit ProxyPool(db::DatabaseManager& database, ProxyPoolConfig config = {});

    db::Result<ProxyAcquire> acquire(UnixSeconds now);
    db::Result<void> reportSuccess(std::int64_t proxyId);
    db::Result<void> reportFailure(std::int64_t proxyId, UnixSeconds now);

private:
    db::DatabaseManager& database_;
    ProxyPoolConfig config_;
};

}  // namespace dropship::monitor
