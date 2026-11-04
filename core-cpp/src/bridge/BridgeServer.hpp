#pragma once

// Локальный WebSocket-мост между ядром и расширением браузера: ws://127.0.0.1:<port>/ws.
//
// Безопасность:
//  * слушает только loopback-адрес;
//  * соединение с Origin обычной веб-страницы (http/https) отклоняется на рукопожатии,
//    чтобы сайт в браузере не мог стучаться на локальный порт;
//  * первым сообщением обязателен AUTH с токеном (браузерный WebSocket не умеет ставить
//    свои заголовки, поэтому токен передаётся первым пакетом); без него соединение закрывается;
//  * размер сообщения ограничен, бинарные кадры не принимаются.
//
// Потоки: вся сетевая работа идёт в одном потоке std::jthread, поэтому обработчики
// RpcRouter выполняются последовательно и должны быть быстрыми. Один активный клиент:
// новое авторизованное соединение вытесняет предыдущее.

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>

#include "bridge/JsonRpc.hpp"
#include "bridge/RpcRouter.hpp"

namespace dropship::bridge {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::size_t kMinTokenLength = 32;

struct BridgeConfig {
    std::string bindAddress = "127.0.0.1";  // только loopback
    std::uint16_t port = 8765;              // 0 — свободный порт (тесты)
    std::string authToken;                  // не короче kMinTokenLength символов
    std::size_t maxMessageBytes = 4 * 1024 * 1024;
    std::chrono::milliseconds authTimeout{5000};
    std::chrono::seconds idleTimeout{90};  // расширение шлёт PING каждые 20 с
};

using CallResult = Expected<Json, RpcError>;

class BridgeServer {
public:
    static Expected<std::unique_ptr<BridgeServer>, std::string> start(BridgeConfig config,
                                                                      std::shared_ptr<const RpcRouter> router);
    ~BridgeServer();  // корректно закрывает соединения и дожидается потока
    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;

    std::uint16_t port() const;
    bool extensionConnected() const;

    // Запрос ядро -> расширение (EXECUTE_AUTO_ORDER, SCRAPE_DYNAMIC_PAGE, ...).
    // Future всегда получает значение: ответ, ошибку расширения, kTimeout или
    // kExtensionNotConnected (нет соединения / оно оборвалось / сервер остановлен).
    std::future<CallResult> call(std::string method, Json params, std::chrono::milliseconds timeout);

private:
    BridgeServer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dropship::bridge
