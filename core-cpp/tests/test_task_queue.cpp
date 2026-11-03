#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#include "TestDb.hpp"

using namespace testdb;
using std::chrono::seconds;

namespace {

std::int64_t enqueue(DatabaseManager& db, TaskType type, int priority, UnixSeconds scheduledAt,
                     const std::string& payload = "{}", int maxAttempts = 3) {
    return unwrap(db.enqueueTask(type, priority, payload, scheduledAt, maxAttempts));
}

std::optional<Task> claim(DatabaseManager& db, UnixSeconds now, seconds lock = seconds(300),
                          std::span<const TaskType> types = {}) {
    return unwrap(db.claimNextTask(now, lock, types));
}

std::string statusOf(DatabaseManager& db, std::int64_t id) {
    return unwrap(db.query("SELECT status FROM task_queue WHERE id = ?", {id})).textAt(0, "status");
}

}  // namespace

TEST(queue_rejects_invalid_priority_and_attempts) {
    TempDir dir;
    auto db = openDb(dir);
    CHECK(!db->enqueueTask(TaskType::AiRewrite, 0, "{}", 0).has_value());
    CHECK(!db->enqueueTask(TaskType::AiRewrite, 11, "{}", 0).has_value());
    CHECK(!db->enqueueTask(TaskType::AiRewrite, 5, "{}", 0, 0).has_value());
    CHECK(db->enqueueTask(TaskType::AiRewrite, 10, "{}", 0, 1).has_value());
}

TEST(queue_claims_by_priority_then_schedule_then_id) {
    TempDir dir;
    auto db = openDb(dir);
    const auto low = enqueue(*db, TaskType::PriceSync, 5, 100);
    const auto lateHigh = enqueue(*db, TaskType::PriceSync, 1, 200);
    const auto earlyHigh = enqueue(*db, TaskType::PriceSync, 1, 100);

    CHECK_EQ(claim(*db, 500)->id, earlyHigh);
    CHECK_EQ(claim(*db, 500)->id, lateHigh);
    CHECK_EQ(claim(*db, 500)->id, low);
    CHECK(!claim(*db, 500).has_value());
}

TEST(queue_claim_returns_task_data_and_marks_it_in_progress) {
    TempDir dir;
    auto db = openDb(dir);
    const auto id = enqueue(*db, TaskType::AutoOrder, 2, 100, R"({"order":42})", 4);

    const auto task = claim(*db, 100);
    REQUIRE(task.has_value());
    CHECK_EQ(task->id, id);
    CHECK(task->type == TaskType::AutoOrder);
    CHECK_EQ(task->priority, 2);
    CHECK_EQ(task->payloadJson, std::string(R"({"order":42})"));
    CHECK_EQ(task->attempts, 1);
    CHECK_EQ(task->maxAttempts, 4);
    CHECK_EQ(statusOf(*db, id), std::string("IN_PROGRESS"));
}

TEST(queue_does_not_claim_future_tasks) {
    TempDir dir;
    auto db = openDb(dir);
    enqueue(*db, TaskType::PriceSync, 5, 1000);
    CHECK(!claim(*db, 999).has_value());
    CHECK(claim(*db, 1000).has_value());
}

TEST(queue_filters_by_task_type) {
    TempDir dir;
    auto db = openDb(dir);
    const auto ai = enqueue(*db, TaskType::AiRewrite, 1, 0);
    const auto order = enqueue(*db, TaskType::AutoOrder, 5, 0);

    const TaskType onlyOrders[] = {TaskType::AutoOrder};
    CHECK_EQ(claim(*db, 10, seconds(300), onlyOrders)->id, order);
    CHECK(!claim(*db, 10, seconds(300), onlyOrders).has_value());  // AI-задачу воркер заказов не трогает
    const TaskType onlyAi[] = {TaskType::AiRewrite};
    CHECK_EQ(claim(*db, 10, seconds(300), onlyAi)->id, ai);
}

