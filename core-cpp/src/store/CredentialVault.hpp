#pragma once

// Шифрование учётных данных магазинов (stores.encrypted_credentials): AES-256-GCM.
// Формат блоба: nonce(12) | шифртекст | тег(16). Ключ — 32 случайных байта в файле secret.key (0600),
// который лежит рядом с базой, но не в ней: украденная копия dropship.db без ключа бесполезна.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/Expected.hpp"

namespace dropship::store {

class CredentialVault {
public:
    using Key = std::array<std::uint8_t, 32>;
    using Blob = std::vector<std::uint8_t>;

    static CredentialVault fromKey(const Key& key) { return CredentialVault(key); }
    // Читает ключ или создаёт новый. Повреждённый существующий файл — ошибка, а не тихая замена
    // (иначе уже сохранённые данные стали бы нечитаемыми без предупреждения).
    static Expected<CredentialVault, std::string> openOrCreate(const std::filesystem::path& keyFile);

    Expected<Blob, std::string> encrypt(std::string_view plaintext) const;
    Expected<std::string, std::string> decrypt(const Blob& blob) const;

private:
    explicit CredentialVault(const Key& key) : key_(key) {}
    Key key_;
};

}  // namespace dropship::store
