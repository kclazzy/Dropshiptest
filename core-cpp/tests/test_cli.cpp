#include <sstream>

#include "TestDb.hpp"
#include "app/Cli.hpp"

using namespace testdb;
using namespace dropship;

namespace {

struct Run {
    int code = 0;
    std::string out, err;
};

Run cmd(DatabaseManager& db, const store::CredentialVault& vault, std::vector<std::string> args, const std::string& stdinText = "") {
    std::istringstream in(stdinText);
    std::ostringstream out, err;
    Run r;
    r.code = cli::run(args, db, vault, in, out, err, 500);
    r.out = out.str();
    r.err = err.str();
    return r;
}

const store::CredentialVault kVault = store::CredentialVault::fromKey(store::CredentialVault::Key{});

}  // namespace

TEST(cli_store_add_encrypts_credentials_and_lists_the_store) {
    TempDir dir;
    auto db = openDb(dir);
    const auto r = cmd(*db, kVault, {"store-add", "--name", "Мой Ozon", "--platform", "ozon"},
                       R"({"client_id":"1","api_key":"top-secret","warehouse_id":9})");
    CHECK_EQ(r.code, 0);
    const auto row = unwrap(db->query("SELECT encrypted_credentials, platform_type FROM stores"));
    const auto& blob = std::get<db::Blob>(row.at(0, "encrypted_credentials"));
    CHECK(std::string(blob.begin(), blob.end()).find("top-secret") == std::string::npos);
    CHECK(kVault.decrypt(store::CredentialVault::Blob(blob.begin(), blob.end())).has_value());
    const auto list = cmd(*db, kVault, {"store-list"});
    CHECK(list.out.find("Мой Ozon") != std::string::npos);
    CHECK(list.out.find("ozon") != std::string::npos);
    CHECK(list.out.find("top-secret") == std::string::npos);
}

TEST(cli_store_add_rejects_bad_input_without_touching_the_database) {
    TempDir dir;
    auto db = openDb(dir);
    CHECK_EQ(cmd(*db, kVault, {"store-add", "--name", "x"}).code, 2);                                   // нет платформы
    CHECK_EQ(cmd(*db, kVault, {"store-add", "--name", "x", "--platform", "amazon"}, "{}").code, 2);     // неизвестная
    CHECK_EQ(cmd(*db, kVault, {"store-add", "--name", "x", "--platform", "ozon"}, "not json").code, 2);
    CHECK_EQ(cmd(*db, kVault, {"store-add", "--name", "x", "--platform", "ozon"}, R"({"client_id":"1","api_key":"k"})").code, 2);  // нет склада
    CHECK_EQ(cmd(*db, kVault, {"store-add", "--name", "x", "--platform", "avito"}, R"({"client_id":"1"})").code, 2);
    CHECK_EQ(cmd(*db, kVault, {"store-add", "--name", "x", "--platform", "wildberries"}, "{}").code, 2);  // адаптера ещё нет
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM stores"), 0);
}

TEST(cli_local_store_needs_no_credentials) {
    TempDir dir;
    auto db = openDb(dir);
    CHECK_EQ(cmd(*db, kVault, {"store-add", "--name", "Каталог", "--platform", "local"}).code, 0);
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM stores"), 1);
}

TEST(cli_link_sets_clears_and_validates) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    const auto id = std::to_string(f.variantId);
    CHECK_EQ(cmd(*db, kVault, {"link", "--variant", id, "--external", "OFFER-7"}).code, 0);
    CHECK_EQ(unwrap(db->query("SELECT external_variant_id FROM product_variants")).textAt(0, "external_variant_id"), std::string("OFFER-7"));
    const auto shown = cmd(*db, kVault, {"variants"});
    CHECK(shown.out.find("OFFER-7") != std::string::npos);
    CHECK(shown.out.find("SKU-1") != std::string::npos);
    CHECK_EQ(cmd(*db, kVault, {"link", "--variant", id, "--external", "-"}).code, 0);
    CHECK(unwrap(db->query("SELECT external_variant_id FROM product_variants")).isNull(0, "external_variant_id"));
    CHECK_EQ(cmd(*db, kVault, {"link", "--variant", "9999", "--external", "X"}).code, 1);
    CHECK_EQ(cmd(*db, kVault, {"link", "--variant", "abc", "--external", "X"}).code, 2);
    CHECK_EQ(cmd(*db, kVault, {"link", "--variant", "1; DROP TABLE stores", "--external", "X"}).code, 2);
    CHECK_EQ(cmd(*db, kVault, {"link", "--variant", id}).code, 2);
    CHECK_EQ(cmd(*db, kVault, {"variants", "--product", "x"}).code, 2);
}

TEST(cli_recognises_its_commands_only) {
    CHECK(cli::isCommand("store-add"));
    CHECK(cli::isCommand("link"));
    CHECK(!cli::isCommand("--port"));
    CHECK(!cli::isCommand(""));
}

TEST(cli_order_commands_cover_the_manual_flow) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    unwrap(db->execute("UPDATE product_variants SET external_variant_id = 'OFFER-1' WHERE id = ?", {f.variantId}));
    unwrap(db->execute(
        "INSERT INTO pricing_rules(store_id, price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, "
        "marketplace_fee_percent, rounding_cents, min_stock_threshold, min_profit_cents) VALUES(?, 0, 9000000000000, 0, 0, 10.0, NULL, 1, 200)",
        {f.storeId}));
    const std::string store = std::to_string(f.storeId);
    const auto add = cmd(*db, kVault, {"order-add", "--store", store, "--id", "A-1", "--offer", "OFFER-1", "--qty", "1", "--total", "144.99"},
                         R"({"city":"Тула"})");
    CHECK_EQ(add.code, 0);
    CHECK(add.out.find("queued") != std::string::npos);
    CHECK_EQ(cmd(*db, kVault, {"order-add", "--store", store, "--id", "A-1", "--offer", "OFFER-1", "--qty", "1", "--total", "144.99"}, "{}").code, 0);
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM orders"), 1);
    CHECK_EQ(cmd(*db, kVault, {"order-add", "--store", store, "--id", "A-2", "--offer", "OFFER-1", "--qty", "1", "--total", "12.345"}, "{}").code, 2);
    CHECK_EQ(cmd(*db, kVault, {"order-add", "--store", store, "--id", "A-2", "--offer", "OFFER-1", "--qty", "1", "--total", "144.99"}, "garbage").code, 2);

    CHECK_EQ(cmd(*db, kVault, {"order-track", "--order", "1", "--number", "T1"}).code, 2);  // ещё не куплен
    CHECK_EQ(cmd(*db, kVault, {"order-done", "--order", "1", "--supplier-order", "S-5"}).code, 0);
    CHECK_EQ(cmd(*db, kVault, {"order-track", "--order", "1", "--number", "T1", "--carrier", "CDEK"}).code, 0);
    const auto list = cmd(*db, kVault, {"orders", "--status", "SHIPPED"});
    CHECK(list.out.find("A-1") != std::string::npos);
    CHECK(list.out.find("T1") != std::string::npos);
    CHECK(cmd(*db, kVault, {"orders", "--status", "NEW"}).out.find("A-1") == std::string::npos);
    CHECK_EQ(cmd(*db, kVault, {"order-retry", "--order", "1"}).code, 0);
    CHECK_EQ(cmd(*db, kVault, {"order-done", "--order", "x", "--supplier-order", "S"}).code, 2);
}
