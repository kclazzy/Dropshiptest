#include "Harness.hpp"
#include "TestDb.hpp"
#include "importer/ProductImport.hpp"
#include "importer/UrlNormalizer.hpp"
#include "util/Sha256.hpp"

using namespace dropship;
using namespace dropship::importer;
using namespace testdb;

namespace {

Json validPayload() {
    return Json::parse(R"({
        "url": "https://Shop.Example/p/1?utm_source=ad&color=red",
        "title": "Наушники X1",
        "description": "Bluetooth 5.3",
        "images": ["https://img.example/1.jpg", "https://img.example/2.jpg"],
        "variants": [
          {"sku": "A", "attributes": {"color": "Black"}, "price_cents": 129900, "shipping_cents": 5000, "stock": 12},
          {"sku": "B", "attributes": {"color": "White"}, "price_cents": 139900, "stock": 0}
        ]})");
}

bool invalidParam(const Json& payload, const std::string& mention) {
    const auto r = parseImportPayload(payload);
    return !r.has_value() && r.error().code == bridge::rpc_code::kInvalidParams &&
           r.error().message.find(mention) != std::string::npos;
}

Json with(Json base, const std::string& key, Json value) {
    base[key] = std::move(value);
    return base;
}

std::int64_t addStore(DatabaseManager& db, bool active = true, const std::string& name = "Ozon") {
    return unwrap(db.execute("INSERT INTO stores(name, platform_type, api_url, encrypted_credentials, is_active, created_at) "
                             "VALUES(?, 'ozon', 'https://api.example', x'00', ?, 1)",
                             {name, std::int64_t{active ? 1 : 0}}))
        .lastInsertRowId;
}

ImportPayload parsedOk(const Json& json) {
    auto r = parseImportPayload(json);
    if (!r) {
        ::testing::fail(__FILE__, __LINE__, "payload rejected: " + r.error().message);
        throw ::testing::Abort{};
    }
    return std::move(*r);
}

}  // namespace

// ---- SHA-256 ----

