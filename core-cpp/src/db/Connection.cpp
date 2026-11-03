#include "db/Connection.hpp"

#include <sqlite3.h>

#include <cctype>

namespace dropship::db {

namespace {

struct StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const noexcept { sqlite3_finalize(stmt); }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

DbError sqliteError(sqlite3* db, int rc) {
    return DbError{db ? sqlite3_extended_errcode(db) : rc, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)};
}

// После первого выражения допустимы только пробелы и ';'.
bool hasTrailingSql(const char* tail, const char* end) {
    for (; tail && tail < end; ++tail) {
        if (*tail != ';' && !std::isspace(static_cast<unsigned char>(*tail))) return true;
    }
    return false;
}

Result<StmtPtr> prepare(sqlite3* db, std::string_view sql) {
    sqlite3_stmt* raw = nullptr;
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw, &tail);
    StmtPtr stmt(raw);
    if (rc != SQLITE_OK) return unexpected(sqliteError(db, rc));
    if (!stmt) return unexpected(makeError("empty SQL statement"));
    if (hasTrailingSql(tail, sql.data() + sql.size())) {
        return unexpected(makeError("multiple SQL statements are not allowed here"));
    }
    return stmt;
}

struct Binder {
    sqlite3_stmt* stmt;
    int index;

    int operator()(std::monostate) const { return sqlite3_bind_null(stmt, index); }
    int operator()(std::int64_t v) const { return sqlite3_bind_int64(stmt, index, v); }
    int operator()(double v) const { return sqlite3_bind_double(stmt, index, v); }
    int operator()(const std::string& v) const {
        return sqlite3_bind_text64(stmt, index, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    }
    int operator()(const Blob& v) const {
        // bind_blob64 с нулевым указателем создаёт NULL, а не пустой BLOB.
        if (v.empty()) return sqlite3_bind_zeroblob(stmt, index, 0);
        return sqlite3_bind_blob64(stmt, index, v.data(), v.size(), SQLITE_TRANSIENT);
    }
};

Result<void> bindAll(sqlite3* db, sqlite3_stmt* stmt, const Params& params) {
    const int expected = sqlite3_bind_parameter_count(stmt);
    if (expected != static_cast<int>(params.size())) {
        return unexpected(makeError("parameter count mismatch: SQL expects " + std::to_string(expected) +
                                    ", got " + std::to_string(params.size())));
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const int rc = std::visit(Binder{stmt, static_cast<int>(i) + 1}, params[i]);
        if (rc != SQLITE_OK) return unexpected(sqliteError(db, rc));
    }
    return {};
}

Value readValue(sqlite3_stmt* stmt, int column) {
    switch (sqlite3_column_type(stmt, column)) {
        case SQLITE_INTEGER:
            return static_cast<std::int64_t>(sqlite3_column_int64(stmt, column));
        case SQLITE_FLOAT:
            return sqlite3_column_double(stmt, column);
        case SQLITE_TEXT: {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
            const int bytes = sqlite3_column_bytes(stmt, column);
            return std::string(text, static_cast<std::size_t>(bytes));
        }
        case SQLITE_BLOB: {
            const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt, column));
            const int bytes = sqlite3_column_bytes(stmt, column);
            if (bytes == 0) return Blob{};
            return Blob(data, data + bytes);
        }
        default:
            return std::monostate{};
    }
}

// Пошагово выполняет выражение; строки собираются, только если out != nullptr.
Result<void> run(sqlite3* db, sqlite3_stmt* stmt, ResultSet* out) {
    if (out) {
        const int columns = sqlite3_column_count(stmt);
        for (int i = 0; i < columns; ++i) out->columns.emplace_back(sqlite3_column_name(stmt, i));
    }
    for (;;) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) return {};
        if (rc != SQLITE_ROW) return unexpected(sqliteError(db, rc));
        if (out) {
            std::vector<Value> row;
            row.reserve(out->columns.size());
            for (int i = 0; i < static_cast<int>(out->columns.size()); ++i) row.push_back(readValue(stmt, i));
            out->rows.push_back(std::move(row));
        }
    }
}

// Путь передаётся SQLite в UTF-8 (на Windows string() — это ANSI-кодовая страница).
std::string utf8Path(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

Result<std::unique_ptr<Connection>> Connection::open(const std::filesystem::path& file, OpenMode mode,
                                                     std::chrono::milliseconds busyTimeout) {
    // Соединением пользуется один поток за раз (гарантирует DatabaseManager) — мьютекс SQLite не нужен.
    const int flags = (mode == OpenMode::ReadWrite ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                                   : SQLITE_OPEN_READONLY) |
                      SQLITE_OPEN_NOMUTEX;
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(utf8Path(file).c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        DbError error = sqliteError(raw, rc);
        sqlite3_close(raw);
        return unexpected(std::move(error));
    }
    std::unique_ptr<Connection> connection(new Connection(raw));
    sqlite3_extended_result_codes(raw, 1);
    sqlite3_busy_timeout(raw, static_cast<int>(busyTimeout.count()));
    return connection;
}

Connection::~Connection() { sqlite3_close_v2(db_); }

Result<ExecResult> Connection::execute(std::string_view sql, const Params& params) {
    auto stmt = prepare(db_, sql);
    if (!stmt) return unexpected(stmt.error());
    if (auto bound = bindAll(db_, stmt->get(), params); !bound) return unexpected(bound.error());
    if (auto done = run(db_, stmt->get(), nullptr); !done) return unexpected(done.error());

    ExecResult result;
    // changes() относится к последнему DML и для SELECT/PRAGMA был бы устаревшим.
    if (!sqlite3_stmt_readonly(stmt->get())) result.changes = sqlite3_changes64(db_);
    result.lastInsertRowId = sqlite3_last_insert_rowid(db_);
    return result;
}

Result<ResultSet> Connection::query(std::string_view sql, const Params& params) {
    auto stmt = prepare(db_, sql);
    if (!stmt) return unexpected(stmt.error());
    if (auto bound = bindAll(db_, stmt->get(), params); !bound) return unexpected(bound.error());
    ResultSet rows;
    if (auto done = run(db_, stmt->get(), &rows); !done) return unexpected(done.error());
    return rows;
}

Result<void> Connection::executeScript(const std::string& sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return {};
    DbError error{sqlite3_extended_errcode(db_), message ? message : sqlite3_errstr(rc)};
    sqlite3_free(message);
    return unexpected(std::move(error));
}

}  // namespace dropship::db
