#include "db/Migrations.hpp"

#include <iterator>
#include <string_view>

namespace dropship::db {

namespace {

struct Migration {
    int version;
    std::string_view sql;
};

// v1 — схема из технического задания без изменений (PRAGMA выставляются на каждом соединении).
constexpr std::string_view kV1CoreSchema = R"SQL(
CREATE TABLE stores (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    platform_type TEXT NOT NULL, -- см. dropship::Platform: 'shopify', 'ozon', 'avito', ...
    api_url TEXT NOT NULL,
    encrypted_credentials BLOB NOT NULL, -- AES-256-GCM
    rate_limit_rps REAL DEFAULT 2.0,
    is_active INTEGER DEFAULT 1,
    created_at INTEGER NOT NULL
);

CREATE TABLE pricing_rules (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    store_id INTEGER NOT NULL REFERENCES stores(id) ON DELETE CASCADE,
    price_from_cents INTEGER NOT NULL,
    price_to_cents INTEGER NOT NULL,
    margin_percent REAL NOT NULL,
    fixed_markup_cents INTEGER DEFAULT 0,
    marketplace_fee_percent REAL DEFAULT 0.0,
    rounding_cents INTEGER DEFAULT 99, -- NULL: без психологического округления
    min_stock_threshold INTEGER DEFAULT 5,
    min_profit_cents INTEGER DEFAULT 200
);

CREATE TABLE proxies (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    proxy_url TEXT NOT NULL UNIQUE, -- socks5://user:pass@ip:port
    fail_count INTEGER DEFAULT 0,
    last_used_at INTEGER DEFAULT 0,
    cooldown_until INTEGER DEFAULT 0,
    is_active INTEGER DEFAULT 1
);

CREATE TABLE products (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    store_id INTEGER NOT NULL REFERENCES stores(id) ON DELETE CASCADE,
    external_store_id TEXT,
    supplier_url TEXT NOT NULL,
    supplier_product_id TEXT NOT NULL,
    url_sha256 TEXT NOT NULL UNIQUE,
    raw_title TEXT NOT NULL,
    raw_description TEXT,
    ai_seo_title TEXT,
    ai_html_description TEXT,
    ai_tags_json TEXT,
    images_json TEXT NOT NULL,
    ai_status TEXT DEFAULT 'PENDING', -- 'PENDING', 'PROCESSING', 'READY', 'ERROR'
    monitor_status TEXT DEFAULT 'ACTIVE', -- 'ACTIVE', 'PAUSED', 'CAPTCHA_BLOCKED', 'ERROR'
    check_interval_sec INTEGER DEFAULT 1800,
    last_checked_at INTEGER DEFAULT 0,
    next_check_at INTEGER DEFAULT 0
);

CREATE INDEX idx_products_next_check ON products(monitor_status, next_check_at);
CREATE INDEX idx_products_ai_status ON products(ai_status);

CREATE TABLE product_variants (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
    external_variant_id TEXT,
    supplier_sku TEXT NOT NULL,
    attributes_json TEXT NOT NULL, -- {"color": "Black", "size": "XL"}
    supplier_price_cents INTEGER NOT NULL,
    shipping_price_cents INTEGER DEFAULT 0,
    retail_price_cents INTEGER NOT NULL,
    supplier_stock INTEGER NOT NULL,
    is_in_stock INTEGER DEFAULT 1,
    state_hash TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE orders (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    store_id INTEGER NOT NULL REFERENCES stores(id) ON DELETE CASCADE,
    external_order_id TEXT NOT NULL UNIQUE,
    variant_id INTEGER REFERENCES product_variants(id),
    quantity INTEGER NOT NULL,
    buyer_address_json TEXT NOT NULL,
    retail_total_cents INTEGER NOT NULL,
    supplier_cost_cents INTEGER,
    net_profit_cents INTEGER,
    status TEXT DEFAULT 'NEW', -- 'NEW', 'QUEUED_FOR_EXT', 'ORDERED', 'SHIPPED', 'MANUAL_REVIEW', 'ERROR'
    supplier_order_id TEXT,
    tracking_number TEXT,
    tracking_carrier TEXT,
    error_message TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE task_queue (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_type TEXT NOT NULL, -- см. dropship::TaskType
    priority INTEGER DEFAULT 5, -- 1 (высший) .. 10 (низший)
    payload_json TEXT NOT NULL,
    status TEXT DEFAULT 'PENDING', -- 'PENDING', 'IN_PROGRESS', 'COMPLETED', 'FAILED'
    attempts INTEGER DEFAULT 0,
    max_attempts INTEGER DEFAULT 3,
    scheduled_at INTEGER NOT NULL,
    locked_at INTEGER DEFAULT 0,
    error_log TEXT
);

CREATE INDEX idx_task_queue_poll ON task_queue(status, priority ASC, scheduled_at ASC);
)SQL";

// v2 — редактирование карточек пользователем, история цен/остатков, аудит правок,
// валюта магазина (RUB для Ozon/Avito) и представление для списка товаров в GUI.
// История и аудит пишутся триггерами: код приложения не может «забыть» записать её.
constexpr std::string_view kV2HistoryAndEditing = R"SQL(
ALTER TABLE stores ADD COLUMN currency TEXT NOT NULL DEFAULT 'USD'; -- ISO 4217: 'RUB', 'USD', ...
ALTER TABLE products ADD COLUMN title_override TEXT;
ALTER TABLE products ADD COLUMN description_override TEXT;
ALTER TABLE products ADD COLUMN user_notes TEXT;
ALTER TABLE products ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;

CREATE TABLE price_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    variant_id INTEGER NOT NULL REFERENCES product_variants(id) ON DELETE CASCADE,
    supplier_price_cents INTEGER NOT NULL,
    shipping_price_cents INTEGER NOT NULL,
    retail_price_cents INTEGER NOT NULL,
    supplier_stock INTEGER NOT NULL,
    recorded_at INTEGER NOT NULL
);
CREATE INDEX idx_price_history_variant ON price_history(variant_id, recorded_at);

CREATE TRIGGER trg_variants_history_insert AFTER INSERT ON product_variants
BEGIN
    INSERT INTO price_history(variant_id, supplier_price_cents, shipping_price_cents,
                              retail_price_cents, supplier_stock, recorded_at)
    VALUES (NEW.id, NEW.supplier_price_cents, COALESCE(NEW.shipping_price_cents, 0),
            NEW.retail_price_cents, NEW.supplier_stock, CAST(strftime('%s', 'now') AS INTEGER));
END;

CREATE TRIGGER trg_variants_history_update AFTER UPDATE ON product_variants
WHEN OLD.supplier_price_cents IS NOT NEW.supplier_price_cents
  OR OLD.shipping_price_cents IS NOT NEW.shipping_price_cents
  OR OLD.retail_price_cents IS NOT NEW.retail_price_cents
  OR OLD.supplier_stock IS NOT NEW.supplier_stock
BEGIN
    INSERT INTO price_history(variant_id, supplier_price_cents, shipping_price_cents,
                              retail_price_cents, supplier_stock, recorded_at)
    VALUES (NEW.id, NEW.supplier_price_cents, COALESCE(NEW.shipping_price_cents, 0),
            NEW.retail_price_cents, NEW.supplier_stock, CAST(strftime('%s', 'now') AS INTEGER));
END;

CREATE TABLE audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_type TEXT NOT NULL, -- 'product' | 'order'
    entity_id INTEGER NOT NULL,
    field TEXT NOT NULL,
    old_value TEXT,
    new_value TEXT,
    changed_at INTEGER NOT NULL
);
CREATE INDEX idx_audit_entity ON audit_log(entity_type, entity_id, changed_at);

CREATE TRIGGER trg_products_audit AFTER UPDATE ON products
BEGIN
    INSERT INTO audit_log(entity_type, entity_id, field, old_value, new_value, changed_at)
    SELECT 'product', NEW.id, f.field, f.old_value, f.new_value, CAST(strftime('%s', 'now') AS INTEGER)
    FROM (SELECT 'title_override' AS field, OLD.title_override AS old_value, NEW.title_override AS new_value
          UNION ALL SELECT 'description_override', OLD.description_override, NEW.description_override
          UNION ALL SELECT 'user_notes', OLD.user_notes, NEW.user_notes
          UNION ALL SELECT 'monitor_status', OLD.monitor_status, NEW.monitor_status) AS f
    WHERE f.old_value IS NOT f.new_value;
END;

CREATE TRIGGER trg_orders_audit AFTER UPDATE ON orders
BEGIN
    INSERT INTO audit_log(entity_type, entity_id, field, old_value, new_value, changed_at)
    SELECT 'order', NEW.id, f.field, f.old_value, f.new_value, CAST(strftime('%s', 'now') AS INTEGER)
    FROM (SELECT 'status' AS field, OLD.status AS old_value, NEW.status AS new_value
          UNION ALL SELECT 'tracking_number', OLD.tracking_number, NEW.tracking_number
          UNION ALL SELECT 'supplier_order_id', OLD.supplier_order_id, NEW.supplier_order_id) AS f
    WHERE f.old_value IS NOT f.new_value;
END;

-- Единственное место, где определено «что показывать»: правка пользователя > ИИ > оригинал.
CREATE VIEW v_product_overview AS
SELECT p.id,
       p.store_id,
       s.name AS store_name,
       s.platform_type,
       s.currency,
       COALESCE(NULLIF(p.title_override, ''), NULLIF(p.ai_seo_title, ''), p.raw_title) AS title,
       COALESCE(NULLIF(p.description_override, ''), NULLIF(p.ai_html_description, ''), p.raw_description)
           AS description,
       p.supplier_url,
       p.monitor_status,
       p.ai_status,
       p.last_checked_at,
       p.user_notes,
       COUNT(v.id) AS variant_count,
       MIN(v.retail_price_cents) AS min_retail_price_cents,
       MAX(v.retail_price_cents) AS max_retail_price_cents,
       COALESCE(SUM(v.supplier_stock), 0) AS total_stock
FROM products p
JOIN stores s ON s.id = p.store_id
LEFT JOIN product_variants v ON v.product_id = p.id
GROUP BY p.id;
)SQL";

