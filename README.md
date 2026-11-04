# Dropship

Локальная система автоматизации дропшиппинга и ресейла: ядро на Rust, окно на Tauri v2 (React + Tailwind),
расширение браузера (Manifest V3), ИИ — локальная Ollama **или облачные провайдеры** (шаг 2).
Работает с Ozon, Avito и другими площадками; данные хранятся только на вашем компьютере (SQLite).

## Структура

| Путь | Что внутри |
|---|---|
| `src-tauri/` | Ядро на Rust и приложение Tauri: `db/` (SQLite WAL, очередь задач), `pricing/` (цены), `error.rs` |
| `src-tauri/migrations/` | Схема базы данных |
| `frontend/` | Интерфейс (React 19, TypeScript, Tailwind): пока калькулятор прибыли |
| `extension-mv3/` | Расширение браузера (шаг 3) |
| `docs/dev-log.md` | Журнал разработки: решения, отклонения от ТЗ, что не проверено |

## Сборка и тесты

Нужны Rust 1.80+ и Node 20+. На Linux для Tauri: `libwebkit2gtk-4.1-dev libsoup-3.0-dev libayatana-appindicator3-dev librsvg2-dev`.

```bash
cd frontend && npm install && npm run build && cd ..
cargo test --workspace          # тесты ядра
cargo clippy --all-targets      # линтер
cargo tauri dev                 # запуск окна (нужен cargo install tauri-cli)
```

## План

| Шаг | Содержание |
|---|---|
| 1 ✅ | Структура, БД (WAL, миграции, атомарная очередь), `PricingEngine`, калькулятор в окне |
| 2 | Адаптеры `OzonSellerAdapter` и `AvitoAdapter`; ИИ: Ollama **и облачные провайдеры** с инструкциями подключения; генерация карточек под Ozon/Avito |
| 3 | WebSocket-мост и расширение, мониторинг цен и остатков |
| 4 | Заказы, выкуп, трек-номера |
| 5 | Полный интерфейс (4 вкладки, мастер запуска, трей), обновление в один клик |
