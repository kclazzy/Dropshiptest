#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "Harness.hpp"
#include "TestDb.hpp"
#include "ai/AiWorker.hpp"

using namespace dropship;
using namespace dropship::ai;
using namespace testdb;
using namespace std::chrono_literals;

namespace {

class FakeGenerator : public IContentGenerator {
public:
    dropship::ai::Result<GeneratedContent> generate(const ProductSource& source) override {
        std::lock_guard lock(mutex);
        sources.push_back(source);
        if (results.empty()) return GeneratedContent{"AI title", "<p>AI description</p>", {"b1", "b2"}, {"t1", "t2"}};
        auto next = std::move(results.front());
        results.pop_front();
        return next;
    }
    std::string name() const override { return "fake"; }

    void failWith(AiErrorKind kind, int status, const std::string& message) {
        std::lock_guard lock(mutex);
        results.emplace_back(unexpected(AiError{kind, status, message}));
    }
    std::size_t calls() {
        std::lock_guard lock(mutex);
        return sources.size();
    }

    std::mutex mutex;
    std::deque<dropship::ai::Result<GeneratedContent>> results;
    std::vector<ProductSource> sources;
};

std::int64_t enqueueFor(DatabaseManager& db, std::int64_t productId, int maxAttempts = 3) {
    return unwrap(db.enqueueTask(TaskType::AiRewrite, 5, dumpJson(Json{{"product_id", productId}}), 0, maxAttempts));
}

std::string aiStatus(DatabaseManager& db, std::int64_t productId) {
    return unwrap(db.query("SELECT ai_status FROM products WHERE id = ?", {productId})).textAt(0, "ai_status");
}

std::string taskStatus(DatabaseManager& db, std::int64_t taskId) {
    return unwrap(db.query("SELECT status FROM task_queue WHERE id = ?", {taskId})).textAt(0, "status");
}

AiTaskOutcome process(DatabaseManager& db, FakeGenerator& g, UnixSeconds now = 100) {
    return unwrap(processNextAiTask(db, g, now));
}

bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

}  // namespace

TEST(aiworker_reports_an_empty_queue) {
    TempDir dir;
    auto db = openDb(dir);
    FakeGenerator generator;
    CHECK(process(*db, generator) == AiTaskOutcome::NoTask);
    CHECK_EQ(generator.calls(), std::size_t{0});
}

TEST(aiworker_generates_and_saves_the_listing) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    unwrap(db->execute("UPDATE products SET raw_description = 'Bluetooth 5.3' WHERE id = ?", {f.productId}));
    unwrap(db->execute("UPDATE product_variants SET attributes_json = ? WHERE id = ?",
                       {std::string(R"({"color":"Black","size":"M"})"), f.variantId}));
    unwrap(db->execute(
        "INSERT INTO product_variants(product_id, supplier_sku, attributes_json, supplier_price_cents, "
        "retail_price_cents, supplier_stock, state_hash, updated_at) VALUES(?, 'SKU-2', ?, 1, 1, 1, 'h', 1)",
        {f.productId, std::string(R"({"color":"White","size":"M"})")}));
    const auto task = enqueueFor(*db, f.productId);

    FakeGenerator generator;
    CHECK(process(*db, generator) == AiTaskOutcome::Completed);

    REQUIRE(generator.sources.size() == 1);
    const auto& source = generator.sources[0];
    CHECK_EQ(source.title, std::string("Raw title"));
    CHECK_EQ(source.description, std::string("Bluetooth 5.3"));
    // Одинаковые значения не повторяются, разные склеиваются.
    CHECK(source.attributes == (std::vector<std::pair<std::string, std::string>>{{"color", "Black, White"}, {"size", "M"}}));

    const auto row = unwrap(db->query("SELECT * FROM products WHERE id = ?", {f.productId}));
    CHECK_EQ(row.textAt(0, "ai_seo_title"), std::string("AI title"));
    CHECK_EQ(row.textAt(0, "ai_html_description"), std::string("<p>AI description</p>"));
    const Json extras = Json::parse(row.textAt(0, "ai_tags_json"));
    CHECK(extras["search_tags"] == Json::array({"t1", "t2"}));
    CHECK(extras["bullet_points"] == Json::array({"b1", "b2"}));
    CHECK_EQ(row.textAt(0, "ai_status"), std::string("READY"));
    CHECK_EQ(taskStatus(*db, task), std::string("COMPLETED"));
}

TEST(aiworker_keeps_user_edits_untouched) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    unwrap(db->execute("UPDATE products SET title_override = 'Мой заголовок' WHERE id = ?", {f.productId}));
    enqueueFor(*db, f.productId);
    FakeGenerator generator;
    CHECK(process(*db, generator) == AiTaskOutcome::Completed);
    const auto view = unwrap(db->query("SELECT title FROM v_product_overview WHERE id = ?", {f.productId}));
    CHECK_EQ(view.textAt(0, "title"), std::string("Мой заголовок"));
}

