#pragma once

#include <filesystem>
#include <string>

#include "core/Expected.hpp"

namespace dropship::bridge {

inline constexpr std::size_t kTokenHexLength = 64;  // 32 случайных байта

// Токен локального моста: читает файл или создаёт новый (OpenSSL RAND_bytes, права 0600).
// Файл с повреждённым содержимым заменяется новым токеном.
Expected<std::string, std::string> loadOrCreateToken(const std::filesystem::path& file);

}  // namespace dropship::bridge
