#include "store/StoreSync.hpp"

#include <condition_variable>

#include "core/Json.hpp"
#include "store/AvitoAdapter.hpp"
#include "store/OzonAdapter.hpp"

namespace dropship::store {

namespace {

using Kind = StoreError::Kind;
constexpr std::chrono::seconds kIdlePoll{2};

// Ozon: не более 80 запросов в минуту на аккаунт — потолок ниже, чем «по умолчанию» 2 запроса/с.
double effectiveRate(const std::string& platform, double configured) {
    const double rate = configured > 0 ? configured : 1.0;
    return platform == "ozon" ? std::min(rate, 1.3) : rate;
}

}  // namespace

StoreRegistry::StoreRegistry(db::DatabaseManager& database, const CredentialVault& vault, net::IHttpTransport& transport,
                             std::function<double()> nowSeconds)
    : database_(database), vault_(vault), transport_(transport), now_(std::move(nowSeconds)) {}

Result<IStoreAdapter*> StoreRegistry::adapterFor(std::int64_t storeId) {
    const auto row = database_.query("SELECT platform_type, encrypted_credentials, rate_limit_rps FROM stores WHERE id = ?", {storeId});
    if (!row) return unexpected(makeStoreError(Kind::Transport, "database: " + row.error().message));
    if (row->rows.empty()) return unexpected(makeStoreError(Kind::Config, "store not found"));
    const auto& blobValue = row->at(0, "encrypted_credentials");
    const auto* blob = std::get_if<db::Blob>(&blobValue);
    if (blob == nullptr || blob->empty()) {
        return unexpected(makeStoreError(Kind::Config, "store has no credentials: add them with the store-add command"));
    }

    std::lock_guard lock(mutex_);
    auto it = entries_.find(storeId);
    if (it != entries_.end() && it->second.credentialsBlob == *blob) return it->second.adapter.get();

    const auto plain = vault_.decrypt(*blob);
    if (!plain) return unexpected(makeStoreError(Kind::Config, "cannot decrypt store credentials: " + plain.error()));
    const Json creds = parseJson(*plain);
    if (!creds.is_object()) return unexpected(makeStoreError(Kind::Config, "store credentials are not a JSON object"));

    const std::string platform = row->textAt(0, "platform_type");
    const auto& rpsValue = row->at(0, "rate_limit_rps");
    const double configuredRps = std::holds_alternative<double>(rpsValue) ? std::get<double>(rpsValue) : 1.0;
    const double rate = effectiveRate(platform, configuredRps);

    Entry entry;
    entry.credentialsBlob = *blob;
    entry.limiter = std::make_unique<TokenBucketRateLimiter>(rate, std::max(1.0, rate * 2));
    if (platform == "ozon") {
        OzonConfig cfg;
        cfg.clientId = creds.value("client_id", std::string{});
        cfg.apiKey = creds.value("api_key", std::string{});
        if (const auto url = creds.value("base_url", std::string{}); url.rfind("http", 0) == 0) cfg.baseUrl = url;  // тесты, прокси-шлюзы
        cfg.warehouseId = creds.contains("warehouse_id") && creds["warehouse_id"].is_number_integer() ? creds["warehouse_id"].get<std::int64_t>() : 0;
        entry.adapter = std::make_unique<OzonAdapter>(std::move(cfg), transport_, *entry.limiter, now_);
    } else if (platform == "avito") {
        AvitoConfig cfg;
        cfg.clientId = creds.value("client_id", std::string{});
        cfg.clientSecret = creds.value("client_secret", std::string{});
        if (const auto url = creds.value("base_url", std::string{}); url.rfind("http", 0) == 0) cfg.baseUrl = url;
        entry.adapter = std::make_unique<AvitoAdapter>(std::move(cfg), transport_, *entry.limiter, now_);
    } else {
        return unexpected(makeStoreError(Kind::Unsupported, "no adapter for platform '" + platform + "' yet"));
    }
    IStoreAdapter* raw = entry.adapter.get();
    entries_[storeId] = std::move(entry);
    return raw;
}

db::Result<int> scheduleStoreSync(db::DatabaseManager& database, UnixSeconds now, int limit) {
    int queued = 0;
    const auto done = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        // Состояние ещё не отправлено и не было отклонено площадкой; магазин настоящий и включён.
        const auto due = tx.query(
            "SELECT DISTINCT p.id FROM products p "
            "JOIN stores s ON s.id = p.store_id AND s.is_active = 1 AND s.platform_type != 'local' "
            "JOIN product_variants v ON v.product_id = p.id "
            "WHERE v.external_variant_id IS NOT NULL AND v.external_variant_id != '' "
            "  AND (v.published_hash IS NULL OR v.published_hash != v.state_hash) "
            "  AND (v.rejected_hash IS NULL OR v.rejected_hash != v.state_hash) "
            "  AND NOT EXISTS (SELECT 1 FROM task_queue t WHERE t.task_type = 'STORE_PUBLISH' "
            "                  AND t.status IN ('PENDING', 'IN_PROGRESS') "
            "                  AND json_extract(t.payload_json, '$.product_id') = p.id) "
            "ORDER BY p.id LIMIT ?",
            {std::int64_t{limit}});
        if (!due) return unexpected(due.error());
        for (std::size_t i = 0; i < due->rows.size(); ++i) {
            const auto r = db::DatabaseManager::enqueueTaskIn(tx, TaskType::StorePublish, 4,
                                                              dumpJson(Json{{"product_id", due->intAt(i, "id")}}), now, 3);
            if (!r) return unexpected(r.error());
            ++queued;
        }
        return {};
    });
    if (!done) return unexpected(done.error());
    return queued;
}

