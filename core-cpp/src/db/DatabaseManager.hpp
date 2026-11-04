#pragma once

// SQLite в режиме WAL: один сериализованный писатель + пул read-only соединений.
// Все запросы параметризованы (prepared statements); SQL со значениями внутри
// строки не принимается — значения передаются только через Params.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/Domain.hpp"
#include "core/Expected.hpp"

namespace dropship::db {

struct DbError {
    int sqliteCode = 0;  // расширенный код SQLite, 0 — ошибка уровня приложения
    std::string message;
};

template <class T>
using Result = Expected<T, DbError>;

using Blob = std::vector<std::uint8_t>;
// NULL | INTEGER | REAL | TEXT | BLOB
using Value = std::variant<std::monostate, std::int64_t, double, std::string, Blob>;
using Params = std::vector<Value>;

struct ExecResult {
    std::int64_t changes = 0;
    std::int64_t lastInsertRowId = 0;
};

struct ResultSet {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;

    // Доступ по имени колонки. Неверное имя колонки — ошибка программиста (исключение).
    const Value& at(std::size_t row, std::string_view column) const;
    std::int64_t intAt(std::size_t row, std::string_view column) const;
    const std::string& textAt(std::size_t row, std::string_view column) const;
    bool isNull(std::size_t row, std::string_view column) const;
};

class Connection;  // внутренняя обёртка над sqlite3*

// Доступ к соединению писателя внутри транзакции.
class Tx {
public:
    Result<ExecResult> execute(std::string_view sql, const Params& params = {});
    Result<ResultSet> query(std::string_view sql, const Params& params = {});

private:
    friend class DatabaseManager;
    explicit Tx(Connection& connection) : connection_(connection) {}
    Connection& connection_;
};

struct OpenOptions {
    std::size_t readerCount = 4;
    std::chrono::milliseconds busyTimeout{5000};
};

struct Task {
    std::int64_t id = 0;
    TaskType type = TaskType::AiRewrite;
    int priority = 5;
    std::string payloadJson;
    int attempts = 0;  // номер текущей попытки, начиная с 1
    int maxAttempts = 3;
    UnixSeconds scheduledAt = 0;
};

enum class FailOutcome {
    WillRetry,       // вернётся в PENDING после retryDelay
    Exhausted,       // попытки закончились — FAILED
    NotInProgress,   // задача не в работе (уже завершена/сброшена): ничего не изменено
};

class DatabaseManager {
public:
    // Создаёт каталог и файл БД при необходимости, включает WAL и накатывает миграции.
    static Result<std::unique_ptr<DatabaseManager>> open(const std::filesystem::path& file,
                                                         OpenOptions options = {});
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    // Запись — через единственное соединение писателя.
    Result<ExecResult> execute(std::string_view sql, const Params& params = {});
    // Чтение — через пул read-only соединений, параллельно с записью (WAL).
    Result<ResultSet> query(std::string_view sql, const Params& params = {});
    // BEGIN IMMEDIATE ... COMMIT; при ошибке или исключении — ROLLBACK.
    // Внутри колбэка использовать только Tx, а не методы DatabaseManager (взаимная блокировка).
    Result<void> transaction(const std::function<Result<void>(Tx&)>& body);

    Result<int> schemaVersion();

    // ---- Очередь задач (task_queue) ----

    Result<std::int64_t> enqueueTask(TaskType type, int priority, std::string_view payloadJson,
                                     UnixSeconds scheduledAt, int maxAttempts = 3);
    // То же внутри уже открытой транзакции (единственное место, где описана вставка в очередь).
    static Result<std::int64_t> enqueueTaskIn(Tx& tx, TaskType type, int priority, std::string_view payloadJson,
                                              UnixSeconds scheduledAt, int maxAttempts = 3);

    // Атомарно забирает самую приоритетную готовую задачу (priority, scheduled_at, id).
    // Задачи IN_PROGRESS с истёкшей блокировкой берутся повторно; исчерпавшие попытки —
    // переводятся в FAILED. Пустой `types` — любой тип.
    Result<std::optional<Task>> claimNextTask(UnixSeconds now,
                                              std::chrono::seconds lockTimeout = std::chrono::minutes(5),
                                              std::span<const TaskType> types = {});

    // true, если этот вызов перевёл задачу из IN_PROGRESS в COMPLETED.
    Result<bool> completeTask(std::int64_t taskId);

    // Вернуть захваченную задачу в очередь до момента `until`, не расходуя попытку
    // (например, домен временно ограничен лимитером). true — задача была в работе.
    Result<bool> deferTask(std::int64_t taskId, UnixSeconds until);

    // allowRetry=false — ошибка окончательная (неверный ключ, битые данные): сразу FAILED.
    Result<FailOutcome> failTask(std::int64_t taskId, std::string_view errorMessage, UnixSeconds now,
                                 std::chrono::seconds retryDelay = std::chrono::seconds(30), bool allowRetry = true);

private:
    DatabaseManager();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dropship::db
