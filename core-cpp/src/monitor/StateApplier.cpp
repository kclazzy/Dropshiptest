#include "monitor/StateApplier.hpp"

#include <algorithm>
#include <map>

#include "core/Json.hpp"
#include "importer/ProductImport.hpp"
#include "pricing/PricingEngine.hpp"

namespace dropship::monitor {

namespace {

using db::Tx;

struct StoredVariant {
    std::int64_t id = 0;
    std::string sku;
    Cents price = 0;
    Cents shipping = 0;
    std::int64_t stock = 0;
    std::string hash;
};

// Правила магазина. Повреждённые строки не должны ломать мониторинг: набор считается пустым,
// цена остаётся прежней (это видно по счётчику `unpriced`).
std::optional<pricing::RuleSet> loadRules(Tx& tx, std::int64_t storeId) {
    const auto rows = tx.query(
        "SELECT price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, marketplace_fee_percent, "
        "rounding_cents, min_stock_threshold, min_profit_cents FROM pricing_rules WHERE store_id = ?",
        {storeId});
    if (!rows || rows->rows.empty()) return std::nullopt;
    std::vector<pricing::PricingRule> rules;
    for (std::size_t i = 0; i < rows->rows.size(); ++i) {
        pricing::RuleSpec spec;
        spec.priceFromCents = rows->intAt(i, "price_from_cents");
        spec.priceToCents = rows->intAt(i, "price_to_cents");
        spec.marginPercent = std::get<double>(rows->at(i, "margin_percent"));
        spec.fixedMarkupCents = rows->intAt(i, "fixed_markup_cents");
        const auto& fee = rows->at(i, "marketplace_fee_percent");
        spec.marketplaceFeePercent = std::holds_alternative<double>(fee) ? std::get<double>(fee) : 0.0;
        if (!rows->isNull(i, "rounding_cents")) spec.roundingCents = static_cast<int>(rows->intAt(i, "rounding_cents"));
        else spec.roundingCents = std::nullopt;
        spec.minStockThreshold = rows->intAt(i, "min_stock_threshold");
        spec.minProfitCents = rows->intAt(i, "min_profit_cents");
        auto rule = pricing::PricingRule::fromSpec(spec);
        if (!rule) return std::nullopt;
        rules.push_back(*rule);
    }
    auto set = pricing::RuleSet::create(std::move(rules));
    if (!set) return std::nullopt;
    return std::move(*set);
}

}  // namespace

db::Result<ApplyOutcome> applyObservation(db::DatabaseManager& database, std::int64_t productId,
                                          const std::vector<ObservedVariant>& observed, UnixSeconds now) {
    ApplyOutcome outcome;
    const auto done = database.transaction([&](Tx& tx) -> db::Result<void> {
        const auto product = tx.query("SELECT store_id, check_interval_sec FROM products WHERE id = ?", {productId});
        if (!product) return unexpected(product.error());
        if (product->rows.empty()) return unexpected(db::DbError{0, "product not found"});
        const std::int64_t storeId = product->intAt(0, "store_id");
        const std::int64_t interval = std::max<std::int64_t>(product->intAt(0, "check_interval_sec"), 60);

        const auto stored = tx.query(
            "SELECT id, supplier_sku, supplier_price_cents, COALESCE(shipping_price_cents, 0) AS shipping, "
            "supplier_stock, state_hash FROM product_variants WHERE product_id = ? ORDER BY id",
            {productId});
        if (!stored) return unexpected(stored.error());
        std::vector<StoredVariant> variants;
        for (std::size_t i = 0; i < stored->rows.size(); ++i) {
            variants.push_back({stored->intAt(i, "id"), stored->textAt(i, "supplier_sku"),
                                stored->intAt(i, "supplier_price_cents"), stored->intAt(i, "shipping"),
                                stored->intAt(i, "supplier_stock"), stored->textAt(i, "state_hash")});
        }
        const auto rules = loadRules(tx, storeId);

        std::vector<bool> seen(variants.size(), false);
        const auto applyTo = [&](std::size_t index, const ObservedVariant& obs) -> db::Result<void> {
            seen[index] = true;
            const StoredVariant& current = variants[index];
            const std::string hash = importer::variantStateHash(obs.priceCents, current.shipping, obs.stock);
            if (hash == current.hash) {
                ++outcome.unchanged;
                return {};
            }
            const pricing::PricingRule* rule = rules ? rules->find(obs.priceCents) : nullptr;
            std::optional<Cents> retail;
            if (rule != nullptr) {
                if (const auto q = pricing::quote(*rule, {obs.priceCents, current.shipping})) retail = q->retailCents;
            }
            if (!retail) ++outcome.unpriced;
            const std::int64_t sellable = rule != nullptr ? pricing::stockToPublish(*rule, obs.stock) : obs.stock;
            if (sellable == 0) ++outcome.soldOut;
            ++outcome.changed;
            const auto updated = tx.execute(
                "UPDATE product_variants SET supplier_price_cents = ?, supplier_stock = ?, is_in_stock = ?, "
                "retail_price_cents = COALESCE(?, retail_price_cents), state_hash = ?, updated_at = ? WHERE id = ?",
                {obs.priceCents, obs.stock, std::int64_t{sellable > 0 ? 1 : 0},
                 retail ? db::Value{*retail} : db::Value{std::monostate{}}, hash, now, current.id});
            if (!updated) return unexpected(updated.error());
            return {};
        };

        for (const auto& obs : observed) {
            std::optional<std::size_t> match;
            if (!obs.sku.empty()) {
                for (std::size_t i = 0; i < variants.size(); ++i) {
                    if (!seen[i] && variants[i].sku == obs.sku) { match = i; break; }
                }
            } else if (variants.size() == 1 && observed.size() == 1) {
                match = 0;  // единственный безымянный вариант страницы = единственный сохранённый
            } else {
                continue;  // неоднозначно: не угадываем, к какому варианту относится цена
            }
            if (match) {
                if (auto r = applyTo(*match, obs); !r) return r;
                continue;
            }
            const std::string hash = importer::variantStateHash(obs.priceCents, 0, obs.stock);
            const auto inserted = tx.execute(
                "INSERT INTO product_variants(product_id, supplier_sku, attributes_json, supplier_price_cents, "
                "retail_price_cents, supplier_stock, is_in_stock, state_hash, updated_at) VALUES(?, ?, '{}', ?, 0, ?, ?, ?, ?)",
                {productId, obs.sku, obs.priceCents, obs.stock, std::int64_t{obs.stock > 0 ? 1 : 0}, hash, now});
            if (!inserted) return unexpected(inserted.error());
            ++outcome.added;
        }

        // Варианты, которых на странице больше нет, — распроданы (только если страница вообще что-то показала
        // по именованным вариантам; безымянный/неоднозначный разбор их не трогает).
        const bool pageIsAuthoritative = std::any_of(observed.begin(), observed.end(), [](const auto& o) { return !o.sku.empty(); });
        if (pageIsAuthoritative) {
            for (std::size_t i = 0; i < variants.size(); ++i) {
                if (seen[i] || variants[i].stock == 0) continue;
                const std::string hash = importer::variantStateHash(variants[i].price, variants[i].shipping, 0);
                const auto gone = tx.execute(
                    "UPDATE product_variants SET supplier_stock = 0, is_in_stock = 0, state_hash = ?, updated_at = ? WHERE id = ?",
                    {hash, now, variants[i].id});
                if (!gone) return unexpected(gone.error());
                ++outcome.soldOut;
            }
        }

        const auto touched = tx.execute(
            "UPDATE products SET last_checked_at = ?, next_check_at = ?, monitor_status = 'ACTIVE' WHERE id = ?",
            {now, now + interval, productId});
        if (!touched) return unexpected(touched.error());
        return {};
    });
    if (!done) return unexpected(done.error());
    return outcome;
}

db::Result<void> markProductUnavailable(db::DatabaseManager& database, std::int64_t productId, UnixSeconds now) {
    return database.transaction([&](Tx& tx) -> db::Result<void> {
        const auto rows = tx.query(
            "SELECT id, supplier_price_cents, COALESCE(shipping_price_cents, 0) AS shipping FROM product_variants WHERE product_id = ?",
            {productId});
        if (!rows) return unexpected(rows.error());
        for (std::size_t i = 0; i < rows->rows.size(); ++i) {
            const std::string hash =
                importer::variantStateHash(rows->intAt(i, "supplier_price_cents"), rows->intAt(i, "shipping"), 0);
            const auto r = tx.execute(
                "UPDATE product_variants SET supplier_stock = 0, is_in_stock = 0, state_hash = ?, updated_at = ? WHERE id = ?",
                {hash, now, rows->intAt(i, "id")});
            if (!r) return unexpected(r.error());
        }
        const auto paused = tx.execute("UPDATE products SET monitor_status = 'PAUSED', last_checked_at = ? WHERE id = ?",
                                       {now, productId});
        if (!paused) return unexpected(paused.error());
        return {};
    });
}

db::Result<int> resumeBlockedProducts(db::DatabaseManager& database, const std::string& domain) {
    if (domain.empty()) return 0;
    // LIKE-символы в домене экранируются: «_» и «%» не должны работать как шаблон.
    std::string escaped;
    for (const char c : domain) {
        if (c == '%' || c == '_' || c == '\\') escaped += '\\';
        escaped += c;
    }
    const auto r = database.execute(
        "UPDATE products SET monitor_status = 'ACTIVE', next_check_at = 0 WHERE monitor_status = 'CAPTCHA_BLOCKED' AND ("
        "supplier_url LIKE ? ESCAPE '\\' OR supplier_url LIKE ? ESCAPE '\\' OR "
        "supplier_url LIKE ? ESCAPE '\\' OR supplier_url LIKE ? ESCAPE '\\')",
        {"https://" + escaped + "/%", "http://" + escaped + "/%", "https://%." + escaped + "/%", "http://%." + escaped + "/%"});
    if (!r) return unexpected(r.error());
    return static_cast<int>(r->changes);
}

db::Result<int> scheduleDueChecks(db::DatabaseManager& database, UnixSeconds now, int limit) {
    int queued = 0;
    const auto done = database.transaction([&](Tx& tx) -> db::Result<void> {
        const auto due = tx.query(
            "SELECT p.id FROM products p WHERE p.monitor_status = 'ACTIVE' AND p.next_check_at <= ? "
            "AND NOT EXISTS (SELECT 1 FROM task_queue t WHERE t.task_type = 'PRICE_SYNC' "
            "                AND t.status IN ('PENDING', 'IN_PROGRESS') "
            "                AND json_extract(t.payload_json, '$.product_id') = p.id) "
            "ORDER BY p.next_check_at ASC, p.id ASC LIMIT ?",
            {now, std::int64_t{limit}});
        if (!due) return unexpected(due.error());
        for (std::size_t i = 0; i < due->rows.size(); ++i) {
            const auto id = due->intAt(i, "id");
            const auto r = db::DatabaseManager::enqueueTaskIn(tx, TaskType::PriceSync, 5,
                                                              dumpJson(Json{{"product_id", id}}), now, 3);
            if (!r) return unexpected(r.error());
            ++queued;
        }
        return {};
    });
    if (!done) return unexpected(done.error());
    return queued;
}

}  // namespace dropship::monitor
