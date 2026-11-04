#include "monitor/DomainLimiter.hpp"

#include <algorithm>

namespace dropship::monitor {

bool DomainLimiter::tryAcquire(const std::string& host, UnixSeconds now) {
    std::lock_guard lock(mutex_);
    auto& next = nextAllowed_[host];
    if (now < next) return false;
    next = now + minInterval_;
    return true;
}

UnixSeconds DomainLimiter::availableAt(const std::string& host) const {
    std::lock_guard lock(mutex_);
    const auto it = nextAllowed_.find(host);
    return it == nextAllowed_.end() ? 0 : it->second;
}

void DomainLimiter::blockUntil(const std::string& host, UnixSeconds until) {
    std::lock_guard lock(mutex_);
    auto& next = nextAllowed_[host];
    next = std::max(next, until);  // блокировка не сокращается более ранним вызовом
}

void DomainLimiter::unblock(const std::string& host) {
    std::lock_guard lock(mutex_);
    nextAllowed_.erase(host);
}

}  // namespace dropship::monitor
