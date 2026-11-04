#include "bridge/TokenStore.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dropship::bridge {

namespace {

bool isValidToken(const std::string& token) {
    return token.size() == kTokenHexLength &&
           std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string readTrimmed(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

Expected<std::string, std::string> generate() {
    unsigned char bytes[kTokenHexLength / 2];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return unexpected(std::string("cannot generate random token"));
    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    for (const unsigned char b : bytes) {
        token += kHex[b >> 4];
        token += kHex[b & 0x0F];
    }
    return token;
}

// Файл создаётся сразу с правами 0600, а не «создать, потом ограничить».
Expected<void, std::string> writePrivate(const std::filesystem::path& file, const std::string& token) {
#ifdef _WIN32
    std::ofstream out(file, std::ios::trunc);
    out << token;
    if (!out) return unexpected(std::string("cannot write token file"));
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return unexpected(std::string("cannot open token file for writing"));
    std::size_t written = 0;
    while (written < token.size()) {
        const auto n = ::write(fd, token.data() + written, token.size() - written);
        if (n < 0) {
            ::close(fd);
            return unexpected(std::string("cannot write token file"));
        }
        written += static_cast<std::size_t>(n);
    }
    ::close(fd);
    // O_TRUNC не меняет права уже существующего файла.
    std::error_code ec;
    std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) return unexpected(std::string("cannot restrict token file permissions"));
#endif
    return {};
}

}  // namespace

Expected<std::string, std::string> loadOrCreateToken(const std::filesystem::path& file) {
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        std::string existing = readTrimmed(file);
        if (isValidToken(existing)) return existing;
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec) return unexpected(std::string("cannot create token directory: ") + ec.message());
    }
    auto token = generate();
    if (!token) return unexpected(token.error());
    if (auto written = writePrivate(file, *token); !written) return unexpected(written.error());
    return token;
}

}  // namespace dropship::bridge
