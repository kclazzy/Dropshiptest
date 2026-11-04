#pragma once

// Единый интерфейс витрины (Ozon, Avito, ...). Бизнес-логика синхронизации знает только его.
// Операции, которые площадка/адаптер не поддерживает, возвращают Unsupported с понятным текстом.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/Domain.hpp"
#include "core/Expected.hpp"

namespace dropship::store {

struct StoreError {
    enum class Kind {
        Auth,         // неверный/просроченный ключ — повтор бессмыслен, нужен пользователь
        RateLimited,  // слишком часто — повторить позже (retryAfterSeconds)
        Rejected,     // площадка отклонила запрос целиком (неверные данные)
        Transport,    // сеть/5xx/непонятный ответ — повторить
        Unsupported,  // операция недоступна для этой площадки
        Config,       // не хватает настроек магазина (склад, id)
    };
    Kind kind = Kind::Transport;
    int httpStatus = 0;
    std::string message;
    int retryAfterSeconds = 0;

    bool retryable() const noexcept { return kind == Kind::Transport || kind == Kind::RateLimited; }
};

template <class T>
using Result = Expected<T, StoreError>;

inline StoreError makeStoreError(StoreError::Kind kind, std::string message, int httpStatus = 0, int retryAfter = 0) {
    return StoreError{kind, httpStatus, std::move(message), retryAfter};
}

// Что отправляем на витрину по одному варианту.
struct VariantUpdate {
    std::int64_t variantId = 0;
    std::string externalId;   // offer_id (Ozon) / item_id (Avito) — связь карточки с нашим вариантом
    Cents retailCents = 0;    // 0 = цена не рассчитана: цену не отправляем
    std::int64_t stock = 0;   // уже с защитой остатков (0, если ниже порога)
};

struct ItemResult {
    std::int64_t variantId = 0;
    bool ok = false;
    bool priceSent = false;
    bool stockSent = false;
    std::string error;
};

struct PublishRequest {
    std::string title;
    std::string htmlDescription;
    std::vector<std::string> images;
};

struct StoreOrder {
    std::string externalOrderId;
    std::string externalVariantId;
    std::int64_t quantity = 0;
    Cents totalCents = 0;
};

class IStoreAdapter {
public:
    virtual ~IStoreAdapter() = default;
    virtual std::string name() const = 0;

    // Результат по каждому варианту; ошибка всего вызова — если запрос не удался целиком.
    virtual Result<std::vector<ItemResult>> updateVariantPriceAndStock(std::span<const VariantUpdate> updates) = 0;

    virtual Result<std::string> publishProduct(const PublishRequest&) {
        return unexpected(makeStoreError(StoreError::Kind::Unsupported, name() + ": publishing is not supported yet"));
    }
    virtual Result<std::vector<StoreOrder>> fetchNewOrders(UnixSeconds) {
        return unexpected(makeStoreError(StoreError::Kind::Unsupported, name() + ": orders are not supported yet"));
    }
    virtual Result<void> submitTrackingNumber(const std::string&, const std::string&) {
        return unexpected(makeStoreError(StoreError::Kind::Unsupported, name() + ": tracking is not supported yet"));
    }
};

}  // namespace dropship::store
