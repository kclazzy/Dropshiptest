#pragma once

#include <cstddef>

#include "net/HttpTransport.hpp"

namespace dropship::net {

// Реальный HTTP-клиент на libcurl. Каждый вызов использует собственный easy-handle,
// поэтому один экземпляр можно безопасно вызывать из нескольких потоков.
class CurlTransport : public IHttpTransport {
public:
    static constexpr std::size_t kDefaultMaxResponseBytes = 16 * 1024 * 1024;

    explicit CurlTransport(std::size_t maxResponseBytes = kDefaultMaxResponseBytes);

    // Только http/https; редиректы не выполняются (локальные сервисы их не используют,
    // а редирект на другой хост мог бы унести заголовок Authorization).
    Expected<HttpResponse, TransportError> send(const HttpRequest& request) override;

private:
    std::size_t maxResponseBytes_;
};

}  // namespace dropship::net
