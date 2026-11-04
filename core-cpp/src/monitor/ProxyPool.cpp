#include "monitor/ProxyPool.hpp"

#include <algorithm>

namespace dropship::monitor {

ProxyPool::ProxyPool(db::DatabaseManager& database, ProxyPoolConfig config) : database_(database), config_(config) {}

db::Result<ProxyAcquire> ProxyPool::acquire(UnixSeconds now) {
    ProxyAcquire result;
    const auto done = database_.transaction([&](db::Tx& tx) -> db::Result<void> {
        // Захват одним UPDATE ... RETURNING: два воркера не получат один прокси в один момент.
        const auto taken = tx.query(
            "UPDATE proxies SET last_used_at = ? WHERE id = (SELECT id FROM proxies "
            "WHERE is_active = 1 AND cooldown_until <= ? ORDER BY last_used_at ASC, id ASC LIMIT 1) "
            "RETURNING id, proxy_url",
            {now, now});
        if (!taken) return unexpected(taken.error());
        if (!taken->rows.empty()) {
            result.availability = ProxyAvailability::Leased;
            result.lease = {taken->intAt(0, "id"), taken->textAt(0, "proxy_url")};
            return {};
        }
        const auto active = tx.query("SELECT COUNT(*) AS n FROM proxies WHERE is_active = 1");
        if (!active) return unexpected(active.error());
        result.availability = active->intAt(0, "n") > 0 ? ProxyAvailability::AllCoolingDown : ProxyAvailability::NoneConfigured;
        return {};
    });
    if (!done) return unexpected(done.error());
    return result;
}

db::Result<void> ProxyPool::reportSuccess(std::int64_t proxyId) {
    const auto r = database_.execute("UPDATE proxies SET fail_count = 0, cooldown_until = 0 WHERE id = ?", {proxyId});
    if (!r) return unexpected(r.error());
    return {};
}

db::Result<void> ProxyPool::reportFailure(std::int64_t proxyId, UnixSeconds now) {
    return database_.transaction([&](db::Tx& tx) -> db::Result<void> {
        const auto row = tx.query("SELECT fail_count FROM proxies WHERE id = ?", {proxyId});
        if (!row) return unexpected(row.error());
        if (row->rows.empty()) return {};
        const std::int64_t failures = row->intAt(0, "fail_count") + 1;
        // Кулдаун base * 2^(failures-1), но не больше максимума; сдвиг ограничен, чтобы не переполниться.
        const std::int64_t factor = std::int64_t{1} << std::min<std::int64_t>(failures - 1, 20);
        const std::int64_t cooldown = std::min<std::int64_t>(config_.baseCooldown.count() * factor, config_.maxCooldown.count());
        const auto r = tx.execute(
            "UPDATE proxies SET fail_count = ?, cooldown_until = ?, is_active = CASE WHEN ? >= ? THEN 0 ELSE is_active END "
            "WHERE id = ?",
            {failures, now + cooldown, failures, std::int64_t{config_.maxFailures}, proxyId});
        if (!r) return unexpected(r.error());
        return {};
    });
}

}  // namespace dropship::monitor
