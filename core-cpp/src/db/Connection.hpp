#pragma once

// Внутренний RAII-слой над sqlite3*. Соединение не потокобезопасно само по себе:
// DatabaseManager гарантирует, что одновременно им пользуется один поток.

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "db/DatabaseManager.hpp"

struct sqlite3;

namespace dropship::db {

enum class OpenMode { ReadWrite, ReadOnly };

// Ошибка уровня приложения (не SQLite): sqliteCode == 0.
inline DbError makeError(std::string message) { return DbError{0, std::move(message)}; }

class Connection {
public:
    static Result<std::unique_ptr<Connection>> open(const std::filesystem::path& file, OpenMode mode,
                                                    std::chrono::milliseconds busyTimeout);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Ровно одно SQL-выражение; значения — только через params.
    Result<ExecResult> execute(std::string_view sql, const Params& params);
    Result<ResultSet> query(std::string_view sql, const Params& params);
    // Несколько выражений без параметров (DDL миграций, PRAGMA).
    Result<void> executeScript(const std::string& sql);

private:
    explicit Connection(sqlite3* db) : db_(db) {}
    sqlite3* db_;
};

}  // namespace dropship::db
