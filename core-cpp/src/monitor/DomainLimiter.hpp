#pragma once

// Вежливость к поставщику: не чаще одного запроса к домену раз в minInterval, плюс «блокировка»
// домена после 403/429 (капча/бан) на заданное время. Состояние в памяти — после перезапуска безопасно начинать заново.

#include <mutex>
#include <string>
#include <unordered_map>

#include "core/Domain.hpp"

namespace dropship::monitor {

class DomainLimiter {
public:
    explicit DomainLimiter(UnixSeconds minIntervalSeconds = 2) : minInterval_(minIntervalSeconds) {}

    // true — запрос разрешён и слот занят; false — подождите (см. retryAfter).
    bool tryAcquire(const std::string& host, UnixSeconds now);
    // Не раньше какого момента домен снова доступен (0 — уже доступен).
    UnixSeconds availableAt(const std::string& host) const;
    void blockUntil(const std::string& host, UnixSeconds until);
    void unblock(const std::string& host);  // сессия обновлена — домен снова доступен сразу

private:
    UnixSeconds minInterval_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, UnixSeconds> nextAllowed_;
};

}  // namespace dropship::monitor
