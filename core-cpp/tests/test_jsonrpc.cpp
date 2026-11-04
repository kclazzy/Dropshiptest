#include "Harness.hpp"
#include "bridge/JsonRpc.hpp"
#include "bridge/RpcRouter.hpp"

using namespace dropship;
using namespace dropship::bridge;

namespace {

const RpcRequest* asRequest(const Incoming& in) { return std::get_if<RpcRequest>(&in); }
const RpcResponse* asResponse(const Incoming& in) { return std::get_if<RpcResponse>(&in); }

bool rejects(std::string_view text, int code) {
    const auto r = parseIncoming(text);
    return !r.has_value() && r.error().error.code == code;
}

Json parsed(const std::optional<std::string>& text) {
    if (!text) return Json();
    return Json::parse(*text);
}

}  // namespace

TEST(rpc_parses_requests_with_integer_and_string_ids) {
    auto a = parseIncoming(R"({"jsonrpc":"2.0","id":7,"method":"PING","params":{"x":1}})");
    REQUIRE(a.has_value());
    const auto* req = asRequest(*a);
    REQUIRE(req != nullptr);
    CHECK(req->id.has_value());
    CHECK_EQ(std::get<std::int64_t>(*req->id), std::int64_t{7});
    CHECK_EQ(req->method, std::string("PING"));
    CHECK_EQ(req->params["x"].get<int>(), 1);

    auto b = parseIncoming(R"({"jsonrpc":"2.0","id":"abc","method":"M"})");
    REQUIRE(b.has_value());
    CHECK_EQ(std::get<std::string>(*asRequest(*b)->id), std::string("abc"));
    CHECK(asRequest(*b)->params.is_null());
}

TEST(rpc_parses_notifications_and_array_params) {
    auto n = parseIncoming(R"({"jsonrpc":"2.0","method":"EVENT","params":[1,2]})");
    REQUIRE(n.has_value());
    CHECK(!asRequest(*n)->id.has_value());
    CHECK(asRequest(*n)->params.is_array());
}

TEST(rpc_parses_responses) {
    auto ok = parseIncoming(R"({"jsonrpc":"2.0","id":3,"result":{"a":1}})");
    REQUIRE(ok.has_value());
    const auto* okResp = asResponse(*ok);
    REQUIRE(okResp != nullptr);
    REQUIRE(okResp->outcome.has_value());
    CHECK_EQ((*okResp->outcome)["a"].get<int>(), 1);

    auto err = parseIncoming(R"({"jsonrpc":"2.0","id":4,"error":{"code":-5,"message":"nope","data":{"k":1}}})");
    REQUIRE(err.has_value());
    const auto* errResp = asResponse(*err);
    REQUIRE(errResp != nullptr);
    REQUIRE(!errResp->outcome.has_value());
    CHECK_EQ(errResp->outcome.error().code, -5);
    CHECK_EQ(errResp->outcome.error().message, std::string("nope"));
    CHECK_EQ(errResp->outcome.error().data["k"].get<int>(), 1);
}

TEST(rpc_rejects_invalid_text_and_structure) {
    CHECK(rejects("not json", rpc_code::kParseError));
    CHECK(rejects("", rpc_code::kParseError));
    CHECK(rejects("[1,2]", rpc_code::kInvalidRequest));
    CHECK(rejects("[]", rpc_code::kInvalidRequest));
    CHECK(rejects("42", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"id":1,"method":"M"})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"1.0","id":1,"method":"M"})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"2.0","id":1})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"2.0","id":1,"method":5})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"2.0","id":null,"method":"M"})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"2.0","id":1.5,"method":"M"})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"2.0","id":1,"method":"M","params":"text"})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"2.0","id":1,"result":1,"error":{"code":1,"message":"x"}})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"2.0","result":1})", rpc_code::kInvalidRequest));
    CHECK(rejects(R"({"jsonrpc":"2.0","id":1,"error":{"code":"x","message":"x"}})", rpc_code::kInvalidRequest));
}

