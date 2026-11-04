# Dropship

Локальная система автоматизации дропшиппинга и ресейла: ядро на Rust, окно на Tauri v2 (React + Tailwind),
расширение браузера (Manifest V3), ИИ — локальная Ollama **или облачные провайдеры** (шаг 2).
Работает с Ozon, Avito и другими площадками; данные хранятся только на вашем компьютере (SQLite).

## Структура

| Путь | Что внутри |
|---|---|
| `src-tauri/src/` | Ядро на Rust: `db/` (SQLite WAL, очередь задач), `pricing/` (цены), `catalog`, `import`, `monitor/` (мониторинг), `sync` (отправка на витрины), `orders/`, `bridge/` (WebSocket-мост), `stores/` (Ozon, Avito), `ai/` (Ollama и облачные провайдеры), `ui` + `commands` (API интерфейса), `tray`, `update` |
| `src-tauri/migrations/` | Схема базы данных |
| `frontend/` | Интерфейс (React 19, TypeScript, Tailwind): 4 вкладки, мастер первого запуска |
| `extension-mv3/` | Расширение браузера (Manifest V3): импорт товара, сессия поставщика, помощь при выкупе |
| `docs/USER_GUIDE.md` | **Инструкция пользователя** |
| `docs/AI_PROVIDERS.md`, `docs/STORES.md` | Подключение ИИ-сервисов и магазинов |
| `docs/dev-log.md` | Журнал разработки: решения, отклонения от ТЗ, что не проверено |

## Сборка и тесты

Нужны Rust 1.80+ и Node 20+. На Linux для Tauri: `libwebkit2gtk-4.1-dev libsoup-3.0-dev libayatana-appindicator3-dev librsvg2-dev`.

```bash
cd frontend && npm install && npm run build && cd ..
cargo test --workspace          # тесты ядра
cargo clippy --all-targets      # линтер
cargo tauri dev                 # запуск окна (нужен cargo install tauri-cli)
cd extension-mv3 && npm ci && npm test && npm run build   # расширение -> extension-mv3/dist
```

## Что сделано и что нет

| Шаг | Статус |
|---|---|
| 1. БД, очередь, PricingEngine, калькулятор | ✅ |
| 2. Адаптеры Ozon/Avito, ИИ (Ollama + облако) | ✅ код и тесты на фиктивных серверах; **живые API не проверялись** |
| 3. Импорт, мост, мониторинг, расширение | ✅ (клиент расширения проверен против настоящего моста; в Chrome не запускалось) |
| 4. Заказы, ассистированный выкуп, трек | ✅ без автозагрузки заказов с площадок и без автооплаты |
| 5. Интерфейс, трей, мастер, обновление | ✅ интерфейс проверен в браузере с подставным API; окно Tauri не запускалось (нет дисплея); обновление — через страницу релиза |

Не реализовано: создание карточек на Ozon/Avito (OZON_PUBLISH), загрузка заказов и сообщений с площадок, Shopify/WooCommerce/eBay, тихое автообновление (нужны подписанные релизы).