// v3: что уже отправлено на витрину (идемпотентная синхронизация) и последняя ошибка магазина.
constexpr std::string_view kV3StoreSync = R"SQL(
ALTER TABLE product_variants ADD COLUMN published_hash TEXT;
ALTER TABLE product_variants ADD COLUMN rejected_hash TEXT;  -- состояние, которое площадка отклонила: не долбим повторно
ALTER TABLE stores ADD COLUMN last_error TEXT;
CREATE INDEX idx_variants_external ON product_variants(external_variant_id);

-- Привязка к карточке на витрине изменилась (или исправлена): отправку нужно выполнить заново.
CREATE TRIGGER trg_variants_relink AFTER UPDATE OF external_variant_id ON product_variants
WHEN OLD.external_variant_id IS NOT NEW.external_variant_id
BEGIN
    UPDATE product_variants SET published_hash = NULL, rejected_hash = NULL WHERE id = NEW.id;
END;
)SQL";

constexpr Migration kMigrations[] = {
    {1, kV1CoreSchema},
    {2, kV2HistoryAndEditing},
    {3, kV3StoreSync},
};

static_assert(std::size(kMigrations) == kLatestSchemaVersion &&
                  kMigrations[std::size(kMigrations) - 1].version == kLatestSchemaVersion,
              "kLatestSchemaVersion must match the last migration");

