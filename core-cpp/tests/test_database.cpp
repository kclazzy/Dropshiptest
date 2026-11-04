#include <atomic>
#include <limits>
#include <thread>

#include "TestDb.hpp"

using namespace testdb;

namespace {

std::int64_t countRows(DatabaseManager& db, const std::string& table) {
    return scalarInt(db, "SELECT COUNT(*) FROM " + table);
}

void insertOrder(DatabaseManager& db, const Fixture& f, const std::string& externalId) {
    unwrap(db.execute(
        "INSERT INTO orders(store_id, external_order_id, variant_id, quantity, buyer_address_json, "
        "retail_total_cents, created_at, updated_at) VALUES(?, ?, ?, 1, '{}', 14499, 1, 1)",
        {f.storeId, externalId, f.variantId}));
}

}  // namespace

TEST(db_open_enables_wal_and_applies_all_migrations) {
    TempDir dir;
    auto db = openDb(dir);

    const auto mode = unwrap(db->query("PRAGMA journal_mode"));
    CHECK_EQ(mode.textAt(0, "journal_mode"), std::string("wal"));
    CHECK_EQ(unwrap(db->schemaVersion()), 3);

    for (const char* table : {"stores", "pricing_rules", "proxies", "products", "product_variants", "orders",
                              "task_queue", "price_history", "audit_log"}) {
        CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?",
                           {std::string(table)}),
                 1);
    }
}

TEST(db_reopen_is_idempotent_and_keeps_data) {
    TempDir dir;
    {
        auto db = openDb(dir);
        insertFixture(*db);
    }
    auto db = openDb(dir);
    CHECK_EQ(unwrap(db->schemaVersion()), 3);
    CHECK_EQ(countRows(*db, "stores"), 1);
    CHECK_EQ(countRows(*db, "product_variants"), 1);
    CHECK_EQ(countRows(*db, "price_history"), 1);  // миграция не дублирует историю
}

TEST(db_roundtrips_every_value_type) {
    TempDir dir;
    auto db = openDb(dir);
    unwrap(db->execute("CREATE TABLE scratch(i INTEGER, r REAL, t TEXT, b BLOB, n)"));

    const std::int64_t big = std::numeric_limits<std::int64_t>::max();
    const std::string text = "Привет, Ozon — 日本語";
    const Blob blob{0, 255, 0, 7};
    unwrap(db->execute("INSERT INTO scratch VALUES(?, ?, ?, ?, ?)", {big, 1.5, text, blob, std::monostate{}}));
    unwrap(db->execute("INSERT INTO scratch VALUES(?, ?, ?, ?, ?)",
                       {std::int64_t{0}, 0.0, std::string(), Blob{}, std::monostate{}}));

    const auto rs = unwrap(db->query("SELECT * FROM scratch ORDER BY rowid"));
    REQUIRE(rs.rows.size() == 2);
    CHECK_EQ(rs.intAt(0, "i"), big);
    CHECK_EQ(std::get<double>(rs.at(0, "r")), 1.5);
    CHECK_EQ(rs.textAt(0, "t"), text);
    CHECK(std::get<Blob>(rs.at(0, "b")) == blob);
    CHECK(rs.isNull(0, "n"));
    CHECK_EQ(rs.textAt(1, "t"), std::string());
    // Пустой BLOB — это BLOB, а не NULL.
    CHECK(!rs.isNull(1, "b"));
    CHECK(std::get<Blob>(rs.at(1, "b")).empty());
}

TEST(db_rejects_invalid_sql_and_misused_parameters) {
    TempDir dir;
    auto db = openDb(dir);

    const auto bad = db->execute("SELEC 1");
    REQUIRE(!bad.has_value());
    CHECK(!bad.error().message.empty());

    CHECK(!db->execute("SELECT ?").has_value());                         // не хватает параметра
    CHECK(!db->execute("SELECT 1", {std::int64_t{1}}).has_value());     // лишний параметр
    CHECK(!db->query("SELECT 1; SELECT 2").has_value());                 // несколько выражений
    CHECK(db->query("SELECT 1;  ").has_value());                         // хвостовой ';' допустим
}

