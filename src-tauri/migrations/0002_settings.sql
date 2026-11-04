-- Настройки приложения: ключ -> значение (JSON или текст). Секреты хранятся только зашифрованными (hex).
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);
