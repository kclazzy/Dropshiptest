#include "monitor/PriceSync.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>

#include "core/Json.hpp"

namespace dropship::monitor {

namespace {

constexpr std::chrono::seconds kIdlePoll{1};

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Хост из http(s)-адреса; пустая строка для остальных схем и мусора.
std::string hostOf(const std::string& url) {
    std::size_t start;
    if (url.rfind("https://", 0) == 0) start = 8;
    else if (url.rfind("http://", 0) == 0) start = 7;
    else return {};
    const auto end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority.erase(0, at + 1);
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        return close == std::string::npos ? std::string{} : lowerAscii(authority.substr(0, close + 1));
    }
    if (const auto colon = authority.find(':'); colon != std::string::npos) authority.erase(colon);
    return lowerAscii(authority);
}

struct Fetched {
    std::optional<net::HttpResponse> response;
    std::string error;  // непусто, если ответа нет
};

Fetched fetch(MonitorContext& ctx, const std::string& startUrl, const std::string& proxyUrl) {
    const std::string originHost = hostOf(startUrl);
    const auto session = ctx.sessions.lookup(originHost);
    std::string url = startUrl;
    for (int hop = 0; hop <= ctx.config.maxRedirects; ++hop) {
        const std::string host = hostOf(url);
        if (host.empty()) return {std::nullopt, "unsupported redirect target"};
        net::HttpRequest request;
        request.url = url;
        request.proxy = proxyUrl;
        request.timeout = ctx.config.requestTimeout;
        request.headers = {{"User-Agent", session && !session->userAgent.empty() ? session->userAgent : ctx.config.defaultUserAgent},
                           {"Accept", "text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.5"},
                           {"Accept-Language", "ru,en;q=0.8"}};
        // Куки сессии принадлежат поставщику: на чужой хост после редиректа они не уходят.
        if (session && !session->cookieHeader.empty() && host == originHost) {
            request.headers.emplace_back("Cookie", session->cookieHeader);
        }
        auto response = ctx.transport.send(request);
        if (!response) return {std::nullopt, response.error().message};
        if (response->status >= 300 && response->status < 400 && !response->redirectUrl.empty()) {
            url = response->redirectUrl;
            continue;
        }
        return {std::move(*response), {}};
    }
    return {std::nullopt, "too many redirects"};
}

}  // namespace