TEST(sha256_matches_known_vectors) {
    CHECK_EQ(sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

// ---- нормализация ссылок ----

TEST(url_normalization_removes_noise_and_sorts_params) {
    CHECK_EQ(*normalizeUrl("HTTPS://Example.COM:443/Path/?b=2&a=1&utm_source=x&gclid=1#frag"),
             std::string("https://example.com/Path?a=1&b=2"));
    CHECK_EQ(*normalizeUrl("http://example.com"), std::string("http://example.com/"));
    CHECK_EQ(*normalizeUrl("http://example.com:80/x"), std::string("http://example.com/x"));
    CHECK_EQ(*normalizeUrl("http://example.com:8080/x"), std::string("http://example.com:8080/x"));
    CHECK_EQ(*normalizeUrl("https://example.com/?utm_medium=a&UTM_Campaign=b"), std::string("https://example.com/"));
    CHECK_EQ(*normalizeUrl("https://example.com/p?id=5&&flag"), std::string("https://example.com/p?flag&id=5"));
}

TEST(url_normalization_makes_tracking_variants_equal) {
    CHECK_EQ(*normalizeUrl("https://a.example/p/1?utm_source=a"), *normalizeUrl("https://A.example/p/1/#top"));
}

TEST(url_normalization_rejects_unsafe_input) {
    for (const char* bad : {"", "ftp://example.com/x", "javascript:alert(1)", "//example.com/x", "http:///x",
                            "http://user@evil.example/", "http://user:pw@evil.example/", "https://exa mple.com/",
                            "https://example.com/a\nb", "http://example.com:99999/", "http://example.com:abc/",
                            "example.com/x"}) {
        CHECK(!normalizeUrl(bad).has_value());
    }
}

// ---- разбор payload ----

TEST(import_parse_reads_all_fields_and_normalizes_the_url) {
    const auto p = parsedOk(validPayload());
    CHECK_EQ(p.supplierUrl, std::string("https://shop.example/p/1?color=red"));
    CHECK_EQ(p.title, std::string("Наушники X1"));
    CHECK_EQ(p.imageUrls.size(), std::size_t{2});
    REQUIRE(p.variants.size() == 2);
    CHECK_EQ(p.variants[0].sku, std::string("A"));
    CHECK_EQ(p.variants[0].attributes.at("color"), std::string("Black"));
    CHECK_EQ(p.variants[0].priceCents, Cents{129900});
    CHECK_EQ(p.variants[0].shippingCents, Cents{5000});
    CHECK_EQ(p.variants[0].stock, std::int64_t{12});
    CHECK_EQ(p.variants[1].shippingCents, Cents{0});  // не задано — 0
    CHECK(!p.storeId.has_value());
    CHECK_EQ(p.supplierProductId.size(), std::size_t{16});  // не задан — префикс хэша ссылки
}

TEST(import_parse_accepts_minimal_single_variant) {
    const Json minimal = Json::parse(R"({"url":"https://s.example/x","title":"T",
        "variants":[{"price_cents":100,"stock":3}]})");
    const auto p = parsedOk(minimal);
    CHECK_EQ(p.variants[0].sku, std::string("default"));
    CHECK(p.imageUrls.empty());
    CHECK_EQ(p.description, std::string());
}

TEST(import_parse_rejects_bad_fields_and_names_them) {
    const Json ok = validPayload();
    CHECK(invalidParam(Json::array(), "params"));
    Json noUrl = ok;
    noUrl.erase("url");
    CHECK(invalidParam(noUrl, "url"));
    CHECK(invalidParam(with(ok, "url", "ftp://x.example/p"), "url"));
    CHECK(invalidParam(with(ok, "title", "   "), "title"));
    CHECK(invalidParam(with(ok, "title", std::string(3000, 'x')), "title"));
    CHECK(invalidParam(with(ok, "description", 5), "description"));
    CHECK(invalidParam(with(ok, "images", "x"), "images"));
    CHECK(invalidParam(with(ok, "images", Json::array({"javascript:alert(1)"})), "images[0]"));
    CHECK(invalidParam(with(ok, "variants", Json::array()), "variants"));
    CHECK(invalidParam(with(ok, "store_id", "1"), "store_id"));
    CHECK(invalidParam(with(ok, "store_id", 0), "store_id"));

    Json tooManyImages = Json::array();
    for (int i = 0; i < 51; ++i) tooManyImages.push_back("https://img.example/" + std::to_string(i));
    CHECK(invalidParam(with(ok, "images", tooManyImages), "images"));
}

TEST(import_parse_rejects_bad_variants_and_names_them) {
    const auto badVariant = [&](const std::string& key, Json value) {
        Json p = validPayload();
        p["variants"][1][key] = std::move(value);
        return p;
    };
    CHECK(invalidParam(badVariant("price_cents", -1), "variants[1].price_cents"));
    CHECK(invalidParam(badVariant("price_cents", 12.5), "variants[1].price_cents"));
    CHECK(invalidParam(badVariant("price_cents", "100"), "variants[1].price_cents"));
    CHECK(invalidParam(badVariant("shipping_cents", -5), "variants[1].shipping_cents"));
    CHECK(invalidParam(badVariant("stock", -1), "variants[1].stock"));
    CHECK(invalidParam(badVariant("stock", 1.5), "variants[1].stock"));
    CHECK(invalidParam(badVariant("sku", ""), "variants[1].sku"));
    CHECK(invalidParam(badVariant("sku", "A"), "variants[1].sku"));  // дубль SKU
    CHECK(invalidParam(badVariant("attributes", Json::array()), "variants[1].attributes"));
    CHECK(invalidParam(badVariant("attributes", Json{{"color", 5}}), "variants[1].attributes"));

    Json noPrice = validPayload();
    noPrice["variants"][0].erase("price_cents");
    CHECK(invalidParam(noPrice, "variants[0].price_cents"));
    Json noStock = validPayload();
    noStock["variants"][0].erase("stock");
    CHECK(invalidParam(noStock, "variants[0].stock"));

    Json tooMany = validPayload();
    tooMany["variants"] = Json::array();
    for (int i = 0; i < 201; ++i) {
        tooMany["variants"].push_back(Json{{"sku", "S" + std::to_string(i)}, {"price_cents", 1}, {"stock", 1}});
    }
    CHECK(invalidParam(tooMany, "variants"));
}

// ---- запись в БД ----

namespace {

ImportOutcome imported(DatabaseManager& db, const ImportPayload& payload, UnixSeconds now = 5000) {
    auto r = importProduct(db, payload, now);
    if (!r) {
        ::testing::fail(__FILE__, __LINE__, "import failed: " + r.error().message);
        throw ::testing::Abort{};
    }
    return *r;
}

int errorCode(DatabaseManager& db, const ImportPayload& payload) {
    const auto r = importProduct(db, payload, 5000);
    return r.has_value() ? 0 : r.error().code;
}

std::int64_t count(DatabaseManager& db, const std::string& sql, const Params& params = {}) {
    return scalarInt(db, sql, params);
}

}  // namespace

TEST(import_creates_product_variants_history_and_ai_task) {
    TempDir dir;
    auto db = openDb(dir);
    const auto store = addStore(*db);

    const auto out = imported(*db, parsedOk(validPayload()));
    CHECK(!out.duplicate);
    CHECK_EQ(out.variantCount, std::size_t{2});

    const auto product = unwrap(db->query("SELECT * FROM products WHERE id = ?", {out.productId}));
    REQUIRE(product.rows.size() == 1);
    CHECK_EQ(product.intAt(0, "store_id"), store);
    CHECK_EQ(product.textAt(0, "supplier_url"), std::string("https://shop.example/p/1?color=red"));
    CHECK_EQ(product.textAt(0, "url_sha256"), sha256Hex("https://shop.example/p/1?color=red"));
    CHECK_EQ(product.textAt(0, "raw_title"), std::string("Наушники X1"));
    CHECK_EQ(Json::parse(product.textAt(0, "images_json")).size(), std::size_t{2});
    CHECK_EQ(product.textAt(0, "ai_status"), std::string("PENDING"));
    CHECK_EQ(product.intAt(0, "created_at"), std::int64_t{5000});

    const auto variants = unwrap(db->query("SELECT * FROM product_variants WHERE product_id = ? ORDER BY id", {out.productId}));
    REQUIRE(variants.rows.size() == 2);
    CHECK_EQ(variants.textAt(0, "supplier_sku"), std::string("A"));
    CHECK_EQ(variants.intAt(0, "supplier_price_cents"), std::int64_t{129900});
    CHECK_EQ(variants.intAt(0, "shipping_price_cents"), std::int64_t{5000});
    CHECK_EQ(variants.intAt(0, "supplier_stock"), std::int64_t{12});
    CHECK_EQ(variants.intAt(0, "retail_price_cents"), std::int64_t{0});  // «не оценён»
    CHECK_EQ(variants.textAt(0, "state_hash").size(), std::size_t{64});
    CHECK_EQ(variants.intAt(0, "updated_at"), std::int64_t{5000});
    CHECK_EQ(Json::parse(variants.textAt(0, "attributes_json"))["color"].get<std::string>(), std::string("Black"));

    // Стартовые точки истории создаются триггером.
    CHECK_EQ(count(*db, "SELECT COUNT(*) FROM price_history"), 2);

    const auto task = unwrap(db->query("SELECT * FROM task_queue"));
    REQUIRE(task.rows.size() == 1);
    CHECK_EQ(task.textAt(0, "task_type"), std::string("AI_REWRITE"));
    CHECK_EQ(Json::parse(task.textAt(0, "payload_json"))["product_id"].get<std::int64_t>(), out.productId);
    CHECK_EQ(task.intAt(0, "scheduled_at"), std::int64_t{5000});
}

TEST(import_does_not_create_the_same_product_twice) {
    TempDir dir;
    auto db = openDb(dir);
    addStore(*db);

    const auto first = imported(*db, parsedOk(validPayload()));
    // Та же страница с другим трекингом и регистром хоста.
    Json again = with(validPayload(), "url", "https://SHOP.example/p/1/?color=red&utm_campaign=z#x");
    const auto second = imported(*db, parsedOk(again), 6000);

    CHECK(second.duplicate);
    CHECK_EQ(second.productId, first.productId);
    CHECK_EQ(count(*db, "SELECT COUNT(*) FROM products"), 1);
    CHECK_EQ(count(*db, "SELECT COUNT(*) FROM product_variants"), 2);
    CHECK_EQ(count(*db, "SELECT COUNT(*) FROM task_queue"), 1);
}

TEST(import_uses_first_active_store_by_default_and_honors_store_id) {
    TempDir dir;
    auto db = openDb(dir);
    addStore(*db, false, "disabled");
    const auto active1 = addStore(*db, true, "A");
    const auto active2 = addStore(*db, true, "B");

    const auto a = imported(*db, parsedOk(validPayload()));
    CHECK_EQ(count(*db, "SELECT store_id FROM products WHERE id = ?", {a.productId}), active1);

    Json explicitStore = with(with(validPayload(), "url", "https://shop.example/p/2"), "store_id", active2);
    const auto b = imported(*db, parsedOk(explicitStore));
    CHECK_EQ(count(*db, "SELECT store_id FROM products WHERE id = ?", {b.productId}), active2);
}

TEST(import_reports_missing_or_unusable_stores) {
    TempDir dir;
    auto db = openDb(dir);
    CHECK_EQ(errorCode(*db, parsedOk(validPayload())), bridge::rpc_code::kNoStore);

    const auto inactive = addStore(*db, false);
    CHECK_EQ(errorCode(*db, parsedOk(validPayload())), bridge::rpc_code::kNoStore);
    CHECK_EQ(errorCode(*db, parsedOk(with(validPayload(), "store_id", inactive))), bridge::rpc_code::kStoreNotFound);
    CHECK_EQ(errorCode(*db, parsedOk(with(validPayload(), "store_id", 999))), bridge::rpc_code::kStoreNotFound);
    CHECK_EQ(count(*db, "SELECT COUNT(*) FROM products"), 0);
    CHECK_EQ(count(*db, "SELECT COUNT(*) FROM task_queue"), 0);
}
