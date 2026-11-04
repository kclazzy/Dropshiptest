#include "store/CredentialVault.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fstream>
#include <memory>
#include <sstream>

#include "bridge/TokenStore.hpp"

namespace dropship::store {

namespace {

constexpr int kNonceSize = 12;
constexpr int kTagSize = 16;
constexpr std::string_view kAad = "dropship-store-credentials-v1";

struct CtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept { EVP_CIPHER_CTX_free(ctx); }
};
using Ctx = std::unique_ptr<EVP_CIPHER_CTX, CtxDeleter>;

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::optional<CredentialVault::Key> decodeKey(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (text.size() != 64) return std::nullopt;
    CredentialVault::Key key{};
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = hexValue(text[2 * i]), lo = hexValue(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        key[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return key;
}

}  // namespace

Expected<CredentialVault, std::string> CredentialVault::openOrCreate(const std::filesystem::path& keyFile) {
    std::error_code ec;
    if (std::filesystem::exists(keyFile, ec)) {
        std::ifstream in(keyFile);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto key = decodeKey(buffer.str());
        if (!key) return unexpected(std::string("secret.key is damaged; restore it from a backup (it is not replaced automatically)"));
        return CredentialVault(*key);
    }
    // Файла нет — токен-хранилище создаёт его сразу с правами 0600.
    const auto created = bridge::loadOrCreateToken(keyFile);
    if (!created) return unexpected(created.error());
    const auto key = decodeKey(*created);
    if (!key) return unexpected(std::string("cannot create secret.key"));
    return CredentialVault(*key);
}

Expected<CredentialVault::Blob, std::string> CredentialVault::encrypt(std::string_view plaintext) const {
    Blob out(kNonceSize);
    if (RAND_bytes(out.data(), kNonceSize) != 1) return unexpected(std::string("random generator failed"));
    Ctx ctx(EVP_CIPHER_CTX_new());
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key_.data(), out.data()) != 1) {
        return unexpected(std::string("cipher init failed"));
    }
    int len = 0;
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(kAad.data()),
                          static_cast<int>(kAad.size())) != 1) {
        return unexpected(std::string("cipher aad failed"));
    }
    out.resize(kNonceSize + plaintext.size());
    if (EVP_EncryptUpdate(ctx.get(), out.data() + kNonceSize, &len, reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        return unexpected(std::string("encryption failed"));
    }
    int finalLen = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), out.data() + kNonceSize + len, &finalLen) != 1) {
        return unexpected(std::string("encryption failed"));
    }
    out.resize(kNonceSize + static_cast<std::size_t>(len + finalLen) + kTagSize);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kTagSize, out.data() + out.size() - kTagSize) != 1) {
        return unexpected(std::string("cannot read auth tag"));
    }
    return out;
}

Expected<std::string, std::string> CredentialVault::decrypt(const Blob& blob) const {
    if (blob.size() < static_cast<std::size_t>(kNonceSize + kTagSize)) return unexpected(std::string("credentials blob is too short"));
    const std::size_t cipherLen = blob.size() - kNonceSize - kTagSize;
    Ctx ctx(EVP_CIPHER_CTX_new());
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key_.data(), blob.data()) != 1) {
        return unexpected(std::string("cipher init failed"));
    }
    int len = 0;
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(kAad.data()),
                          static_cast<int>(kAad.size())) != 1) {
        return unexpected(std::string("cipher aad failed"));
    }
    std::string plain(cipherLen, '\0');
    if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()), &len, blob.data() + kNonceSize,
                          static_cast<int>(cipherLen)) != 1) {
        return unexpected(std::string("decryption failed"));
    }
    std::array<std::uint8_t, kTagSize> tag{};
    std::copy(blob.end() - kTagSize, blob.end(), tag.begin());
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTagSize, tag.data()) != 1) {
        return unexpected(std::string("cannot set auth tag"));
    }
    int finalLen = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()) + len, &finalLen) != 1) {
        return unexpected(std::string("credentials are damaged or the key is wrong"));
    }
    plain.resize(static_cast<std::size_t>(len + finalLen));
    return plain;
}

}  // namespace dropship::store