db::Result<SyncOutcome> processNextPriceSync(MonitorContext& ctx, UnixSeconds now) {
    auto& database = ctx.database;
    const TaskType types[] = {TaskType::PriceSync};
    const auto claimed = database.claimNextTask(now, std::chrono::minutes(5), types);
    if (!claimed) return unexpected(claimed.error());
    if (!*claimed) return SyncOutcome::NoTask;
    const db::Task& task = **claimed;

    const auto fail = [&](std::string message, bool retry, std::optional<std::int64_t> productId) -> db::Result<SyncOutcome> {
        const auto outcome = database.failTask(task.id, message, now, ctx.config.retryDelay, retry);
        if (!outcome) return unexpected(outcome.error());
        if (*outcome == db::FailOutcome::WillRetry) return SyncOutcome::Retrying;
        if (productId) {
            const auto marked = database.execute("UPDATE products SET monitor_status = 'ERROR' WHERE id = ?", {*productId});
            if (!marked) return unexpected(marked.error());
        }
        return SyncOutcome::Failed;
    };
    const auto defer = [&](UnixSeconds until) -> db::Result<SyncOutcome> {
        const auto r = database.deferTask(task.id, until);
        if (!r) return unexpected(r.error());
        return SyncOutcome::Deferred;
    };

    const Json payload = parseJson(task.payloadJson);
    const auto id = payload.is_object() && payload.contains("product_id") ? asInt64(payload["product_id"]) : std::nullopt;
    if (!id) return fail("invalid PRICE_SYNC payload", false, std::nullopt);
    const std::int64_t productId = *id;

    const auto product = database.query("SELECT supplier_url, monitor_status FROM products WHERE id = ?", {productId});
    if (!product) return unexpected(product.error());
    if (product->rows.empty()) return fail("product " + std::to_string(productId) + " not found", false, std::nullopt);
    if (product->textAt(0, "monitor_status") != "ACTIVE") {
        const auto done = database.completeTask(task.id);  // на паузе/в ошибке — делать нечего
        if (!done) return unexpected(done.error());
        return SyncOutcome::Unchanged;
    }
    const std::string url = product->textAt(0, "supplier_url");
    const std::string host = hostOf(url);
    if (host.empty()) return fail("supplier_url is not an http(s) address", false, productId);

    if (!ctx.limiter.tryAcquire(host, now)) return defer(std::max(ctx.limiter.availableAt(host), now + 1));

    const auto lease = ctx.proxies.acquire(now);
    if (!lease) return unexpected(lease.error());
    if (lease->availability == ProxyAvailability::AllCoolingDown) {
        return defer(now + ctx.config.proxyWait.count());  // напрямую не идём: раскроем свой адрес
    }
    const bool viaProxy = lease->availability == ProxyAvailability::Leased;
    const std::string proxyUrl = viaProxy ? lease->lease.url : std::string{};
    const auto proxyFailed = [&]() -> db::Result<void> {
        return viaProxy ? ctx.proxies.reportFailure(lease->lease.id, now) : db::Result<void>{};
    };

    const Fetched fetched = fetch(ctx, url, proxyUrl);
    if (!fetched.response) {
        if (auto r = proxyFailed(); !r) return unexpected(r.error());
        return fail(fetched.error, true, productId);
    }
    const net::HttpResponse& response = *fetched.response;

    if (response.status == 403 || response.status == 429) {
        if (viaProxy) {
            if (auto r = proxyFailed(); !r) return unexpected(r.error());
            return defer(now + ctx.config.proxyWait.count());  // следующая попытка пойдёт через другой прокси
        }
        ctx.limiter.blockUntil(host, now + ctx.config.blockCooldown.count());
        const auto blocked = database.execute("UPDATE products SET monitor_status = 'CAPTCHA_BLOCKED', last_checked_at = ? WHERE id = ?",
                                              {now, productId});
        if (!blocked) return unexpected(blocked.error());
        const auto done = database.completeTask(task.id);
        if (!done) return unexpected(done.error());
        return SyncOutcome::Blocked;
    }

    if (response.status == 404 || response.status == 410) {
        if (viaProxy) {
            if (auto r = ctx.proxies.reportSuccess(lease->lease.id); !r) return unexpected(r.error());
        }
        if (auto r = markProductUnavailable(database, productId, now); !r) return unexpected(r.error());
        const auto done = database.completeTask(task.id);
        if (!done) return unexpected(done.error());
        return SyncOutcome::Gone;
    }

    if (response.status != 200) {
        return fail("supplier answered HTTP " + std::to_string(response.status), true, productId);
    }
    if (viaProxy) {
        if (auto r = ctx.proxies.reportSuccess(lease->lease.id); !r) return unexpected(r.error());
    }
    const auto parsed = parseProductPage(response.body);
    if (!parsed) return fail(parsed.error(), true, productId);

    const auto applied = applyObservation(database, productId, *parsed, now);
    if (!applied) return unexpected(applied.error());
    const auto done = database.completeTask(task.id);
    if (!done) return unexpected(done.error());
    return applied->changed + applied->added > 0 ? SyncOutcome::Updated : SyncOutcome::Unchanged;
}

MonitorService::MonitorService(MonitorContext context, std::function<UnixSeconds()> clock, int workers, LogFn log) {
    struct Shared {
        MonitorContext context;
        std::function<UnixSeconds()> clock;
        LogFn log;
        void say(const std::string& text) const { if (log) log(text); }
    };
    auto shared = std::make_shared<Shared>(Shared{std::move(context), std::move(clock), std::move(log)});

    threads_.emplace_back([shared](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        while (!stop.stop_requested()) {
            const auto queued = scheduleDueChecks(shared->context.database, shared->clock());
            if (!queued) shared->say("monitor: scheduler error: " + queued.error().message);
            else if (*queued > 0) shared->say("monitor: queued " + std::to_string(*queued) + " checks");
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::max(shared->context.config.scheduleEvery, std::chrono::seconds(1)), [] { return false; });
        }
    });
    for (int i = 0; i < std::max(workers, 1); ++i) {
        threads_.emplace_back([shared](std::stop_token stop) {
            std::mutex mutex;
            std::condition_variable_any wake;
            while (!stop.stop_requested()) {
                const auto outcome = processNextPriceSync(shared->context, shared->clock());
                if (outcome && *outcome != SyncOutcome::NoTask) continue;  // очередь могла не опустеть
                if (!outcome) shared->say("monitor: database error: " + outcome.error().message);
                std::unique_lock lock(mutex);
                wake.wait_for(lock, stop, kIdlePoll, [] { return false; });
            }
        });
    }
}

MonitorService::~MonitorService() = default;  // jthread: request_stop + join

}  // namespace dropship::monitor
