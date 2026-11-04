#include "bridge/RpcRouter.hpp"

namespace dropship::bridge {

void RpcRouter::add(std::string method, RpcHandler handler) {
    handlers_.insert_or_assign(std::move(method), std::move(handler));
}

std::optional<std::string> RpcRouter::handle(const RpcRequest& request) const {
    const auto it = handlers_.find(request.method);
    if (it == handlers_.end()) {
        if (!request.id) return std::nullopt;
        return makeError(request.id, RpcError{rpc_code::kMethodNotFound, "method not found: " + request.method, nullptr});
    }

    Expected<Json, RpcError> outcome = [&]() -> Expected<Json, RpcError> {
        try {
            return it->second(request.params);
        } catch (...) {
            // Текст исключения может содержать пути и другие детали машины пользователя.
            return unexpected(RpcError{rpc_code::kInternalError, "internal error", nullptr});
        }
    }();

    if (!request.id) return std::nullopt;
    if (outcome.has_value()) return makeResult(*request.id, outcome.value());
    return makeError(request.id, outcome.error());
}

}  // namespace dropship::bridge
