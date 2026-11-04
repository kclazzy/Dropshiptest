#include "FakeTransport.hpp"
#include "TestDb.hpp"
#include "store/StoreSync.hpp"

using namespace testdb;
using namespace dropship::store;

namespace {

struct Rig {
    TempDir dir;
    std::unique_ptr<DatabaseManager> db = openDb(dir);
    CredentialVault vault = CredentialVault::fromKey(CredentialVault::Key{});
    testfakes::FakeTransport transport;
    StoreRegistry registry{*db, vault, transport, [] { return 1000.0; }};
    Fixture f = insertFixture(*db);

    void setStore(const std::string& platform, const std::string& credentialsJson) {
        const auto blob = *vault.encrypt(credentialsJson);
        unwrap(db->execute("UPDATE stores SET platform_type = ?, encrypted_credentials = ? WHERE id = ?",
                           {platform, db::Blob(blob.begin(), blob.end()), f.storeId}));
    }
};

}  // namespace

TEST(registry_builds_an_ozon_adapter_from_encrypted_credentials) {
    Rig rig;
    rig.setStore("ozon", R"({"client_id":"c","api_key":"k","warehouse_id":42})");
    const auto adapter = rig.registry.adapterFor(rig.f.storeId);
    REQUIRE(adapter.has_value());
    CHECK_EQ((*adapter)->name(), std::string("ozon"));
    CHECK(*rig.registry.adapterFor(rig.f.storeId) == *adapter);  // кэш: тот же объект
}

TEST(registry_rebuilds_the_adapter_when_credentials_change) {
    Rig rig;
    rig.setStore("avito", R"({"client_id":"c","client_secret":"s"})");
    auto* first = *rig.registry.adapterFor(rig.f.storeId);
    CHECK_EQ(first->name(), std::string("avito"));
    rig.setStore("ozon", R"({"client_id":"c","api_key":"k","warehouse_id":1})");
    CHECK_EQ((*rig.registry.adapterFor(rig.f.storeId))->name(), std::string("ozon"));
}

TEST(registry_reports_missing_broken_or_foreign_credentials_and_unknown_platforms) {
    Rig rig;
    CHECK(rig.registry.adapterFor(9999).error().kind == StoreError::Kind::Config);
    // В фикстуре учётные данные — не шифртекст нашего сейфа.
    CHECK(rig.registry.adapterFor(rig.f.storeId).error().kind == StoreError::Kind::Config);
    rig.setStore("ozon", "not json");
    CHECK(rig.registry.adapterFor(rig.f.storeId).error().kind == StoreError::Kind::Config);
    rig.setStore("wildberries", "{}");
    CHECK(rig.registry.adapterFor(rig.f.storeId).error().kind == StoreError::Kind::Unsupported);
    unwrap(rig.db->execute("UPDATE stores SET encrypted_credentials = x''"));
    CHECK(rig.registry.adapterFor(rig.f.storeId).error().kind == StoreError::Kind::Config);
}
