#include "bridge/Handlers.hpp"

#include "importer/ProductImport.hpp"
#include "orders/AutoOrder.hpp"
#include "orders/Tracking.hpp"

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
    if (services.onSessionUpdated) services.onSessionUpdated(domain->get<std::string>());
    return Json{{"stored", cookies.size()}};
}

// Ошибка уровня приложения (code 0) — неверные данные клиента; остальное — внутренняя ошибка базы.
RpcError fromDb(const db::DbError& error) {
    if (error.sqliteCode == 0) return invalidParams(error.message);
    return RpcError{rpc_code::kInternalError, "database error", nullptr};
}

std::optional<std::int64_t> orderIdParam(const Json& params) {
    if (!params.is_object() || !params.contains("order_id")) return std::nullopt;
    const auto id = asInt64(params["order_id"]);
    return id && *id > 0 ? id : std::nullopt;
}

std::string stringParam(const Json& params, const char* key) {
    const auto it = params.find(key);
    return it != params.end() && it->is_string() ? it->get<std::string>() : std::string();
}

Expected<Json, RpcError> handleFulfillment(CoreServices& services, const Json& params) {
    const auto id = orderIdParam(params);
    if (!id) return unexpected(invalidParams("order_id must be a positive integer"));
    const std::string status = stringParam(params, "status");
    orders::Fulfillment result;
    result.orderId = *id;
    result.supplierOrderId = stringParam(params, "supplier_order_id");
    result.message = stringParam(params, "message");
    if (status == "ordered") result.status = orders::FulfillmentStatus::Ordered;
    else if (status == "failed") result.status = orders::FulfillmentStatus::Failed;
    else if (status == "needs_manual") result.status = orders::FulfillmentStatus::NeedsManual;
    else return unexpected(invalidParams("status must be ordered, failed or needs_manual"));
    const auto changed = orders::applyFulfillment(services.db, result, services.clock());
    if (!changed) return unexpected(fromDb(changed.error()));
    return Json{{"changed", *changed}};
}

Expected<Json, RpcError> handleTracking(CoreServices& services, const Json& params) {
    const auto id = orderIdParam(params);
    if (!id) return unexpected(invalidParams("order_id must be a positive integer"));
    const auto changed = orders::applyTracking(services.db, *id, stringParam(params, "tracking_number"),
                                               stringParam(params, "carrier"), services.clock());
    if (!changed) return unexpected(fromDb(changed.error()));
    return Json{{"changed", *changed}};
}

Expected<Json, RpcError> handleListOrders(CoreServices& services, const Json& params) {
    const std::string status = params.is_object() ? stringParam(params, "status") : std::string();
    const auto rows = services.db.query(
        "SELECT o.id, o.external_order_id, o.status, o.quantity, o.retail_total_cents, o.supplier_cost_cents, "
        "o.net_profit_cents, o.buyer_address_json, o.supplier_order_id, o.tracking_number, o.tracking_carrier, "
        "o.error_message, o.created_at, p.supplier_url, v.supplier_sku, "
        "COALESCE(p.title_override, p.ai_seo_title, p.raw_title) AS title "
        "FROM orders o LEFT JOIN product_variants v ON v.id = o.variant_id LEFT JOIN products p ON p.id = v.product_id "
        "WHERE (? = '' OR o.status = ?) ORDER BY o.id DESC LIMIT 200",
        {status, status});
    if (!rows) return unexpected(fromDb(rows.error()));
    const auto text = [&](std::size_t i, const char* c) -> Json {
        return rows->isNull(i, c) ? Json(nullptr) : Json(rows->textAt(i, c));
    };
    const auto number = [&](std::size_t i, const char* c) -> Json {
        return rows->isNull(i, c) ? Json(nullptr) : Json(rows->intAt(i, c));
    };
    Json list = Json::array();
    for (std::size_t i = 0; i < rows->rows.size(); ++i) {
        list.push_back({{"id", rows->intAt(i, "id")},
                        {"external_order_id", rows->textAt(i, "external_order_id")},
                        {"status", rows->textAt(i, "status")},
                        {"quantity", rows->intAt(i, "quantity")},
                        {"retail_total_cents", rows->intAt(i, "retail_total_cents")},
                        {"supplier_cost_cents", number(i, "supplier_cost_cents")},
                        {"net_profit_cents", number(i, "net_profit_cents")},
                        {"address", parseJson(rows->textAt(i, "buyer_address_json"))},
                        {"supplier_order_id", text(i, "supplier_order_id")},
                        {"tracking_number", text(i, "tracking_number")},
                        {"tracking_carrier", text(i, "tracking_carrier")},
                        {"error_message", text(i, "error_message")},
                        {"supplier_url", text(i, "supplier_url")},
                        {"sku", text(i, "supplier_sku")},
                        {"title", text(i, "title")},
                        {"created_at", rows->intAt(i, "created_at")}});
    }
    return Json{{"orders", list}};
}

}  // namespace

void registerCoreHandlers(RpcRouter& router, CoreServices services) {
    router.add("PING", [services](const Json&) -> Expected<Json, RpcError> {
        return Json{{"pong", true}, {"time", services.clock()}};
    });
    router.add("IMPORT_PRODUCT_PAYLOAD", [services](const Json& params) mutable { return handleImport(services, params); });
    router.add("ORDER_FULFILLMENT_RESULT", [services](const Json& params) mutable { return handleFulfillment(services, params); });
    router.add("ORDER_TRACKING_UPDATE", [services](const Json& params) mutable { return handleTracking(services, params); });
    router.add("LIST_ORDERS", [services](const Json& params) mutable { return handleListOrders(services, params); });
    router.add("SYNC_SUPPLIER_COOKIES", [services](const Json& params) mutable { return handleCookies(services, params); });
}

}  // namespace dropship::bridge
