#pragma once

// Ограничитель частоты запросов к API площадки (token bucket). Время передаётся снаружи,
// поэтому поведение проверяется тестами без sleep. Потокобезопасен.

#include <mutex>

namespace dropship::store {

class TokenBucketRateLimiter {
public:
    // ratePerSecond — пополнение, burst — максимальный запас токенов (и начальный).
    TokenBucketRateLimiter(double ratePerSecond, double burst);

    // true — токен списан, запрос можно отправлять.
    bool tryAcquire(double nowSeconds, double tokens = 1.0);
    // Через сколько секунд появится нужное число токенов (0 — уже есть).
    double secondsUntilAvailable(double nowSeconds, double tokens = 1.0);

private:
    void refill(double nowSeconds);

    std::mutex mutex_;
    double rate_;
    double burst_;
    double tokens_;
    double last_ = 0.0;
    bool started_ = false;
};

}  // namespace dropship::store
