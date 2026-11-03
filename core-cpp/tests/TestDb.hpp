#pragma once

// Общие помощники для тестов базы данных.

#include <atomic>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "Harness.hpp"
#include "db/DatabaseManager.hpp"

namespace testdb {

using namespace dropship;
using namespace dropship::db;

// Временный каталог, удаляемый при выходе из области видимости.
class TempDir {
public:
    TempDir() {
        static const unsigned token = std::random_device{}();
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("dropship_test_" + std::to_string(token) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::filesystem::path file(const std::string& name = "test.db") const { return path_ / name; }

private:
    std::filesystem::path path_;
};

template <class T>
T unwrap(Result<T>&& r) {
    if (!r) {
        ::testing::fail(__FILE__, __LINE__,
                        "unexpected db error (" + std::to_string(r.error().sqliteCode) +
                            "): " + r.error().message);
        throw ::testing::Abort{};
    }
    return std::move(*r);
}

inline void unwrap(Result<void>&& r) {
    if (!r) {
        ::testing::fail(__FILE__, __LINE__,
                        "unexpected db error (" + std::to_string(r.error().sqliteCode) +
                            "): " + r.error().message);
        throw ::testing::Abort{};
    }
}

inline std::unique_ptr<DatabaseManager> openDb(const TempDir& dir, OpenOptions options = {}) {
    return unwrap(DatabaseManager::open(dir.file(), options));
}

inline std::int64_t scalarInt(DatabaseManager& db, std::string_view sql, const Params& params = {}) {
    const auto rs = unwrap(db.query(sql, params));
    if (rs.rows.empty()) throw std::runtime_error("scalarInt: no rows");
    return std::get<std::int64_t>(rs.rows[0][0]);
}

// Минимальный набор связанных строк: магазин -> товар -> вариант.
struct Fixture {
    std::int64_t storeId = 0;
    std::int64_t productId = 0;
    std::int64_t variantId = 0;
};

inline Fixture insertFixture(DatabaseManager& db, std::string_view sha = "sha-1") {
    Fixture f;
    f.storeId = unwrap(db.execute(
                           "INSERT INTO stores(name, platform_type, api_url, encrypted_credentials, created_at) "
                           "VALUES(?, ?, ?, ?, ?)",
                           {std::string("Ozon shop"), std::string("ozon"), std::string("https://api.ozon.example"),
                            Blob{1, 2, 3}, std::int64_t{1000}}))
                    .lastInsertRowId;
    f.productId = unwrap(db.execute(
                             "INSERT INTO products(store_id, supplier_url, supplier_product_id, url_sha256, "
                             "raw_title, images_json) VALUES(?, ?, ?, ?, ?, ?)",
                             {f.storeId, std::string("https://supplier.example/p/1"), std::string("p1"),
                              std::string(sha), std::string("Raw title"), std::string("[]")}))
                      .lastInsertRowId;
    f.variantId = unwrap(db.execute(
                             "INSERT INTO product_variants(product_id, supplier_sku, attributes_json, "
                             "supplier_price_cents, retail_price_cents, supplier_stock, state_hash, updated_at) "
                             "VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                             {f.productId, std::string("SKU-1"), std::string("{}"), std::int64_t{10000},
                              std::int64_t{14499}, std::int64_t{10}, std::string("h0"), std::int64_t{1000}}))
                      .lastInsertRowId;
    return f;
}

}  // namespace testdb