Result<void> rollbackWith(Connection& writer, DbError error) {
    (void)writer.execute("ROLLBACK", {});
    return unexpected(std::move(error));
}

Result<void> applyOne(Connection& writer, const Migration& migration) {
    if (auto begun = writer.execute("BEGIN IMMEDIATE", {}); !begun) return unexpected(begun.error());

    const auto applied = writer.query("SELECT 1 FROM schema_migrations WHERE version = ?",
                                      {std::int64_t{migration.version}});
    if (!applied) return rollbackWith(writer, applied.error());
    const bool alreadyApplied = !applied->rows.empty();

    if (!alreadyApplied) {
        if (auto ran = writer.executeScript(std::string(migration.sql)); !ran) {
            return rollbackWith(writer, ran.error());
        }
        const auto recorded =
            writer.execute("INSERT INTO schema_migrations(version, applied_at) "
                           "VALUES(?, CAST(strftime('%s', 'now') AS INTEGER))",
                           {std::int64_t{migration.version}});
        if (!recorded) return rollbackWith(writer, recorded.error());
    }

    if (auto committed = writer.execute("COMMIT", {}); !committed) return rollbackWith(writer, committed.error());
    return {};
}

}  // namespace

Result<void> migrate(Connection& writer) {
    if (auto created = writer.executeScript(
            "CREATE TABLE IF NOT EXISTS schema_migrations("
            "version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)");
        !created) {
        return created;
    }
    for (const auto& migration : kMigrations) {
        if (auto applied = applyOne(writer, migration); !applied) return applied;
    }
    return {};
}

}  // namespace dropship::db
