#pragma once

// JSON-RPC 2.0 (без batch): разбор входящих сообщений и сборка исходящих.
// Данные проверяются один раз, на входе; дальше код доверяет разобранным структурам.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "core/Expected.hpp"
#include "core/Json.hpp"

namespace dropship::bridge {

namespace rpc_code {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
// Серверные коды (-32000..-32099).
inline constexpr int kExtensionNotConnected = -32001;
inline constexpr int kTimeout = -32002;
inline constexpr int kUnauthorized = -32003;
inline constexpr int kNoStore = -32010;
inline constexpr int kStoreNotFound = -32011;
}  // namespace rpc_code

using RpcId = std::variant<std::int64_t, std::string>;

struct RpcError {
    int code = rpc_code::kInternalError;
    std::string message;
    Json data = nullptr;
};

struct RpcRequest {
    std::optional<RpcId> id;  // нет id — notification, ответа не требуется
    std::string method;
    Json params = nullptr;  // null, если параметров нет
};

struct RpcResponse {
    RpcId id;
    Expected<Json, RpcError> outcome;
};

using Incoming = std::variant<RpcRequest, RpcResponse>;

// Что ответить отправителю, если сообщение разобрать не удалось.
struct ParseFailure {
    RpcError error;
    std::optional<RpcId> id;  // известен, если удалось его прочитать
};

Expected<Incoming, ParseFailure> parseIncoming(std::string_view text);

std::string makeRequest(const RpcId& id, std::string_view method, const Json& params);
std::string makeResult(const RpcId& id, const Json& result);
std::string makeError(const std::optional<RpcId>& id, const RpcError& error);

}  // namespace dropship::bridge
