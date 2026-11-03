#include "db/DatabaseManager.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>

#include "db/Connection.hpp"
#include "db/Migrations.hpp"

namespace dropship::db {

// ---- ResultSet ----

const Value& ResultSet::at(std::size_t row, std::string_view column) const {
    const auto it = std::find(columns.begin(), columns.end(), column);
    if (it == columns.end()) throw std::out_of_range("no such column: " + std::string(column));
    return rows.at(row)[static_cast<std::size_t>(it - columns.begin())];
}

std::int64_t ResultSet::intAt(std::size_t row, std::string_view column) const {
    return std::get<std::int64_t>(at(row, column));
}

const std::string& ResultSet::textAt(std::size_t row, std::string_view column) const {
    return std::get<std::string>(at(row, column));
}

bool ResultSet::isNull(std::size_t row, std::string_view column) const {
    return std::holds_alternative<std::monostate>(at(row, column));
}

// ---- Tx ----

Result<ExecResult> Tx::execute(std::string_view sql, const Params& params) {
    return connection_.execute(sql, params);
}

Result<ResultSet> Tx::query(std::string_view sql, const Params& params) {
    return connection_.query(sql, params);
}

// ---- DatabaseManager ----

struct DatabaseManager::Impl {
    // Единственный писатель: все изменения сериализуются этим мьютексом.
    std::unique_ptr<Connection> writer;
    std::mutex writerMutex;

    // Пул read-only соединений; поток берёт соединение на время одного запроса.
    std::mutex readerMutex;
    std::condition_variable readerAvailable;
    std::vector<std::unique_ptr<Connection>> idleReaders;

    class ReaderLease {
    public:
        explicit ReaderLease(Impl& impl) : impl_(impl) {
            std::unique_lock lock(impl_.readerMutex);
            impl_.readerAvailable.wait(lock, [&] { return !impl_.idleReaders.empty(); });
            connection_ = std::move(impl_.idleReaders.back());
            impl_.idleReaders.pop_back();
        }
        ~ReaderLease() {
            {
                std::lock_guard lock(impl_.readerMutex);
                impl_.idleReaders.push_back(std::move(connection_));
            }
            impl_.readerAvailable.notify_one();
        }
        ReaderLease(const ReaderLease&) = delete;
        ReaderLease& operator=(const ReaderLease&) = delete;
        Connection& get() { return *connection_; }

    private:
        Impl& impl_;
        std::unique_ptr<Connection> connection_;
    };
};

DatabaseManager::DatabaseManager() = default;
DatabaseManager::~DatabaseManager() = default;

Result<std::unique_ptr<DatabaseManager>> DatabaseManager::open(const std::filesystem::path& file,
                                                               OpenOptions options) {
    if (file.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec) return unexpected(makeError("cannot create database directory: " + ec.message()));
    }

    auto writer = Connection::open(file, OpenMode::ReadWrite, options.busyTimeout);
    if (!writer) return unexpected(writer.error());

    // WAL позволяет читателям не блокироваться писателем. Режим хранится в файле БД.
    const auto mode = (*writer)->query("PRAGMA journal_mode=WAL", {});
    if (!mode) return unexpected(mode.error());
    if (mode->rows.empty() || mode->textAt(0, "journal_mode") != "wal") {
        return unexpected(makeError("database does not support WAL journal mode"));
    }
    if (auto pragmas = (*writer)->executeScript("PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"); !pragmas) {
        return unexpected(pragmas.error());
    }
    if (auto migrated = migrate(**writer); !migrated) return unexpected(migrated.error());

    auto manager = std::unique_ptr<DatabaseManager>(new DatabaseManager());
    manager->impl_ = std::make_unique<Impl>();
    manager->impl_->writer = std::move(*writer);

    const std::size_t readers = std::max<std::size_t>(options.readerCount, 1);
    for (std::size_t i = 0; i < readers; ++i) {
        auto reader = Connection::open(file, OpenMode::ReadOnly, options.busyTimeout);
        if (!reader) return unexpected(reader.error());
        // Страховка поверх SQLITE_OPEN_READONLY: читатель не может изменить данные ни при каких условиях.
        if (auto guard = (*reader)->executeScript("PRAGMA query_only=ON;"); !guard) return unexpected(guard.error());
        manager->impl_->idleReaders.push_back(std::move(*reader));
    }
    return manager;
}

Result<ExecResult> DatabaseManager::execute(std::string_view sql, const Params& params) {
    std::lock_guard lock(impl_->writerMutex);
    return impl_->writer->execute(sql, params);
}

Result<ResultSet> DatabaseManager::query(std::string_view sql, const Params& params) {
    Impl::ReaderLease lease(*impl_);
    return lease.get().query(sql, params);
}

Result<void> DatabaseManager::transaction(const std::function<Result<void>(Tx&)>& body) {
    std::lock_guard lock(impl_->writerMutex);
    Connection& writer = *impl_->writer;

    if (auto begun = writer.execute("BEGIN IMMEDIATE", {}); !begun) return unexpected(begun.error());

    // Откат при ошибке, исключении и при неудачном COMMIT.
    struct Rollback {
        Connection& connection;
        bool armed = true;
        ~Rollback() {
            if (armed) (void)connection.execute("ROLLBACK", {});
        }
    } rollback{writer};

    Tx tx(writer);
    if (auto result = body(tx); !result) return result;
    if (auto committed = writer.execute("COMMIT", {}); !committed) return unexpected(committed.error());
    rollback.armed = false;
    return {};
}

Result<int> DatabaseManager::schemaVersion() {
    const auto rs = query("SELECT COALESCE(MAX(version), 0) AS version FROM schema_migrations");
    if (!rs) return unexpected(rs.error());
    return static_cast<int>(rs->intAt(0, "version"));
}

}  // namespace dropship::db