TEST(queue_complete_is_idempotent) {
    TempDir dir;
    auto db = openDb(dir);
    const auto id = enqueue(*db, TaskType::PriceSync, 5, 0);
    CHECK(!unwrap(db->completeTask(id)));  // ещё не в работе
    claim(*db, 10);
    CHECK(unwrap(db->completeTask(id)));
    CHECK(!unwrap(db->completeTask(id)));  // повторный вызов ничего не меняет
    CHECK_EQ(statusOf(*db, id), std::string("COMPLETED"));
    CHECK(!claim(*db, 1000).has_value());
}

TEST(queue_failure_retries_with_delay_then_exhausts) {
    TempDir dir;
    auto db = openDb(dir);
    const auto id = enqueue(*db, TaskType::StorePublish, 5, 0, "{}", 2);

    claim(*db, 100);
    CHECK(unwrap(db->failTask(id, "network down", 100, seconds(60))) == FailOutcome::WillRetry);
    CHECK_EQ(statusOf(*db, id), std::string("PENDING"));
    CHECK(!claim(*db, 159).has_value());  // задержка ещё не прошла

    const auto second = claim(*db, 160);
    REQUIRE(second.has_value());
    CHECK_EQ(second->attempts, 2);
    CHECK(unwrap(db->failTask(id, "still down", 160)) == FailOutcome::Exhausted);
    CHECK_EQ(statusOf(*db, id), std::string("FAILED"));
    CHECK_EQ(unwrap(db->query("SELECT error_log FROM task_queue WHERE id = ?", {id})).textAt(0, "error_log"),
             std::string("still down"));
    CHECK(!claim(*db, 100000).has_value());
}

TEST(queue_fail_ignores_tasks_that_are_not_in_progress) {
    TempDir dir;
    auto db = openDb(dir);
    const auto id = enqueue(*db, TaskType::PriceSync, 5, 0);
    CHECK(unwrap(db->failTask(id, "x", 0)) == FailOutcome::NotInProgress);
    CHECK_EQ(statusOf(*db, id), std::string("PENDING"));
}

TEST(queue_reclaims_task_when_worker_died_holding_the_lock) {
    TempDir dir;
    auto db = openDb(dir);
    const auto id = enqueue(*db, TaskType::AiRewrite, 5, 0);

    CHECK_EQ(claim(*db, 1000, seconds(300))->attempts, 1);  // воркер «умер» после захвата
    CHECK(!claim(*db, 1299, seconds(300)).has_value());     // блокировка ещё действует
    const auto again = claim(*db, 1300, seconds(300));
    REQUIRE(again.has_value());
    CHECK_EQ(again->id, id);
    CHECK_EQ(again->attempts, 2);
}

TEST(queue_marks_abandoned_tasks_failed_when_attempts_are_exhausted) {
    TempDir dir;
    auto db = openDb(dir);
    const auto id = enqueue(*db, TaskType::AiRewrite, 5, 0, "{}", 1);

    claim(*db, 1000, seconds(300));
    CHECK(!claim(*db, 5000, seconds(300)).has_value());
    CHECK_EQ(statusOf(*db, id), std::string("FAILED"));
}

TEST(queue_hands_every_task_to_exactly_one_of_many_workers) {
    TempDir dir;
    auto db = openDb(dir);
    constexpr int kTasks = 200;
    for (int i = 0; i < kTasks; ++i) enqueue(*db, TaskType::PriceSync, 1 + i % 10, 0);

    std::mutex mu;
    std::vector<std::int64_t> claimed;
    std::atomic<int> errors{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < 8; ++w) {
        workers.emplace_back([&] {
            for (;;) {
                const auto r = db->claimNextTask(10);
                if (!r) {
                    ++errors;
                    return;
                }
                if (!r->has_value()) return;
                {
                    std::lock_guard lock(mu);
                    claimed.push_back((*r)->id);
                }
                if (!db->completeTask((*r)->id)) ++errors;
            }
        });
    }
    for (auto& t : workers) t.join();

    CHECK_EQ(errors.load(), 0);
    CHECK_EQ(static_cast<int>(claimed.size()), kTasks);
    const std::set<std::int64_t> unique(claimed.begin(), claimed.end());
    CHECK_EQ(static_cast<int>(unique.size()), kTasks);
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM task_queue WHERE status = 'COMPLETED'"), kTasks);
}
