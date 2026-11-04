-- Профилирование (docs/dev-log.md, шаг 8): индексы под реальные запросы.
-- Очередь: воркеры всегда фильтруют по типу задачи, а проверка «уже есть открытая задача» ходит по типу и статусу.
CREATE INDEX idx_task_queue_type_status ON task_queue(task_type, status, priority ASC, scheduled_at ASC);
-- Опрос заказов и пересчёт магазина шли полным сканированием.
CREATE INDEX idx_orders_status ON orders(status);
CREATE INDEX idx_products_store ON products(store_id);
