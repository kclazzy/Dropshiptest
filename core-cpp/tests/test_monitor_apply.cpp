#include "TestDb.hpp"
#include "importer/ProductImport.hpp"
#include "monitor/StateApplier.hpp"
#include "pricing/PricingEngine.hpp"

using namespace testdb;
using namespace dropship::monitor;
namespace pricing = dropship::pricing;

namespace {

template <class T>
T unwrap2(pricing::Result<T> r) {
    if (!r) throw std::runtime_error("pricing error in test");
    return std::move(*r);
}

void addRule(DatabaseManager& db, std::int64_t storeId, std::int64_t from = 0, std::int64_t margin = 30,
             std::int64_t minStock = 5) {
    unwrap(db.execute(
        "INSERT INTO pricing_rules(store_id, price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, "
        "marketplace_fee_percent, rounding_cents, min_stock_threshold, min_profit_cents) "
        "VALUES(?, ?, ?, ?, 0, 0, 99, ?, 0)",
        {storeId, from, std::numeric_limits<std::int64_t>::max(), static_cast<double>(margin), minStock}));
}

Cents expectedRetail(Cents supplier, std::int64_t margin = 30) {
    pricing::RuleSpec spec;
    spec.marginPercent = static_cast<double>(margin);
    spec.roundingCents = 99;
    spec.minProfitCents = 0;
    return unwrap2(pricing::quote(unwrap2(pricing::PricingRule::fromSpec(spec)), {supplier, 0})).retailCents;
}

std::int64_t col(DatabaseManager& db, const std::string& sql, std::int64_t id) {
    return unwrap(db.query(sql, {id})).intAt(0, "v");
}

}  // namespace

TEST(apply_changed_price_updates_variant_retail_and_history) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    addRule(*db, f.storeId);
    const auto before = scalarInt(*db, "SELECT COUNT(*) FROM price_history");

    const auto r = unwrap(applyObservation(*db, f.productId, {{"SKU-1", 12000, 10}}, 5000));
    CHECK_EQ(r.changed, 1);
    CHECK_EQ(col(*db, "SELECT supplier_price_cents AS v FROM product_variants WHERE id = ?", f.variantId), std::int64_t{12000});
    CHECK_EQ(col(*db, "SELECT retail_price_cents AS v FROM product_variants WHERE id = ?", f.variantId),
             std::int64_t{expectedRetail(12000)});
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM price_history"), before + 1);
    const auto product = unwrap(db->query("SELECT last_checked_at, next_check_at, monitor_status FROM products"));
    CHECK_EQ(product.intAt(0, "last_checked_at"), std::int64_t{5000});
    CHECK_EQ(product.intAt(0, "next_check_at"), std::int64_t{5000 + 1800});
    CHECK_EQ(product.textAt(0, "monitor_status"), std::string("ACTIVE"));
}

TEST(apply_same_state_is_a_no_op_for_variants_and_history) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    addRule(*db, f.storeId);
    unwrap(applyObservation(*db, f.productId, {{"SKU-1", 12000, 10}}, 100));
    const auto history = scalarInt(*db, "SELECT COUNT(*) FROM price_history");

    const auto r = unwrap(applyObservation(*db, f.productId, {{"SKU-1", 12000, 10}}, 200));
    CHECK_EQ(r.unchanged, 1);
    CHECK_EQ(r.changed, 0);
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM price_history"), history);
    CHECK_EQ(scalarInt(*db, "SELECT last_checked_at FROM products"), std::int64_t{200});  // проверка всё же учтена
}

TEST(apply_low_stock_is_protected_and_marks_variant_unsellable) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    addRule(*db, f.storeId, 0, 30, /*minStock=*/5);
    const auto r = unwrap(applyObservation(*db, f.productId, {{"SKU-1", 10000, 3}}, 100));
    CHECK_EQ(r.soldOut, 1);
    CHECK_EQ(col(*db, "SELECT is_in_stock AS v FROM product_variants WHERE id = ?", f.variantId), std::int64_t{0});
    CHECK_EQ(col(*db, "SELECT supplier_stock AS v FROM product_variants WHERE id = ?", f.variantId), std::int64_t{3});
}

TEST(apply_without_a_matching_rule_keeps_the_retail_price) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);  // правил нет
    const auto r = unwrap(applyObservation(*db, f.productId, {{"SKU-1", 12000, 10}}, 100));
    CHECK_EQ(r.unpriced, 1);
    CHECK_EQ(col(*db, "SELECT retail_price_cents AS v FROM product_variants WHERE id = ?", f.variantId), std::int64_t{14499});
    CHECK_EQ(col(*db, "SELECT supplier_price_cents AS v FROM product_variants WHERE id = ?", f.variantId), std::int64_t{12000});
}

TEST(apply_variant_missing_from_the_page_is_sold_out_and_new_one_is_added) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    addRule(*db, f.storeId);
    const auto r = unwrap(applyObservation(*db, f.productId, {{"NEW-SKU", 5000, 7}}, 100));
    CHECK_EQ(r.added, 1);
    CHECK_EQ(r.soldOut, 1);
    CHECK_EQ(col(*db, "SELECT supplier_stock AS v FROM product_variants WHERE id = ?", f.variantId), std::int64_t{0});
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM product_variants WHERE supplier_sku = 'NEW-SKU'"), 1);
}

