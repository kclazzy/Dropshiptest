#include "bridge/Handlers.hpp"

#include "importer/ProductImport.hpp"

namespace dropship::bridge {

namespace {

RpcError invalidParams(std::string message) {
    return RpcError{rpc_code::kInvalidParams, std::move(message), nullptr};
}

Expected<Json, RpcError> handleImport(CoreServices& services, const Json& params) {
    auto payload = importer::parseImportPayload(params);
    if (!payload) return unexpected(payload.error());
    auto outcome = importer::importProduct(services.db, *payload, services.clock());
    if (!outcome) return unexpected(outcome.error());
    return Json{{"product_id", outcome->productId},
                {"duplicate", outcome->duplicate},
                {"variants", outcome->variantCount}};
}

Expected<Json, RpcError> handleCookies(CoreServices& services, const Json& params) {
    if (!params.is_object()) return unexpected(invalidParams("params must be an object"));
    const auto domain = params.find("domain");
    if (domain == params.end() || !domain->is_string()) return unexpected(invalidParams("domain must be a string"));
    const auto list = params.find("cookies");
    if (list == params.end() || !list->is_array()) return unexpected(invalidParams("cookies must be an array"));

    std::vector<Cookie> cookies;
    for (const auto& item : *list) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string() || !item.contains("value") ||
            !item["value"].is_string()) {
            return unexpected(invalidParams("each cookie needs string name and value"));
        }
        cookies.push_back({item["name"].get<std::string>(), item["value"].get<std::string>()});
    }

    std::string userAgent;
    if (const auto ua = params.find("user_agent"); ua != params.end() && !ua->is_null()) {
        if (!ua->is_string()) return unexpected(invalidParams("user_agent must be a string"));
        userAgent = ua->get<std::string>();
    }

    const auto stored = services.sessions.update(domain->get<std::string>(), cookies, userAgent, services.clock());
    if (!stored) return unexpected(invalidParams(stored.error()));
    return Json{{"stored", cookies.size()}};
}

}  // namespace

void registerCoreHandlers(RpcRouter& router, CoreServices services) {
    router.add("PING", [services](const Json&) -> Expected<Json, RpcError> {
        return Json{{"pong", true}, {"time", services.clock()}};
    });
    router.add("IMPORT_PRODUCT_PAYLOAD", [services](const Json& params) mutable { return handleImport(services, params); });
    router.add("SYNC_SUPPLIER_COOKIES", [services](const Json& params) mutable { return handleCookies(services, params); });
}

}  // namespace dropship::bridge
