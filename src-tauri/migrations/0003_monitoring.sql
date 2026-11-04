-- Что уже отправлено на витрину / что площадка отклонила (хэш состояния публикации): повторно не шлём.
ALTER TABLE product_variants ADD COLUMN published_hash TEXT;
ALTER TABLE product_variants ADD COLUMN rejected_hash TEXT;
-- Последняя ошибка обмена с магазином (понятный текст для интерфейса).
ALTER TABLE stores ADD COLUMN last_error TEXT;

-- История цен и остатков поставщика: одна строка на каждое реальное изменение.
CREATE TABLE price_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    variant_id INTEGER NOT NULL REFERENCES product_variants(id) ON DELETE CASCADE,
    supplier_price_minor INTEGER NOT NULL,
    shipping_price_minor INTEGER NOT NULL,
    retail_price_minor INTEGER NOT NULL,
    supplier_stock INTEGER NOT NULL,
    recorded_at INTEGER NOT NULL
);
CREATE INDEX idx_price_history_variant ON price_history(variant_id, recorded_at);
CREATE INDEX idx_variants_product ON product_variants(product_id);