TEST(apply_single_unnamed_observation_matches_the_only_variant) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    const auto r = unwrap(applyObservation(*db, f.productId, {{"", 12000, 10}}, 100));
    CHECK_EQ(r.changed, 1);
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM product_variants"), 1);
}

TEST(apply_ambiguous_unnamed_observation_changes_no_variants) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    unwrap(db->execute(
        "INSERT INTO product_variants(product_id, supplier_sku, attributes_json, supplier_price_cents, retail_price_cents, "
        "supplier_stock, state_hash, updated_at) VALUES(?, 'SKU-2', '{}', 1, 1, 1, 'h', 1)", {f.productId}));
    const auto r = unwrap(applyObservation(*db, f.productId, {{"", 12000, 10}}, 100));
    CHECK_EQ(r.changed + r.added + r.soldOut, 0);
    CHECK_EQ(col(*db, "SELECT supplier_price_cents AS v FROM product_variants WHERE id = ?", f.variantId), std::int64_t{10000});
}

TEST(apply_rejects_unknown_product_and_leaves_database_unchanged) {
    TempDir dir;
    auto db = openDb(dir);
    CHECK(!applyObservation(*db, 9999, {{"x", 1, 1}}, 100).has_value());
}

TEST(mark_unavailable_zeroes_stock_and_pauses_monitoring) {
    TempDir dir;
    auto db = openDb(dir);
    const auto f = insertFixture(*db);
    unwrap(markProductUnavailable(*db, f.productId, 100));
    CHECK_EQ(col(*db, "SELECT supplier_stock AS v FROM product_variants WHERE id = ?", f.variantId), std::int64_t{0});
    CHECK_EQ(unwrap(db->query("SELECT monitor_status FROM products")).textAt(0, "monitor_status"), std::string("PAUSED"));
}

TEST(scheduler_enqueues_due_active_products_once) {
    TempDir dir;
    auto db = openDb(dir);
    const auto a = insertFixture(*db, "sha-a");
    const auto b = insertFixture(*db, "sha-b");
    const auto c = insertFixture(*db, "sha-c");
    unwrap(db->execute("UPDATE products SET next_check_at = 100 WHERE id = ?", {a.productId}));
    unwrap(db->execute("UPDATE products SET next_check_at = 9999 WHERE id = ?", {b.productId}));        // ещё рано
    unwrap(db->execute("UPDATE products SET next_check_at = 100, monitor_status = 'PAUSED' WHERE id = ?", {c.productId}));

    CHECK_EQ(unwrap(scheduleDueChecks(*db, 500)), 1);
    CHECK_EQ(unwrap(scheduleDueChecks(*db, 500)), 0);  // повтор не дублирует задачу
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM task_queue WHERE task_type = 'PRICE_SYNC'"), 1);
    const auto payload = unwrap(db->query("SELECT payload_json FROM task_queue")).textAt(0, "payload_json");
    CHECK(payload.find("\"product_id\":" + std::to_string(a.productId)) != std::string::npos);
}

TEST(scheduler_respects_the_limit_and_schedules_again_after_completion) {
    TempDir dir;
    auto db = openDb(dir);
    for (int i = 0; i < 3; ++i) insertFixture(*db, "sha-" + std::to_string(i));
    CHECK_EQ(unwrap(scheduleDueChecks(*db, 500, 2)), 2);
    CHECK_EQ(unwrap(scheduleDueChecks(*db, 500, 2)), 1);  // остальные, не те, что уже в очереди
    unwrap(db->execute("UPDATE task_queue SET status = 'COMPLETED'"));
    CHECK_EQ(unwrap(scheduleDueChecks(*db, 500)), 3);
}

TEST(resume_returns_blocked_products_of_that_domain_only) {
    TempDir dir;
    auto db = openDb(dir);
    const auto a = insertFixture(*db, "sha-a");  // https://supplier.example/p/1
    const auto b = insertFixture(*db, "sha-b");
    const auto c = insertFixture(*db, "sha-c");
    const auto d = insertFixture(*db, "sha-d");
    unwrap(db->execute("UPDATE products SET monitor_status = 'CAPTCHA_BLOCKED', next_check_at = 9999"));
    unwrap(db->execute("UPDATE products SET supplier_url = 'https://shop.supplier.example/x' WHERE id = ?", {b.productId}));
    unwrap(db->execute("UPDATE products SET supplier_url = 'https://notsupplier.example/x' WHERE id = ?", {c.productId}));
    unwrap(db->execute("UPDATE products SET supplier_url = 'https://other.org/x' WHERE id = ?", {d.productId}));

    CHECK_EQ(unwrap(resumeBlockedProducts(*db, "supplier.example")), 2);  // a и поддомен b; не «notsupplier» и не чужой
    CHECK_EQ(scalarInt(*db, "SELECT COUNT(*) FROM products WHERE monitor_status = 'ACTIVE' AND next_check_at = 0"), 2);
    CHECK_EQ(unwrap(resumeBlockedProducts(*db, "supplier.example")), 0);
    CHECK_EQ(unwrap(resumeBlockedProducts(*db, "")), 0);
    CHECK_EQ(unwrap(resumeBlockedProducts(*db, "%")), 0);  // шаблонные символы не работают как шаблон
    (void)a;
}
