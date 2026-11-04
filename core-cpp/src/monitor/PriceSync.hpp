#pragma once

// Мониторинг поставщиков: берёт задачу PRICE_SYNC, скачивает страницу (с куками расширения, через прокси
// из пула, с учётом лимитов домена), разбирает и применяет состояние. Все сетевые исходы описаны явно:
// капча, пропавшая страница, сбой прокси, временная ошибка.

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "bridge/SupplierSessionStore.hpp"
#include "monitor/DomainLimiter.hpp"
#include "monitor/ProxyPool.hpp"
#include "monitor/StateApplier.hpp"
#include "net/HttpTransport.hpp"

namespace dropship::monitor {

struct MonitorConfig {
    std::chrono::seconds retryDelay{300};       // после временной ошибки
    std::chrono::seconds blockCooldown{1800};   // домен «отдыхает» после 403/429
    std::chrono::seconds proxyWait{60};         // все прокси в кулдауне / сбой прокси
    std::chrono::seconds scheduleEvery{15};     // как часто искать карточки, которым пора проверяться
    int maxRedirects = 3;
    std::chrono::milliseconds requestTimeout{20'000};
    std::string defaultUserAgent =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
};

struct MonitorContext {
    db::DatabaseManager& database;
    net::IHttpTransport& transport;
    const bridge::SupplierSessionStore& sessions;
    ProxyPool& proxies;
    DomainLimiter& limiter;
    MonitorConfig config{};
};

enum class SyncOutcome {
    NoTask,
    Updated,    // данные изменились и применены
    Unchanged,  // хэш состояния прежний
    Deferred,   // отложено без расхода попытки (лимит домена / прокси отдыхают)
    Blocked,    // 403/429 без прокси: нужна капча через расширение, карточка CAPTCHA_BLOCKED
    Retrying,   // временная ошибка, будет повтор
    Failed,     // попытки исчерпаны, карточка ERROR
    Gone,       // страница пропала (404/410), остаток 0, мониторинг на паузе
};

db::Result<SyncOutcome> processNextPriceSync(MonitorContext& context, UnixSeconds now);

using LogFn = std::function<void(const std::string&)>;

// Планировщик + N воркеров на std::jthread. Деструктор останавливает и дожидается всех.
class MonitorService {
public:
    MonitorService(MonitorContext context, std::function<UnixSeconds()> clock, int workers, LogFn log = {});
    ~MonitorService();
    MonitorService(const MonitorService&) = delete;
    MonitorService& operator=(const MonitorService&) = delete;

private:
    std::vector<std::jthread> threads_;
};

}  // namespace dropship::monitor
