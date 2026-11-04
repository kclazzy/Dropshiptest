#include "TestDb.hpp"
#include "db/Bootstrap.hpp"
#include "importer/ProductImport.hpp"

using namespace testdb;

TEST(bootstrap_creates_one_local_store_on_a_fresh_database_only_once) {
    TempDir dir;
    auto db = openDb(dir);
    unwrap(ensureDefaultStore(*db, 100));
    unwrap(ensureDefaultStore(*db, 200));
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM stores"), 1);
    const auto row = unwrap(db->query("SELECT platform_type, is_active FROM stores"));
    CHECK_EQ(row.textAt(0, "platform_type"), std::string("local"));
    CHECK_EQ(row.intAt(0, "is_active"), 1);
}

TEST(bootstrap_leaves_existing_stores_alone) {
    TempDir dir;
    auto db = openDb(dir);
    insertFixture(*db);
    unwrap(ensureDefaultStore(*db, 100));
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM stores"), 1);
}
