#pragma once

#include "db/DatabaseManager.hpp"

namespace dropship::db {

// Первый запуск: если магазинов нет, создаёт локальный каталог «Мой каталог», чтобы импорт
// товаров работал сразу, без настройки. Повторный вызов ничего не меняет.
Result<void> ensureDefaultStore(DatabaseManager& database, UnixSeconds now);

}  // namespace dropship::db