TEST(rpc_parse_failure_keeps_the_id_for_the_error_reply) {
    const auto r = parseIncoming(R"({"jsonrpc":"2.0","id":9,"method":5})");
    REQUIRE(!r.has_value());
    REQUIRE(r.error().id.has_value());
    CHECK_EQ(std::get<std::int64_t>(*r.error().id), std::int64_t{9});
    CHECK(!parseIncoming("not json").error().id.has_value());
}

TEST(rpc_serialization_round_trips) {
    const auto request = parseIncoming(makeRequest(RpcId{std::int64_t{11}}, "EXECUTE_AUTO_ORDER", Json{{"qty", 2}}));
    REQUIRE(request.has_value());
    CHECK_EQ(asRequest(*request)->method, std::string("EXECUTE_AUTO_ORDER"));
    CHECK_EQ(asRequest(*request)->params["qty"].get<int>(), 2);

    const auto result = parseIncoming(makeResult(RpcId{std::string("k")}, Json{{"ok", true}}));
    REQUIRE(result.has_value());
    CHECK((*asResponse(*result)->outcome)["ok"].get<bool>());

    const auto error = parseIncoming(makeError(RpcId{std::int64_t{1}}, RpcError{-32601, "no such method", nullptr}));
    REQUIRE(error.has_value());
    CHECK_EQ(asResponse(*error)->outcome.error().code, -32601);

    const Json anonymous = Json::parse(makeError(std::nullopt, RpcError{rpc_code::kParseError, "bad", nullptr}));
    CHECK(anonymous["id"].is_null());
    CHECK_EQ(anonymous["jsonrpc"].get<std::string>(), std::string("2.0"));
}

TEST(router_dispatches_to_the_registered_handler) {
    RpcRouter router;
    router.add("ECHO", [](const Json& params) -> Expected<Json, RpcError> { return Json{{"echo", params}}; });

    RpcRequest request{RpcId{std::int64_t{1}}, "ECHO", Json{{"a", 1}}};
    const Json reply = parsed(router.handle(request));
    CHECK_EQ(reply["id"].get<int>(), 1);
    CHECK_EQ(reply["result"]["echo"]["a"].get<int>(), 1);
}

TEST(router_reports_unknown_methods_but_stays_silent_for_notifications) {
    RpcRouter router;
    const Json reply = parsed(router.handle(RpcRequest{RpcId{std::int64_t{2}}, "NOPE", nullptr}));
    CHECK_EQ(reply["error"]["code"].get<int>(), rpc_code::kMethodNotFound);
    CHECK(!router.handle(RpcRequest{std::nullopt, "NOPE", nullptr}).has_value());
}

TEST(router_passes_handler_errors_through) {
    RpcRouter router;
    router.add("FAIL", [](const Json&) -> Expected<Json, RpcError> {
        return unexpected(RpcError{rpc_code::kInvalidParams, "bad field x", nullptr});
    });
    const Json reply = parsed(router.handle(RpcRequest{RpcId{std::int64_t{3}}, "FAIL", nullptr}));
    CHECK_EQ(reply["error"]["code"].get<int>(), rpc_code::kInvalidParams);
    CHECK_EQ(reply["error"]["message"].get<std::string>(), std::string("bad field x"));
}

TEST(router_hides_exception_text_from_the_peer) {
    RpcRouter router;
    router.add("BOOM", [](const Json&) -> Expected<Json, RpcError> { throw std::runtime_error("secret path /home/u"); });
    const auto text = router.handle(RpcRequest{RpcId{std::int64_t{4}}, "BOOM", nullptr});
    REQUIRE(text.has_value());
    CHECK_EQ(parsed(text)["error"]["code"].get<int>(), rpc_code::kInternalError);
    CHECK(text->find("secret") == std::string::npos);
}
