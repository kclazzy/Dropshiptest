#pragma once

#include "db/Connection.hpp"

namespace dropship::db {

// Накатывает недостающие миграции. Каждая применяется в своей транзакции
// BEGIN IMMEDIATE и повторно проверяется внутри неё, поэтому повторный запуск
// (в том числе после сбоя посередине или из двух процессов) приводит к тому же состоянию.
Result<void> migrate(Connection& writer);

inline constexpr int kLatestSchemaVersion = 3;

}  // namespace dropship::db
