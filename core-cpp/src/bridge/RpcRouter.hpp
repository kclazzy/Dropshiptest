#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "bridge/JsonRpc.hpp"

namespace dropship::bridge {

using RpcHandler = std::function<Expected<Json, RpcError>(const Json& params)>;

// Таблица «метод -> обработчик». Регистрация выполняется до запуска сервера,
// после этого router только читается, поэтому блокировки не нужны.
class RpcRouter {
public:
    void add(std::string method, RpcHandler handler);

    // Готовый текст ответа; nullopt для notification (по спецификации ответа нет, даже на ошибку).
    // Исключение в обработчике превращается в InternalError без утечки текста исключения.
    std::optional<std::string> handle(const RpcRequest& request) const;

private:
    std::unordered_map<std::string, RpcHandler> handlers_;
};

}  // namespace dropship::bridge
