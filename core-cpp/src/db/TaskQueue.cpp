// Очередь задач поверх публичного API DatabaseManager.
// Захват задачи — один UPDATE ... RETURNING внутри транзакции писателя, поэтому
// две конкурирующие стороны никогда не получат одну и ту же задачу.

#include <string>

#include "db/Connection.hpp"
#include "db/DatabaseManager.hpp"

namespace dropship::db {

namespace {

constexpr int kMinPriority = 1;
constexpr int kMaxPriority = 10;

Result<Task> taskFromRow(const ResultSet& rs, std::size_t row) {
    const auto type = parseTaskType(rs.textAt(row, "task_type"));
    if (!type) return unexpected(makeError("unknown task type in queue: " + rs.textAt(row, "task_type")));
    Task task;
    task.id = rs.intAt(row, "id");
    task.type = *type;
    task.priority = static_cast<int>(rs.intAt(row, "priority"));
    task.payloadJson = rs.textAt(row, "payload_json");
    task.attempts = static_cast<int>(rs.intAt(row, "attempts"));
    task.maxAttempts = static_cast<int>(rs.intAt(row, "max_attempts"));
    task.scheduledAt = rs.intAt(row, "scheduled_at");
    return task;
}

// Значения берутся только из перечисления, поэтому подстановка в SQL безопасна.
std::string typeFilter(std::span<const TaskType> types) {
    if (types.empty()) return {};
    std::string filter = "AND task_type IN (";
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (i > 0) filter += ", ";
        filter += "'" + std::string(toString(types[i])) + "'";
    }
    return filter + ")";
}

}  // namespace

Result<std::int64_t> DatabaseManager::enqueueTaskIn(Tx& tx, TaskType type, int priority,
                                                    std::string_view payloadJson, UnixSeconds scheduledAt,
                                                    int maxAttempts) {
    if (priority < kMinPriority || priority > kMaxPriority) {
        return unexpected(makeError("task priority must be in 1..10"));
    }
    if (maxAttempts < 1) return unexpected(makeError("maxAttempts must be at least 1"));

    const auto inserted = tx.execute(
        "INSERT INTO task_queue(task_type, priority, payload_json, scheduled_at, max_attempts) "
        "VALUES(?, ?, ?, ?, ?)",
        {std::string(toString(type)), std::int64_t{priority}, std::string(payloadJson), scheduledAt,
         std::int64_t{maxAttempts}});
    if (!inserted) return unexpected(inserted.error());
    return inserted->lastInsertRowId;
}

Result<std::int64_t> DatabaseManager::enqueueTask(TaskType type, int priority, std::string_view payloadJson,
                                                  UnixSeconds scheduledAt, int maxAttempts) {
    std::int64_t id = 0;
    const auto done = transaction([&](Tx& tx) -> Result<void> {
        const auto queued = enqueueTaskIn(tx, type, priority, payloadJson, scheduledAt, maxAttempts);
        if (!queued) return unexpected(queued.error());
        id = *queued;
        return {};
    });
    if (!done) return unexpected(done.error());
    return id;
}

Result<std::optional<Task>> DatabaseManager::claimNextTask(UnixSeconds now, std::chrono::seconds lockTimeout,
                                                           std::span<const TaskType> types) {
    const UnixSeconds staleBefore = now - static_cast<std::int64_t>(lockTimeout.count());
    const std::string claimSql =
        "UPDATE task_queue SET status = 'IN_PROGRESS', locked_at = ?, attempts = attempts + 1 "
        "WHERE id = (SELECT id FROM task_queue "
        "            WHERE attempts < max_attempts "
        "              AND ((status = 'PENDING' AND scheduled_at <= ?) "
        "                OR (status = 'IN_PROGRESS' AND locked_at <= ?)) " +
        typeFilter(types) +
        "            ORDER BY priority ASC, scheduled_at ASC, id ASC LIMIT 1) "
        "RETURNING id, task_type, priority, payload_json, attempts, max_attempts, scheduled_at";

    std::optional<Task> claimed;
    const auto done = transaction([&](Tx& tx) -> Result<void> {
        // Брошенные воркером задачи без оставшихся попыток не должны висеть в IN_PROGRESS вечно.
        const auto reaped = tx.execute(
            "UPDATE task_queue SET status = 'FAILED', "
            "error_log = COALESCE(error_log || char(10), '') || 'lock expired after last attempt' "
            "WHERE status = 'IN_PROGRESS' AND attempts >= max_attempts AND locked_at <= ?",
            {staleBefore});
        if (!reaped) return unexpected(reaped.error());

        const auto rows = tx.query(claimSql, {now, now, staleBefore});
        if (!rows) return unexpected(rows.error());
        if (rows->rows.empty()) return {};

        auto task = taskFromRow(*rows, 0);
        if (!task) return unexpected(task.error());  // откат: задача остаётся нетронутой
        claimed = std::move(*task);
        return {};
    });
    if (!done) return unexpected(done.error());
    return claimed;
}

Result<bool> DatabaseManager::completeTask(std::int64_t taskId) {
    const auto updated = execute(
        "UPDATE task_queue SET status = 'COMPLETED', locked_at = 0 WHERE id = ? AND status = 'IN_PROGRESS'",
        {taskId});
    if (!updated) return unexpected(updated.error());
    return updated->changes > 0;
}

Result<FailOutcome> DatabaseManager::failTask(std::int64_t taskId, std::string_view errorMessage, UnixSeconds now,
                                              std::chrono::seconds retryDelay) {
    const UnixSeconds retryAt = now + static_cast<std::int64_t>(retryDelay.count());
    FailOutcome outcome = FailOutcome::NotInProgress;

    const auto done = transaction([&](Tx& tx) -> Result<void> {
        const auto rows = tx.query(
            "UPDATE task_queue SET "
            "status = CASE WHEN attempts >= max_attempts THEN 'FAILED' ELSE 'PENDING' END, "
            "scheduled_at = CASE WHEN attempts >= max_attempts THEN scheduled_at ELSE ? END, "
            "locked_at = 0, error_log = ? "
            "WHERE id = ? AND status = 'IN_PROGRESS' RETURNING status",
            {retryAt, std::string(errorMessage), taskId});
        if (!rows) return unexpected(rows.error());
        if (rows->rows.empty()) return {};
        outcome = rows->textAt(0, "status") == "FAILED" ? FailOutcome::Exhausted : FailOutcome::WillRetry;
        return {};
    });
    if (!done) return unexpected(done.error());
    return outcome;
}

}  // namespace dropship::db
