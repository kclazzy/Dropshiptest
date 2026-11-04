#include "bridge/JsonRpc.hpp"

namespace dropship::bridge {

namespace {

Expected<Incoming, ParseFailure> fail(int code, std::string message, std::optional<RpcId> id) {
    return unexpected(ParseFailure{RpcError{code, std::move(message), nullptr}, std::move(id)});
}

Json idToJson(const RpcId& id) {
    if (const auto* number = std::get_if<std::int64_t>(&id)) return Json(*number);
    return Json(std::get<std::string>(id));
}

Expected<Incoming, ParseFailure> parseResponse(const Json& root, const std::optional<RpcId>& id) {
    if (!id) return fail(rpc_code::kInvalidRequest, "response must have an id", std::nullopt);
    const bool hasResult = root.contains("result");
    const bool hasError = root.contains("error");
    if (hasResult == hasError) {
        return fail(rpc_code::kInvalidRequest, "response must have exactly one of result and error", id);
    }
    if (hasResult) return Incoming{RpcResponse{*id, Expected<Json, RpcError>(root["result"])}};

    const Json& error = root["error"];
    if (!error.is_object() || !error.contains("code") || !asInt64(error["code"]) || !error.contains("message") ||
        !error["message"].is_string()) {
        return fail(rpc_code::kInvalidRequest, "error must be an object with integer code and string message", id);
    }
    RpcError parsed;
    parsed.code = static_cast<int>(*asInt64(error["code"]));
    parsed.message = error["message"].get<std::string>();
    if (error.contains("data")) parsed.data = error["data"];
    return Incoming{RpcResponse{*id, Expected<Json, RpcError>(unexpected(std::move(parsed)))}};
}

}  // namespace

Expected<Incoming, ParseFailure> parseIncoming(std::string_view text) {
    const Json root = parseJson(text);
    if (root.is_discarded()) return fail(rpc_code::kParseError, "invalid JSON", std::nullopt);
    if (!root.is_object()) return fail(rpc_code::kInvalidRequest, "message must be a JSON object", std::nullopt);

    std::optional<RpcId> id;
    if (root.contains("id")) {
        const Json& raw = root["id"];
        if (raw.is_string()) {
            id = raw.get<std::string>();
        } else if (const auto number = asInt64(raw)) {
            id = *number;
        } else {
            return fail(rpc_code::kInvalidRequest, "id must be a string or an integer", std::nullopt);
        }
    }

    if (!root.contains("jsonrpc") || root["jsonrpc"] != Json("2.0")) {
        return fail(rpc_code::kInvalidRequest, "jsonrpc must be \"2.0\"", id);
    }

    if (root.contains("method")) {
        if (!root["method"].is_string()) return fail(rpc_code::kInvalidRequest, "method must be a string", id);
        RpcRequest request;
        request.id = id;
        request.method = root["method"].get<std::string>();
        if (root.contains("params")) {
            const Json& params = root["params"];
            if (!params.is_object() && !params.is_array()) {
                return fail(rpc_code::kInvalidRequest, "params must be an object or an array", id);
            }
            request.params = params;
        }
        return Incoming{std::move(request)};
    }

    if (root.contains("result") || root.contains("error")) return parseResponse(root, id);
    return fail(rpc_code::kInvalidRequest, "message has neither method nor result/error", id);
}

std::string makeRequest(const RpcId& id, std::string_view method, const Json& params) {
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = idToJson(id);
    message["method"] = std::string(method);
    if (!params.is_null()) message["params"] = params;
    return dumpJson(message);
}

std::string makeResult(const RpcId& id, const Json& result) {
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = idToJson(id);
    message["result"] = result;
    return dumpJson(message);
}

std::string makeError(const std::optional<RpcId>& id, const RpcError& error) {
    Json body = Json::object();
    body["code"] = error.code;
    body["message"] = error.message;
    if (!error.data.is_null()) body["data"] = error.data;

    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id ? idToJson(*id) : Json(nullptr);
    message["error"] = std::move(body);
    return dumpJson(message);
}

}  // namespace dropship::bridge