TEST(db_transaction_commits_or_rolls_back_as_one_unit) {
    TempDir dir;
    auto db = openDb(dir);
    const auto insertProxy = [](Tx& tx, const char* url) {
        return tx.execute("INSERT INTO proxies(proxy_url) VALUES(?)", {std::string(url)});
    };

    unwrap(db->transaction([&](Tx& tx) -> Result<void> {
        if (auto r = insertProxy(tx, "http://a"); !r) return unexpected(r.error());
        if (auto r = insertProxy(tx, "http://b"); !r) return unexpected(r.error());
        return {};
    }));
    CHECK_EQ(countRows(*db, "proxies"), 2);

    // Вторая вставка нарушает UNIQUE -> откатывается и первая.
    const auto failed = db->transaction([&](Tx& tx) -> Result<void> {
        if (auto r = insertProxy(tx, "http://c"); !r) return unexpected(r.error());
        if (auto r = insertProxy(tx, "http://a"); !r) return unexpected(r.error());
        return {};
    });
    CHECK(!failed.has_value());
    CHECK_EQ(countRows(*db, "proxies"), 2);

    bool threw = false;
    try {
        (void)db->transaction([&](Tx& tx) -> Result<void> {
            (void)insertProxy(tx, "http://d");
            throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(countRows(*db, "proxies"), 2);
    // После отката писатель не застрял в открытой транзакции.
    unwrap(db->execute("INSERT INTO proxies(proxy_url) VALUES('http://e')"));
    CHECK_EQ(countRows(*db, "proxies"), 3);
}

TEST(db_enforces_foreign_keys_and_cascades) {
    TempDir dir;
    auto db = openDb(dir);
    CHECK(!db->execute("INSERT INTO products(store_id, supplier_url, supplier_product_id, url_sha256, "
                       "raw_title, images_json) VALUES(999, 'u', 'p', 's', 't', '[]')")
               .has_value());

    const auto f = insertFixture(*db);
    unwrap(db->execute("DELETE FROM stores WHERE id = ?", {f.storeId}));
    CHECK_EQ(countRows(*db, "products"), 0);
    CHECK_EQ(countRows(*db, "product_variants"), 0);
    CHECK_EQ(countRows(*db, "price_history"), 0);
}

TEST(db_reader_connections_cannot_write) {
    TempDir dir;
    auto db = openDb(dir);
    CHECK(!db->query("INSERT INTO proxies(proxy_url) VALUES('http://x')").has_value());
    CHECK_EQ(countRows(*db, "proxies"), 0);
}

TEST(db_deduplicates_products_by_url_hash) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db, "same-hash");
    const auto dup = db->execute(
        "INSERT INTO products(store_id, supplier_url, supplier_product_id, url_sha256, raw_title, images_json) "
        "VALUES(?, 'u2', 'p2', 'same-hash', 't', '[]')",
        {f.storeId});
    CHECK(!dup.has_value());
    CHECK_EQ(countRows(*db, "products"), 1);
}

TEST(db_price_history_records_real_changes_only) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    const auto history = [&] {
        return scalarInt(*db, "SELECT COUNT(*) FROM price_history WHERE variant_id=?", {f.variantId});
    };
    CHECK_EQ(history(), 1);  // стартовое состояние

    unwrap(db->execute("UPDATE product_variants SET retail_price_cents = 15999 WHERE id = ?", {f.variantId}));
    CHECK_EQ(history(), 2);

    // Запись того же значения и правка постороннего поля историю не засоряют.
    unwrap(db->execute("UPDATE product_variants SET retail_price_cents = retail_price_cents WHERE id = ?",
                       {f.variantId}));
    unwrap(db->execute("UPDATE product_variants SET is_in_stock = 0 WHERE id = ?", {f.variantId}));
    CHECK_EQ(history(), 2);

    unwrap(db->execute("UPDATE product_variants SET supplier_stock = 3 WHERE id = ?", {f.variantId}));
    CHECK_EQ(history(), 3);

    const auto last = unwrap(db->query(
        "SELECT retail_price_cents, supplier_stock, recorded_at FROM price_history "
        "WHERE variant_id = ? ORDER BY id DESC LIMIT 1",
        {f.variantId}));
    CHECK_EQ(last.intAt(0, "retail_price_cents"), 15999);
    CHECK_EQ(last.intAt(0, "supplier_stock"), 3);
    CHECK(last.intAt(0, "recorded_at") > 0);
}

