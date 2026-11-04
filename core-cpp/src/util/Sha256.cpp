#include "util/Sha256.hpp"

#include <openssl/evp.h>

#include <stdexcept>

namespace dropship {

std::string sha256Hex(std::string_view data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr) != 1) {
        // Пустой хэш как ключ дедупликации молча склеил бы разные товары — лучше упасть громко.
        throw std::runtime_error("SHA-256 computation failed");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        hex += kHex[digest[i] >> 4];
        hex += kHex[digest[i] & 0x0F];
    }
    return hex;
}

}  // namespace dropship
