#include "db/Bootstrap.hpp"

namespace dropship::db {

Result<void> ensureDefaultStore(DatabaseManager& database, UnixSeconds now) {
    // Один INSERT ... WHERE NOT EXISTS: проверка и вставка атомарны даже при гонке двух процессов.
    const auto done = database.execute(
        "INSERT INTO stores(name, platform_type, api_url, encrypted_credentials, created_at) "
        "SELECT 'Мой каталог', 'local', '', x'', ? WHERE NOT EXISTS (SELECT 1 FROM stores)",
        {now});
    if (!done) return unexpected(done.error());
    return {};
}

}  // namespace dropship::db
