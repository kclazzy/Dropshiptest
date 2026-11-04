-- Схема из ТЗ. PRAGMA (WAL, synchronous, foreign_keys, busy_timeout) задаются при открытии соединения
-- (db::init_pool): внутри транзакции миграции journal_mode менять нельзя.

CREATE TABLE stores (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    platform_type TEXT NOT NULL, -- 'ozon', 'avito', 'shopify', 'woocommerce', 'ebay'
    api_url TEXT NOT NULL,
    encrypted_credentials BLOB NOT NULL, -- AES-256-GCM (Client-Id/Api-Key, OAuth secrets)
    warehouse_id TEXT, -- ID склада для Ozon FBS/rFBS или адрес для Avito
    currency_code TEXT NOT NULL DEFAULT 'RUB',
    rate_limit_rps REAL NOT NULL DEFAULT 2.0,
    is_active INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL
);

CREATE TABLE pricing_rules (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    store_id INTEGER NOT NULL REFERENCES stores(id) ON DELETE CASCADE,
    rule_name TEXT NOT NULL DEFAULT 'Стандартное правило',
    price_from_minor INTEGER NOT NULL,
    price_to_minor INTEGER NOT NULL,
    margin_percent REAL NOT NULL,
    fixed_markup_minor INTEGER NOT NULL DEFAULT 0,
    marketplace_fee_percent REAL NOT NULL DEFAULT 15.0,
    acquiring_and_tax_percent REAL NOT NULL DEFAULT 6.0,
    discount_striken_percent REAL NOT NULL DEFAULT 30.0,
    rounding_minor INTEGER NOT NULL DEFAULT 0,
    min_stock_threshold INTEGER NOT NULL DEFAULT 5,
    min_profit_minor INTEGER NOT NULL DEFAULT 30000
);

CREATE TABLE proxies (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    proxy_url TEXT NOT NULL UNIQUE,
    fail_count INTEGER NOT NULL DEFAULT 0,
    last_used_at INTEGER NOT NULL DEFAULT 0,
    cooldown_until INTEGER NOT NULL DEFAULT 0,
    is_active INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE products (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    store_id INTEGER NOT NULL REFERENCES stores(id) ON DELETE CASCADE,
    external_store_id TEXT,
    external_offer_id TEXT NOT NULL,
    supplier_url TEXT NOT NULL,
    supplier_product_id TEXT NOT NULL,
    url_sha256 TEXT NOT NULL UNIQUE,
    raw_title TEXT NOT NULL,
    raw_description TEXT,
    ai_seo_title TEXT,
    ai_avito_title TEXT,
    ai_description TEXT,
    ai_attributes_json TEXT,
    ai_tags_json TEXT,
    images_json TEXT NOT NULL,
    marketplace_category_id TEXT,
    package_dimensions_json TEXT NOT NULL DEFAULT '{"weight_g":500,"length_mm":200,"width_mm":150,"height_mm":100}',
    ai_status TEXT NOT NULL DEFAULT 'PENDING',
    monitor_status TEXT NOT NULL DEFAULT 'ACTIVE',
    check_interval_sec INTEGER NOT NULL DEFAULT 1800,
    last_checked_at INTEGER NOT NULL DEFAULT 0,
    next_check_at INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX idx_products_next_check ON products(monitor_status, next_check_at);
CREATE INDEX idx_products_ai_status ON products(ai_status);

CREATE TABLE product_variants (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
    external_variant_id TEXT,
    supplier_sku TEXT NOT NULL,
    attributes_json TEXT NOT NULL,
    supplier_price_minor INTEGER NOT NULL,
    shipping_price_minor INTEGER NOT NULL DEFAULT 0,
    retail_price_minor INTEGER NOT NULL,
    min_price_minor INTEGER NOT NULL,
    old_price_minor INTEGER NOT NULL,
    supplier_stock INTEGER NOT NULL,
    is_in_stock INTEGER NOT NULL DEFAULT 1,
    state_hash TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE orders (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    store_id INTEGER NOT NULL REFERENCES stores(id) ON DELETE CASCADE,
    external_order_id TEXT NOT NULL UNIQUE,
    variant_id INTEGER REFERENCES product_variants(id),
    quantity INTEGER NOT NULL,
    delivery_scheme TEXT NOT NULL DEFAULT 'rFBS',
    buyer_address_json TEXT NOT NULL,
    retail_total_minor INTEGER NOT NULL,
    supplier_cost_minor INTEGER,
    net_profit_minor INTEGER,
    status TEXT NOT NULL DEFAULT 'NEW',
    supplier_order_id TEXT,
    tracking_number TEXT,
    tracking_carrier TEXT,
    human_readable_hint TEXT,
    error_message TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE task_queue (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_type TEXT NOT NULL,
    priority INTEGER NOT NULL DEFAULT 5,
    payload_json TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'PENDING',
    attempts INTEGER NOT NULL DEFAULT 0,
    max_attempts INTEGER NOT NULL DEFAULT 3,
    scheduled_at INTEGER NOT NULL,
    locked_at INTEGER NOT NULL DEFAULT 0,
    error_log TEXT
);

CREATE INDEX idx_task_queue_poll ON task_queue(status, priority ASC, scheduled_at ASC);
