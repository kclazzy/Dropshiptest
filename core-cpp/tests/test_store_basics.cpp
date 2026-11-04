#include <filesystem>
#include <fstream>

#include "Harness.hpp"
#include "TestDb.hpp"
#include "store/CredentialVault.hpp"
#include "store/RateLimiter.hpp"

using namespace dropship;
using namespace dropship::store;

namespace {
CredentialVault::Key keyOf(std::uint8_t v) {
    CredentialVault::Key k{};
    k.fill(v);
    return k;
}
}  // namespace

TEST(vault_roundtrips_and_never_stores_plaintext) {
    const auto vault = CredentialVault::fromKey(keyOf(7));
    const std::string secret = R"({"client_id":"123","api_key":"super-secret-key"})";
    const auto blob = vault.encrypt(secret);
    REQUIRE(blob.has_value());
    const std::string raw(blob->begin(), blob->end());
    CHECK(raw.find("super-secret") == std::string::npos);
    const auto plain = vault.decrypt(*blob);
    REQUIRE(plain.has_value());
    CHECK_EQ(*plain, secret);
}

TEST(vault_uses_a_fresh_nonce_each_time) {
    const auto vault = CredentialVault::fromKey(keyOf(7));
    CHECK(*vault.encrypt("same") != *vault.encrypt("same"));
}

TEST(vault_detects_tampering_wrong_key_and_truncation) {
    const auto vault = CredentialVault::fromKey(keyOf(7));
    auto blob = *vault.encrypt("hello world");
    auto flipped = blob;
    flipped[14] ^= 1;  // шифртекст
    CHECK(!vault.decrypt(flipped).has_value());
    auto badTag = blob;
    badTag.back() ^= 1;
    CHECK(!vault.decrypt(badTag).has_value());
    CHECK(!CredentialVault::fromKey(keyOf(8)).decrypt(blob).has_value());
    CHECK(!vault.decrypt(CredentialVault::Blob(10, 0)).has_value());
    CHECK(!vault.decrypt({}).has_value());
}

TEST(vault_handles_empty_and_large_plaintext) {
    const auto vault = CredentialVault::fromKey(keyOf(1));
    CHECK_EQ(*vault.decrypt(*vault.encrypt("")), std::string(""));
    const std::string big(100000, 'x');
    CHECK_EQ(*vault.decrypt(*vault.encrypt(big)), big);
}

TEST(vault_key_file_is_created_private_reused_and_never_silently_replaced) {
    testdb::TempDir dir;
    const auto keyFile = dir.file("secret.key");
    const auto first = CredentialVault::openOrCreate(keyFile);
    REQUIRE(first.has_value());
    const auto blob = *first->encrypt("data");
    const auto second = CredentialVault::openOrCreate(keyFile);  // тот же ключ
    REQUIRE(second.has_value());
    CHECK_EQ(*second->decrypt(blob), std::string("data"));
#ifndef _WIN32
    const auto perms = std::filesystem::status(keyFile).permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
    std::ofstream(keyFile, std::ios::trunc) << "garbage";
    CHECK(!CredentialVault::openOrCreate(keyFile).has_value());
    std::ifstream in(keyFile);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    CHECK_EQ(content, std::string("garbage"));  // файл не тронут
}

TEST(rate_limiter_allows_a_burst_then_paces_requests) {
    TokenBucketRateLimiter limiter(2.0, 3.0);  // 2 запроса/с, запас 3
    CHECK(limiter.tryAcquire(100.0));
    CHECK(limiter.tryAcquire(100.0));
    CHECK(limiter.tryAcquire(100.0));
    CHECK(!limiter.tryAcquire(100.0));
    CHECK(limiter.secondsUntilAvailable(100.0) > 0.49 && limiter.secondsUntilAvailable(100.0) < 0.51);
    CHECK(!limiter.tryAcquire(100.4));
    CHECK(limiter.tryAcquire(100.5));
}

TEST(rate_limiter_caps_the_stock_and_ignores_clock_going_backwards) {
    TokenBucketRateLimiter limiter(1.0, 2.0);
    CHECK(limiter.tryAcquire(0.0));
    CHECK(limiter.tryAcquire(1000.0));  // долгая пауза не копит больше burst
    CHECK(limiter.tryAcquire(1000.0));
    CHECK(!limiter.tryAcquire(1000.0));
    CHECK(!limiter.tryAcquire(500.0));  // откат часов токенов не добавляет
    CHECK(!limiter.tryAcquire(1000.0, 5.0));  // больше burst — никогда
}
