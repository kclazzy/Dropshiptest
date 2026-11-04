#pragma once

// Свежие куки и User-Agent поставщиков, присланные расширением после прохождения капчи.
// Хранятся только в памяти (шифрованное хранение на диске — вместе с модулем криптографии).

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Domain.hpp"
#include "core/Expected.hpp"

namespace dropship::bridge {

struct Cookie {
    std::string name;
    std::string value;
};

struct SupplierSession {
    std::string cookieHeader;  // "a=b; c=d" — готово для заголовка Cookie
    std::string userAgent;
    UnixSeconds updatedAt = 0;
};

class SupplierSessionStore {
public:
    // Заменяет сессию домена целиком. Значения, пригодные для header-injection (CR/LF, ';'), отклоняются.
    Expected<void, std::string> update(std::string_view domain, const std::vector<Cookie>& cookies,
                                       std::string_view userAgent, UnixSeconds now);

    // Сессия для хоста: точное совпадение домена или поддомен; побеждает самый длинный домен.
    std::optional<SupplierSession> lookup(std::string_view host) const;

    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SupplierSession> sessions_;
};

}  // namespace dropship::bridge