db::Result<StoreSyncOutcome> processNextStorePublish(db::DatabaseManager& database, IAdapterProvider& provider,
                                                     UnixSeconds now, const StoreSyncConfig& config) {
    const TaskType types[] = {TaskType::StorePublish};
    const auto claimed = database.claimNextTask(now, std::chrono::minutes(10), types);
    if (!claimed) return unexpected(claimed.error());
    if (!*claimed) return StoreSyncOutcome::NoTask;
    const db::Task& task = **claimed;

    const auto fail = [&](const std::string& message, bool retry, std::optional<std::int64_t> storeId) -> db::Result<StoreSyncOutcome> {
        if (storeId) {
            const auto marked = database.execute("UPDATE stores SET last_error = ? WHERE id = ?", {message, *storeId});
            if (!marked) return unexpected(marked.error());
        }
        const auto outcome = database.failTask(task.id, message, now, config.retryDelay, retry);
        if (!outcome) return unexpected(outcome.error());
        return *outcome == db::FailOutcome::WillRetry ? StoreSyncOutcome::Retrying : StoreSyncOutcome::Failed;
    };
    const auto finish = [&](StoreSyncOutcome outcome) -> db::Result<StoreSyncOutcome> {
        const auto done = database.completeTask(task.id);
        if (!done) return unexpected(done.error());
        return outcome;
    };

    const Json payload = parseJson(task.payloadJson);
    const auto productId = payload.is_object() && payload.contains("product_id") ? asInt64(payload["product_id"]) : std::nullopt;
    if (!productId) return fail("invalid STORE_PUBLISH payload", false, std::nullopt);

    const auto store = database.query(
        "SELECT s.id, s.platform_type, s.is_active FROM products p JOIN stores s ON s.id = p.store_id WHERE p.id = ?", {*productId});
    if (!store) return unexpected(store.error());
    if (store->rows.empty()) return fail("product " + std::to_string(*productId) + " not found", false, std::nullopt);
    const std::int64_t storeId = store->intAt(0, "id");
    if (store->textAt(0, "platform_type") == "local" || store->intAt(0, "is_active") == 0) return finish(StoreSyncOutcome::Skipped);

    const auto rows = database.query(
        "SELECT id, external_variant_id, retail_price_cents, supplier_stock, is_in_stock, state_hash FROM product_variants "
        "WHERE product_id = ? AND external_variant_id IS NOT NULL AND external_variant_id != '' "
        "AND (published_hash IS NULL OR published_hash != state_hash) ORDER BY id",
        {*productId});
    if (!rows) return unexpected(rows.error());
    if (rows->rows.empty()) return finish(StoreSyncOutcome::UpToDate);

    std::vector<VariantUpdate> updates;
    std::map<std::int64_t, std::string> hashAtRead;
    for (std::size_t i = 0; i < rows->rows.size(); ++i) {
        VariantUpdate u;
        u.variantId = rows->intAt(i, "id");
        u.externalId = rows->textAt(i, "external_variant_id");
        u.retailCents = rows->intAt(i, "retail_price_cents");
        u.stock = rows->intAt(i, "is_in_stock") != 0 ? rows->intAt(i, "supplier_stock") : 0;  // защита остатков
        hashAtRead[u.variantId] = rows->textAt(i, "state_hash");
        updates.push_back(std::move(u));
    }

    const auto adapter = provider.adapterFor(storeId);
    if (!adapter) {
        const auto& e = adapter.error();
        return fail(e.message, e.retryable(), storeId);
    }
    const auto results = (*adapter)->updateVariantPriceAndStock(updates);
    if (!results) {
        const auto& e = results.error();
        if (e.kind == Kind::RateLimited) {
            const auto deferred = database.deferTask(task.id, now + std::max(e.retryAfterSeconds, 1));
            if (!deferred) return unexpected(deferred.error());
            return StoreSyncOutcome::Deferred;
        }
        return fail(e.message, e.retryable(), storeId);
    }

    std::string problems;
    const auto saved = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        for (const auto& item : *results) {
            // Условие по state_hash: если цена изменилась, пока шёл запрос, вариант останется «неотправленным».
            const auto r = item.ok
                ? tx.execute("UPDATE product_variants SET published_hash = state_hash, rejected_hash = NULL WHERE id = ? AND state_hash = ?",
                             {item.variantId, hashAtRead[item.variantId]})
                : tx.execute("UPDATE product_variants SET rejected_hash = ? WHERE id = ? AND state_hash = ?",
                             {hashAtRead[item.variantId], item.variantId, hashAtRead[item.variantId]});
            if (!r) return unexpected(r.error());
            if (!item.ok) problems += "variant " + std::to_string(item.variantId) + ": " + item.error + "\n";
        }
        const auto marked = tx.execute("UPDATE stores SET last_error = ? WHERE id = ?",
                                       {problems.empty() ? db::Value{std::monostate{}} : db::Value{problems}, storeId});
        if (!marked) return unexpected(marked.error());
        return {};
    });
    if (!saved) return unexpected(saved.error());
    return finish(problems.empty() ? StoreSyncOutcome::Published : StoreSyncOutcome::PartiallyPublished);
}

