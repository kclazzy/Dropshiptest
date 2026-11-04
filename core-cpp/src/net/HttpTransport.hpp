#pragma once

// Граница с сетью: всё, что ходит по HTTP, получает транспорт через этот интерфейс,
// поэтому бизнес-логику можно проверять без сети.

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "core/Expected.hpp"

namespace dropship::net {

struct HttpRequest {
    enum class Method { Get, Post };
    Method method = Method::Get;
    std::string url;
    std::string body;  // для POST отправляется как application/json
    std::vector<std::pair<std::string, std::string>> headers;  // например Authorization для облачных API
    std::chrono::milliseconds timeout{30'000};
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

// Не удалось получить ответ вообще (нет соединения, таймаут, слишком большой ответ).
// HTTP-статусы 4xx/5xx — это HttpResponse, а не ошибка транспорта.
struct TransportError {
    std::string message;
};

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    virtual Expected<HttpResponse, TransportError> send(const HttpRequest& request) = 0;
};

}  // namespace dropship::net
