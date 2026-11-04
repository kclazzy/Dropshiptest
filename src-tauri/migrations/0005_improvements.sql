-- Резервные поставщики варианта (ТЗ 4.1), аналитика (4.6), кэш категорий с полнотекстовым поиском (4.4), доработка заказов (4.5).
CREATE TABLE variant_suppliers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    variant_id INTEGER NOT NULL REFERENCES product_variants(id) ON DELETE CASCADE,
    priority INTEGER NOT NULL DEFAULT 1,
    supplier_url TEXT NOT NULL,
    supplier_sku TEXT NOT NULL,
    supplier_price_minor INTEGER NOT NULL,
    shipping_price_minor INTEGER NOT NULL DEFAULT 0,
    supplier_stock INTEGER NOT NULL DEFAULT 0,
    is_available INTEGER NOT NULL DEFAULT 1,
    last_checked_at INTEGER NOT NULL DEFAULT 0,
    UNIQUE(variant_id, priority)
);
CREATE INDEX idx_variant_suppliers_priority ON variant_suppliers(variant_id, priority ASC);

ALTER TABLE product_variants ADD COLUMN active_supplier_id INTEGER;

CREATE TABLE product_analytics (
    product_id INTEGER PRIMARY KEY REFERENCES products(id) ON DELETE CASCADE,
    views_count INTEGER NOT NULL DEFAULT 0,
    cart_adds_count INTEGER NOT NULL DEFAULT 0,
    orders_count INTEGER NOT NULL DEFAULT 0,
    reanimation_attempts INTEGER NOT NULL DEFAULT 0,
    last_synced_at INTEGER NOT NULL DEFAULT 0
);

ALTER TABLE products ADD COLUMN published_at INTEGER NOT NULL DEFAULT 0;

CREATE TABLE marketplace_categories_cache (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    platform_type TEXT NOT NULL,
    category_id TEXT NOT NULL,
    type_id TEXT NOT NULL DEFAULT '',
    full_path_name TEXT NOT NULL,
    required_attributes_json TEXT NOT NULL DEFAULT '[]',
    updated_at INTEGER NOT NULL,
    UNIQUE(platform_type, category_id, type_id)
);

-- Внешний индекс по таблице кэша: rowid совпадает с id строки кэша.
CREATE VIRTUAL TABLE marketplace_categories_fts USING fts5(full_path_name, content='marketplace_categories_cache', content_rowid='id', tokenize='unicode61');

CREATE TRIGGER categories_ai AFTER INSERT ON marketplace_categories_cache BEGIN
    INSERT INTO marketplace_categories_fts(rowid, full_path_name) VALUES (new.id, new.full_path_name);
END;
CREATE TRIGGER categories_ad AFTER DELETE ON marketplace_categories_cache BEGIN
    INSERT INTO marketplace_categories_fts(marketplace_categories_fts, rowid, full_path_name) VALUES ('delete', old.id, old.full_path_name);
END;
CREATE TRIGGER categories_au AFTER UPDATE ON marketplace_categories_cache BEGIN
    INSERT INTO marketplace_categories_fts(marketplace_categories_fts, rowid, full_path_name) VALUES ('delete', old.id, old.full_path_name);
    INSERT INTO marketplace_categories_fts(rowid, full_path_name) VALUES (new.id, new.full_path_name);
END;

ALTER TABLE orders ADD COLUMN chosen_supplier_id INTEGER REFERENCES variant_suppliers(id);
ALTER TABLE orders ADD COLUMN raw_tracking_number TEXT;
ALTER TABLE orders ADD COLUMN masked_tracking_number TEXT;