StoreSyncService::StoreSyncService(db::DatabaseManager& database, IAdapterProvider& provider,
                                   std::function<UnixSeconds()> clock, LogFn log) {
    thread_ = std::jthread([&database, &provider, clock = std::move(clock), log = std::move(log)](std::stop_token stop) {
        const auto say = [&](const std::string& text) { if (log) log(text); };
        std::mutex mutex;
        std::condition_variable_any wake;
        while (!stop.stop_requested()) {
            const auto queued = scheduleStoreSync(database, clock());
            if (!queued) say("store sync: scheduler error: " + queued.error().message);
            const auto outcome = processNextStorePublish(database, provider, clock());
            if (!outcome) {
                say("store sync: database error: " + outcome.error().message);
            } else if (*outcome != StoreSyncOutcome::NoTask) {
                switch (*outcome) {
                    case StoreSyncOutcome::Published: say("store sync: prices and stock published"); break;
                    case StoreSyncOutcome::PartiallyPublished: say("store sync: some items were rejected (see stores.last_error)"); break;
                    case StoreSyncOutcome::Failed: say("store sync: failed (see stores.last_error)"); break;
                    case StoreSyncOutcome::Retrying: say("store sync: temporary error, will retry"); break;
                    default: break;
                }
                continue;
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, kIdlePoll, [] { return false; });
        }
    });
}

StoreSyncService::~StoreSyncService() = default;

}  // namespace dropship::store
