#include "ai/AiWorker.hpp"

#include <condition_variable>
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>

#include "core/Json.hpp"

namespace dropship::ai {

namespace {

using db::DbError;

constexpr std::chrono::seconds kIdlePoll{1};

// Значения одного атрибута у разных вариантов склеиваются без повторов: «Black, White».
std::vector<std::pair<std::string, std::string>> collectAttributes(const db::ResultSet& variants) {
    std::map<std::string, std::vector<std::string>> byName;
    for (std::size_t row = 0; row < variants.rows.size(); ++row) {
        if (variants.isNull(row, "attributes_json")) continue;
        const Json parsed = parseJson(variants.textAt(row, "attributes_json"));
        if (!parsed.is_object()) continue;
        for (const auto& [name, value] : parsed.items()) {
            const std::string text = value.is_string() ? value.get<std::string>() : value.dump();
            if (text.empty()) continue;
            auto& values = byName[name];
            if (std::find(values.begin(), values.end(), text) == values.end()) values.push_back(text);
        }
    }
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [name, values] : byName) {
        std::string joined;
        for (const auto& v : values) joined += (joined.empty() ? "" : ", ") + v;
        result.emplace_back(name, std::move(joined));
    }
    return result;
}

std::string describe(const AiError& error) {
    std::string text = error.message;
    if (error.httpStatus != 0) text = "HTTP " + std::to_string(error.httpStatus) + ": " + text;
    return text;
}

// Единственное место, где неудача задачи превращается в состояние очереди и товара.
db::Result<AiTaskOutcome> recordFailure(db::DatabaseManager& database, const db::Task& task,
                                        std::optional<std::int64_t> productId, const std::string& message,
                                        bool retryable, UnixSeconds now, std::chrono::seconds retryDelay) {
    const auto outcome = database.failTask(task.id, message, now, retryDelay, retryable);
    if (!outcome) return unexpected(outcome.error());
    const bool final = *outcome != db::FailOutcome::WillRetry;
    if (productId) {
        const auto updated = database.execute("UPDATE products SET ai_status = ? WHERE id = ?",
                                              {std::string(final ? "ERROR" : "PENDING"), *productId});
        if (!updated) return unexpected(updated.error());
    }
    return final ? AiTaskOutcome::Failed : AiTaskOutcome::Retrying;
}

}  // namespace

db::Result<AiTaskOutcome> processNextAiTask(db::DatabaseManager& database, IContentGenerator& generator, UnixSeconds now,
                                            std::chrono::seconds retryDelay) {
    const TaskType types[] = {TaskType::AiRewrite};
    const auto claimed = database.claimNextTask(now, std::chrono::minutes(10), types);
    if (!claimed) return unexpected(claimed.error());
    if (!*claimed) return AiTaskOutcome::NoTask;
    const db::Task& task = **claimed;

    const Json payload = parseJson(task.payloadJson);
    const auto id = payload.is_object() && payload.contains("product_id") ? asInt64(payload["product_id"])
                                                                          : std::nullopt;
    if (!id) {
        return recordFailure(database, task, std::nullopt, "invalid AI_REWRITE payload", false, now, retryDelay);
    }
    const std::int64_t productId = *id;

    const auto product = database.query("SELECT raw_title, raw_description FROM products WHERE id = ?", {productId});
    if (!product) return unexpected(product.error());
    if (product->rows.empty()) {
        return recordFailure(database, task, std::nullopt, "product " + std::to_string(productId) + " not found",
                             false, now, retryDelay);
    }
    const auto variants =
        database.query("SELECT attributes_json FROM product_variants WHERE product_id = ? ORDER BY id", {productId});
    if (!variants) return unexpected(variants.error());

    ProductSource source;
    source.title = product->textAt(0, "raw_title");
    if (!product->isNull(0, "raw_description")) source.description = product->textAt(0, "raw_description");
    source.attributes = collectAttributes(*variants);

    const auto marked = database.execute("UPDATE products SET ai_status = 'PROCESSING' WHERE id = ?", {productId});
    if (!marked) return unexpected(marked.error());

    // Сетевой вызов — вне транзакции: писатель БД не блокируется на время генерации.
    const auto generated = generator.generate(source);
    if (!generated) {
        return recordFailure(database, task, productId, describe(generated.error()), generated.error().retryable(),
                             now, retryDelay);
    }

    const Json extras = {{"bullet_points", generated->bulletPoints}, {"search_tags", generated->searchTags}};
    const auto done = database.transaction([&](db::Tx& tx) -> db::Result<void> {
        const auto saved = tx.execute(
            "UPDATE products SET ai_seo_title = ?, ai_html_description = ?, ai_tags_json = ?, ai_status = 'READY' "
            "WHERE id = ?",
            {generated->seoTitle, generated->htmlDescription, dumpJson(extras), productId});
        if (!saved) return unexpected(saved.error());
        const auto completed = tx.execute(
            "UPDATE task_queue SET status = 'COMPLETED', locked_at = 0 WHERE id = ? AND status = 'IN_PROGRESS'",
            {task.id});
        if (!completed) return unexpected(completed.error());
        return {};
    });
    if (!done) return unexpected(done.error());
    return AiTaskOutcome::Completed;
}

AiWorker::AiWorker(db::DatabaseManager& database, IContentGenerator& generator, std::function<UnixSeconds()> clock,
                   LogFn log) {
    thread_ = std::jthread([&database, &generator, clock = std::move(clock), log = std::move(log)](
                               std::stop_token stop) {
        const auto say = [&](const std::string& text) {
            if (log) log(text);
        };
        std::mutex mutex;
        std::condition_variable_any wake;
        while (!stop.stop_requested()) {
            const auto outcome = processNextAiTask(database, generator, clock());
            if (outcome && *outcome != AiTaskOutcome::NoTask) {
                switch (*outcome) {
                    case AiTaskOutcome::Completed: say("AI: listing generated by " + generator.name()); break;
                    case AiTaskOutcome::Retrying: say("AI: temporary failure, will retry"); break;
                    case AiTaskOutcome::Failed: say("AI: task failed permanently"); break;
                    case AiTaskOutcome::NoTask: break;
                }
                continue;  // очередь могла не опустеть
            }
            if (!outcome) say("AI: database error: " + outcome.error().message);
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, kIdlePoll, [] { return false; });
        }
    });
}

AiWorker::~AiWorker() = default;  // jthread: request_stop + join

}  // namespace dropship::ai
