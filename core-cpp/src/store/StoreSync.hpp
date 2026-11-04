#pragma once

// Отправка цен и остатков на витрины. Источник работы — очередь STORE_PUBLISH; задачи создаёт
// scheduleStoreSync для карточек, у которых есть изменения, ещё не отправленные на площадку.
// «Что отправлено» хранится в product_variants.published_hash: повторная отправка того же состояния не делается.

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "db/DatabaseManager.hpp"
#include "net/HttpTransport.hpp"
#include "store/CredentialVault.hpp"
#include "store/RateLimiter.hpp"
#include "store/StoreAdapter.hpp"

namespace dropship::store {

// Откуда брать адаптер магазина (в тестах — заглушка).
class IAdapterProvider {
public:
    virtual ~IAdapterProvider() = default;
    virtual Result<IStoreAdapter*> adapterFor(std::int64_t storeId) = 0;
};

// Строит адаптеры по строкам таблицы stores: расшифровывает учётные данные, создаёт лимитер магазина.
// Не потокобезопасен для вызовов самих адаптеров — работать должен один поток синхронизации.
class StoreRegistry : public IAdapterProvider {
public:
    StoreRegistry(db::DatabaseManager& database, const CredentialVault& vault, net::IHttpTransport& transport,
                  std::function<double()> nowSeconds);
    Result<IStoreAdapter*> adapterFor(std::int64_t storeId) override;

private:
    struct Entry {
        std::vector<std::uint8_t> credentialsBlob;
        std::unique_ptr<TokenBucketRateLimiter> limiter;
        std::unique_ptr<IStoreAdapter> adapter;
    };
    db::DatabaseManager& database_;
    const CredentialVault& vault_;
    net::IHttpTransport& transport_;
    std::function<double()> now_;
    std::mutex mutex_;
    std::map<std::int64_t, Entry> entries_;
};

struct StoreSyncConfig {
    std::chrono::seconds retryDelay{300};
};

enum class StoreSyncOutcome {
    NoTask,
    Published,           // всё, что было связано, отправлено
    PartiallyPublished,  // часть вариантов площадка отклонила (причина в stores.last_error)
    UpToDate,            // отправлять нечего (нет изменений или нет привязанных вариантов)
    Skipped,             // магазин локальный/выключен
    Deferred,            // лимит запросов: вернёмся позже без расхода попытки
    Retrying,            // временная ошибка
    Failed,              // нужна помощь пользователя (ключ, настройки) или попытки кончились
};

db::Result<StoreSyncOutcome> processNextStorePublish(db::DatabaseManager& database, IAdapterProvider& provider,
                                                     UnixSeconds now, const StoreSyncConfig& config = {});

// Ставит STORE_PUBLISH для карточек с привязанными вариантами, изменившимися с прошлой отправки.
db::Result<int> scheduleStoreSync(db::DatabaseManager& database, UnixSeconds now, int limit = 100);

using LogFn = std::function<void(const std::string&)>;

// Один поток: планирование + отправка. Лимиты площадок всё равно не позволяют распараллелить.
class StoreSyncService {
public:
    StoreSyncService(db::DatabaseManager& database, IAdapterProvider& provider, std::function<UnixSeconds()> clock,
                     LogFn log = {});
    ~StoreSyncService();
    StoreSyncService(const StoreSyncService&) = delete;
    StoreSyncService& operator=(const StoreSyncService&) = delete;

private:
    std::jthread thread_;
};

}  // namespace dropship::store