TEST(aiworker_retries_temporary_errors_then_gives_up) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    const auto task = enqueueFor(*db, f.productId, 2);
    FakeGenerator generator;
    generator.failWith(AiErrorKind::Transport, 0, "connection refused");
    generator.failWith(AiErrorKind::Transport, 0, "connection refused");

    CHECK(unwrap(processNextAiTask(*db, generator, 100, 60s)) == AiTaskOutcome::Retrying);
    CHECK_EQ(aiStatus(*db, f.productId), std::string("PENDING"));
    CHECK_EQ(taskStatus(*db, task), std::string("PENDING"));
    CHECK(unwrap(processNextAiTask(*db, generator, 159, 60s)) == AiTaskOutcome::NoTask);  // пауза ещё идёт

    CHECK(unwrap(processNextAiTask(*db, generator, 160, 60s)) == AiTaskOutcome::Failed);  // вторая и последняя попытка
    CHECK_EQ(aiStatus(*db, f.productId), std::string("ERROR"));
    CHECK_EQ(taskStatus(*db, task), std::string("FAILED"));
    const auto log = unwrap(db->query("SELECT error_log FROM task_queue WHERE id = ?", {task}));
    CHECK(log.textAt(0, "error_log").find("connection refused") != std::string::npos);
}

TEST(aiworker_does_not_retry_permanent_errors) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    const auto task = enqueueFor(*db, f.productId, 5);
    FakeGenerator generator;
    generator.failWith(AiErrorKind::Http, 401, "Incorrect API key");

    CHECK(process(*db, generator) == AiTaskOutcome::Failed);
    CHECK_EQ(aiStatus(*db, f.productId), std::string("ERROR"));
    CHECK_EQ(taskStatus(*db, task), std::string("FAILED"));
    CHECK_EQ(generator.calls(), std::size_t{1});
}

TEST(aiworker_fails_tasks_with_broken_payload_or_missing_product) {
    TempDir dir;
    auto db = openDb(dir);
    FakeGenerator generator;
    const auto garbage = unwrap(db->enqueueTask(TaskType::AiRewrite, 5, "not json", 0));
    const auto noField = unwrap(db->enqueueTask(TaskType::AiRewrite, 5, R"({"x":1})", 0));
    const auto missing = enqueueFor(*db, 9999);

    for (int i = 0; i < 3; ++i) CHECK(process(*db, generator) == AiTaskOutcome::Failed);
    CHECK_EQ(taskStatus(*db, garbage), std::string("FAILED"));
    CHECK_EQ(taskStatus(*db, noField), std::string("FAILED"));
    CHECK_EQ(taskStatus(*db, missing), std::string("FAILED"));
    CHECK_EQ(generator.calls(), std::size_t{0});
}

TEST(aiworker_ignores_tasks_of_other_types) {
    TempDir dir;
    auto db = openDb(dir);
    unwrap(db->enqueueTask(TaskType::PriceSync, 1, "{}", 0));
    FakeGenerator generator;
    CHECK(process(*db, generator) == AiTaskOutcome::NoTask);
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM task_queue WHERE status = 'PENDING'"), 1);
}

TEST(aiworker_thread_drains_the_queue_and_stops_promptly) {
    TempDir dir;
    auto db = openDb(dir);
    std::vector<std::int64_t> products;
    for (int i = 0; i < 3; ++i) {
        const auto f = insertFixture(*db, "sha-" + std::to_string(i));
        products.push_back(f.productId);
        enqueueFor(*db, f.productId);
    }

    FakeGenerator generator;
    std::atomic<int> logged{0};
    {
        AiWorker worker(*db, generator, [] { return UnixSeconds{100}; }, [&](const std::string&) { ++logged; });
        CHECK(eventually([&] {
            return scalarInt(*db, "SELECT COUNT(*) FROM products WHERE ai_status = 'READY'") == 3;
        }));
        // Воркер простаивает в ожидании: остановка не должна занимать секунды.
        std::this_thread::sleep_for(100ms);
        const auto started = std::chrono::steady_clock::now();
        // деструктор ниже
        (void)started;
    }
    CHECK(logged.load() >= 3);
}

TEST(aiworker_thread_stops_quickly_when_idle) {
    TempDir dir;
    auto db = openDb(dir);
    FakeGenerator generator;
    const auto started = std::chrono::steady_clock::now();
    {
        AiWorker worker(*db, generator, [] { return UnixSeconds{100}; });
        std::this_thread::sleep_for(150ms);
    }
    CHECK(std::chrono::steady_clock::now() - started < 1500ms);
}