TEST(db_audit_log_records_user_edits_and_order_status) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);

    unwrap(db->execute("UPDATE products SET title_override = ? WHERE id = ?",
                       {std::string("Мой заголовок"), f.productId}));
    unwrap(db->execute("UPDATE products SET title_override = ? WHERE id = ?",
                       {std::string("Мой заголовок"), f.productId}));  // без изменения
    const auto edits = unwrap(db->query(
        "SELECT * FROM audit_log WHERE entity_type='product' AND entity_id=?", {f.productId}));
    REQUIRE(edits.rows.size() == 1);
    CHECK_EQ(edits.textAt(0, "field"), std::string("title_override"));
    CHECK(edits.isNull(0, "old_value"));
    CHECK_EQ(edits.textAt(0, "new_value"), std::string("Мой заголовок"));

    unwrap(db->execute("UPDATE products SET monitor_status = 'PAUSED' WHERE id = ?", {f.productId}));
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM audit_log WHERE field='monitor_status' AND new_value='PAUSED'"),
             1);

    insertOrder(*db, f, "ext-1");
    unwrap(db->execute("UPDATE orders SET status = 'ORDERED' WHERE external_order_id = 'ext-1'"));
    const auto orderRows = unwrap(db->query("SELECT old_value, new_value FROM audit_log WHERE entity_type='order'"));
    REQUIRE(orderRows.rows.size() == 1);
    CHECK_EQ(orderRows.textAt(0, "old_value"), std::string("NEW"));
    CHECK_EQ(orderRows.textAt(0, "new_value"), std::string("ORDERED"));
}

TEST(db_product_overview_prefers_user_edits_and_aggregates_variants) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    unwrap(db->execute(
        "INSERT INTO product_variants(product_id, supplier_sku, attributes_json, supplier_price_cents, "
        "retail_price_cents, supplier_stock, state_hash, updated_at) VALUES(?, 'SKU-2', '{}', 15000, 20000, 3, 'h', 1)",
        {f.productId}));

    const auto title = [&] {
        return unwrap(db->query("SELECT title FROM v_product_overview WHERE id = ?", {f.productId}))
            .textAt(0, "title");
    };
    CHECK_EQ(title(), std::string("Raw title"));
    unwrap(db->execute("UPDATE products SET ai_seo_title = 'AI title' WHERE id = ?", {f.productId}));
    CHECK_EQ(title(), std::string("AI title"));
    unwrap(db->execute("UPDATE products SET title_override = 'Edited title' WHERE id = ?", {f.productId}));
    CHECK_EQ(title(), std::string("Edited title"));

    const auto rs = unwrap(db->query("SELECT * FROM v_product_overview WHERE id = ?", {f.productId}));
    REQUIRE(rs.rows.size() == 1);
    CHECK_EQ(rs.textAt(0, "store_name"), std::string("Ozon shop"));
    CHECK_EQ(rs.textAt(0, "platform_type"), std::string("ozon"));
    CHECK_EQ(rs.intAt(0, "variant_count"), 2);
    CHECK_EQ(rs.intAt(0, "min_retail_price_cents"), 14499);
    CHECK_EQ(rs.intAt(0, "max_retail_price_cents"), 20000);
    CHECK_EQ(rs.intAt(0, "total_stock"), 13);
}

TEST(db_readers_run_in_parallel_with_the_writer) {
    TempDir dir;
    auto db = openDb(dir, OpenOptions{.readerCount = 3});
    constexpr int kRows = 200;

    std::atomic<bool> done{false};
    std::atomic<int> readerErrors{0};
    std::atomic<int> monotonicViolations{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {  // потоков больше, чем соединений в пуле
        readers.emplace_back([&] {
            std::int64_t last = 0;
            while (!done) {
                const auto rs = db->query("SELECT COUNT(*) AS n FROM proxies");
                if (!rs) {
                    ++readerErrors;
                    return;
                }
                const auto n = rs->intAt(0, "n");
                if (n < last) ++monotonicViolations;
                last = n;
            }
        });
    }

    std::atomic<int> writerErrors{0};
    for (int i = 0; i < kRows; ++i) {
        if (!db->execute("INSERT INTO proxies(proxy_url) VALUES(?)", {"http://p/" + std::to_string(i)})) {
            ++writerErrors;
        }
    }
    done = true;
    for (auto& t : readers) t.join();

    CHECK_EQ(writerErrors.load(), 0);
    CHECK_EQ(readerErrors.load(), 0);
    CHECK_EQ(monotonicViolations.load(), 0);
    CHECK_EQ(countRows(*db, "proxies"), kRows);
}
