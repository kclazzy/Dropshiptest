#include "store/RateLimiter.hpp"

#include <algorithm>

namespace dropship::store {

TokenBucketRateLimiter::TokenBucketRateLimiter(double ratePerSecond, double burst)
    : rate_(std::max(ratePerSecond, 1e-9)), burst_(std::max(burst, 1.0)), tokens_(std::max(burst, 1.0)) {}

void TokenBucketRateLimiter::refill(double now) {
    if (!started_) {
        started_ = true;
        last_ = now;
        return;
    }
    if (now > last_) {  // часы не идут назад: откат времени токенов не отнимает
        tokens_ = std::min(burst_, tokens_ + (now - last_) * rate_);
        last_ = now;
    }
}

bool TokenBucketRateLimiter::tryAcquire(double now, double tokens) {
    std::lock_guard lock(mutex_);
    refill(now);
    if (tokens > burst_ || tokens_ < tokens) return false;
    tokens_ -= tokens;
    return true;
}

double TokenBucketRateLimiter::secondsUntilAvailable(double now, double tokens) {
    std::lock_guard lock(mutex_);
    refill(now);
    if (tokens_ >= tokens) return 0.0;
    return (std::min(tokens, burst_) - tokens_) / rate_;
}

}  // namespace dropship::store
