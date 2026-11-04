#pragma once

// Команды управления без окна: добавить магазин (ключи читаются из stdin, а не из аргументов —
// чтобы они не попали в историю командной строки), показать магазины/варианты, привязать вариант к карточке на витрине.

#include <iosfwd>
#include <string>
#include <vector>

#include "db/DatabaseManager.hpp"
#include "store/CredentialVault.hpp"

namespace dropship::cli {

// true, если первый аргумент — команда управления (а не запуск ядра).
bool isCommand(const std::string& arg);

// Возвращает код выхода процесса.
int run(const std::vector<std::string>& args, db::DatabaseManager& database, const store::CredentialVault& vault,
        std::istream& in, std::ostream& out, std::ostream& err, UnixSeconds now);

}  // namespace dropship::cli
