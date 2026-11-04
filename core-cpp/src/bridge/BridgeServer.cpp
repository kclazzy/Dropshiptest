#include "bridge/BridgeServer.hpp"

#include <openssl/crypto.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace dropship::bridge {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

constexpr std::chrono::seconds kHandshakeTimeout{5};

// Браузер всегда присылает Origin. Страницы сайтов (http/https) здесь не нужны;
// допускаются только расширения. Клиенты без Origin — не браузеры, их защищает токен.
bool isAllowedOrigin(std::string_view origin) {
    return origin.empty() || origin.rfind("chrome-extension://", 0) == 0 || origin.rfind("moz-extension://", 0) == 0;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

RpcError notConnected(const char* why) { return RpcError{rpc_code::kExtensionNotConnected, why, nullptr}; }

class Session;

// Всё состояние сервера. Поля, помеченные «io», трогаются только из потока ввода-вывода.
class Core {
public:
    Core(BridgeConfig cfg, std::shared_ptr<const RpcRouter> r)
        : config(std::move(cfg)), router(std::move(r)), acceptor(ioc), acceptBackoff(ioc) {}
    ~Core() { stop(); }

    Expected<void, std::string> listen();
    void stop();
    std::future<CallResult> call(std::string method, Json params, std::chrono::milliseconds timeout);

    void activate(const std::shared_ptr<Session>& session);  // io
    void deactivate(Session* session);                       // io
    void resolve(std::int64_t id, CallResult result);        // io

    const BridgeConfig config;
    const std::shared_ptr<const RpcRouter> router;
    std::atomic<bool> connected{false};
    std::atomic<bool> stopping{false};
    std::uint16_t boundPort = 0;
    std::unordered_set<Session*> sessions;  // io; объявлен до ioc, чтобы пережить его уничтожение

private:
    struct Pending {
        std::shared_ptr<std::promise<CallResult>> promise;
        std::shared_ptr<net::steady_timer> timer;
    };

    void doAccept();
    void shutdownOnIoThread();
    void failAllPending(const RpcError& error);

    std::shared_ptr<Session> active_;                             // io
    std::int64_t nextId_ = 1;                                     // io
    std::unordered_map<std::int64_t, Pending> pending_;          // io
    net::io_context ioc{1};
    tcp::acceptor acceptor;
    net::steady_timer acceptBackoff;
    std::optional<net::executor_work_guard<net::io_context::executor_type>> guard_;
    std::jthread thread_;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(Core& core, tcp::socket&& socket)
        : core_(core), ws_(std::move(socket)), authTimer_(ws_.get_executor()) {
        core_.sessions.insert(this);
    }
    ~Session() { core_.sessions.erase(this); }

    void run() {
        auto& stream = beast::get_lowest_layer(ws_);
        stream.expires_after(kHandshakeTimeout);
        parser_.body_limit(8 * 1024);
        http::async_read(stream, buffer_, parser_, beast::bind_front_handler(&Session::onHttp, shared_from_this()));
    }

    void send(std::string text) {
        if (closed_) return;
        outbox_.push_back(std::move(text));
        if (!writing_) doWrite();
    }

    void hardClose() {
        closed_ = true;
        authTimer_.cancel();
        beast::get_lowest_layer(ws_).close();
        core_.deactivate(this);
    }

private:
    void reject(http::status status, const char* reason) {
        auto response = std::make_shared<http::response<http::string_body>>(status, parser_.get().version());
        response->set(http::field::content_type, "text/plain");
        response->keep_alive(false);
        response->body() = reason;
        response->prepare_payload();
        http::async_write(beast::get_lowest_layer(ws_), *response,
                          [self = shared_from_this(), response](beast::error_code, std::size_t) { self->hardClose(); });
    }

    void onHttp(beast::error_code ec, std::size_t) {
        if (ec) return hardClose();
        const auto& request = parser_.get();
        if (!websocket::is_upgrade(request)) return reject(http::status::upgrade_required, "WebSocket upgrade required");
        if (!isAllowedOrigin(request[http::field::origin])) return reject(http::status::forbidden, "forbidden origin");
        if (request.target() != "/ws") return reject(http::status::not_found, "not found");

        beast::get_lowest_layer(ws_).expires_never();
        websocket::stream_base::timeout timeouts;
        timeouts.handshake_timeout = kHandshakeTimeout;
        timeouts.idle_timeout = core_.config.idleTimeout;
        timeouts.keep_alive_pings = false;
        ws_.set_option(timeouts);
        ws_.read_message_max(core_.config.maxMessageBytes);
        ws_.async_accept(request, beast::bind_front_handler(&Session::onAccept, shared_from_this()));
    }

    void onAccept(beast::error_code ec) {
        if (ec) return hardClose();
        buffer_.consume(buffer_.size());
        authTimer_.expires_after(core_.config.authTimeout);
        authTimer_.async_wait([self = shared_from_this()](beast::error_code timerEc) {
            if (!timerEc && !self->authed_) self->hardClose();
        });
        doRead();
    }

    void doRead() { ws_.async_read(buffer_, beast::bind_front_handler(&Session::onRead, shared_from_this())); }

    void onRead(beast::error_code ec, std::size_t) {
        if (ec) return hardClose();
        if (!ws_.got_text()) return hardClose();  // бинарные кадры протоколом не предусмотрены
        std::string text = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        handle(text);
        if (!closed_) doRead();
    }

    void unauthorized() {
        closeAfterWrite_ = true;
        send(makeError(std::nullopt, RpcError{rpc_code::kUnauthorized, "unauthorized", nullptr}));
    }

    void handle(const std::string& text) {
        auto parsed = parseIncoming(text);
        if (!authed_) return authenticate(parsed);

        if (!parsed) {
            send(makeError(parsed.error().id, parsed.error().error));
        } else if (const auto* request = std::get_if<RpcRequest>(&*parsed)) {
            if (auto reply = core_.router->handle(*request)) send(std::move(*reply));
        } else if (const auto* response = std::get_if<RpcResponse>(&*parsed)) {
            if (const auto* id = std::get_if<std::int64_t>(&response->id)) core_.resolve(*id, response->outcome);
        }
    }

    void authenticate(const Expected<Incoming, ParseFailure>& parsed) {
        const RpcRequest* request = parsed ? std::get_if<RpcRequest>(&*parsed) : nullptr;
        if (request == nullptr || request->method != "AUTH" || !request->id || !request->params.is_object() ||
            !request->params.contains("token") || !request->params["token"].is_string() ||
            !constantTimeEquals(request->params["token"].get<std::string>(), core_.config.authToken)) {
            return unauthorized();
        }
        authed_ = true;
        authTimer_.cancel();
        core_.activate(shared_from_this());
        send(makeResult(*request->id, Json{{"ok", true}, {"protocol", kProtocolVersion}}));
    }

    void doWrite() {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(net::buffer(outbox_.front()), beast::bind_front_handler(&Session::onWrite, shared_from_this()));
    }

    void onWrite(beast::error_code ec, std::size_t) {
        writing_ = false;
        if (ec) return hardClose();
        outbox_.pop_front();
        if (!outbox_.empty()) return doWrite();
        if (closeAfterWrite_) hardClose();
    }

    Core& core_;
    websocket::stream<beast::tcp_stream> ws_;
    net::steady_timer authTimer_;
    beast::flat_buffer buffer_;
    http::request_parser<http::string_body> parser_;
    std::deque<std::string> outbox_;
    bool writing_ = false;
    bool authed_ = false;
    bool closed_ = false;
    bool closeAfterWrite_ = false;
};

// ---- Core ----

Expected<void, std::string> Core::listen() {
    if (config.authToken.size() < kMinTokenLength) {
        return unexpected("auth token must be at least " + std::to_string(kMinTokenLength) + " characters");
    }
    beast::error_code ec;
    const auto address = net::ip::make_address(config.bindAddress, ec);
    if (ec || !address.is_loopback()) return unexpected(std::string("bind address must be a loopback IP address"));

    const tcp::endpoint endpoint(address, config.port);
    acceptor.open(endpoint.protocol(), ec);
    if (!ec) {
#ifndef _WIN32
        acceptor.set_option(net::socket_base::reuse_address(true), ec);  // на Windows это позволило бы занять чужой порт
#endif
    }
    if (!ec) acceptor.bind(endpoint, ec);
    if (!ec) acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        return unexpected("cannot listen on " + config.bindAddress + ":" + std::to_string(config.port) + ": " + ec.message());
    }
    boundPort = acceptor.local_endpoint().port();

    guard_.emplace(net::make_work_guard(ioc));
    doAccept();
    thread_ = std::jthread([this](std::stop_token token) {
        std::stop_callback onStop(token, [this] {
            stopping = true;
            net::post(ioc, [this] { shutdownOnIoThread(); });
        });
        ioc.run();
    });
    return {};
}

void Core::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    thread_.join();
}

void Core::doAccept() {
    acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (stopping || ec == net::error::operation_aborted) return;
        if (ec) {  // например, закончились дескрипторы: подождать и продолжить, а не крутиться вхолостую
            acceptBackoff.expires_after(std::chrono::milliseconds(100));
            acceptBackoff.async_wait([this](beast::error_code waitEc) {
                if (!waitEc && !stopping) doAccept();
            });
            return;
        }
        std::make_shared<Session>(*this, std::move(socket))->run();
        doAccept();
    });
}

void Core::shutdownOnIoThread() {
    beast::error_code ec;
    acceptor.close(ec);
    acceptBackoff.cancel();
    const std::vector<Session*> open(sessions.begin(), sessions.end());
    for (Session* session : open) session->hardClose();
    failAllPending(notConnected("bridge is shutting down"));
    active_.reset();
    connected = false;
    guard_.reset();  // run() завершится, когда обработчики закончатся
}

void Core::activate(const std::shared_ptr<Session>& session) {
    if (active_ && active_ != session) {
        const auto previous = std::move(active_);
        active_ = session;  // раньше hardClose(), чтобы deactivate предыдущей сессии ничего не сбросил
        failAllPending(notConnected("connection replaced by a new extension session"));
        previous->hardClose();
    }
    active_ = session;
    connected = true;
}

void Core::deactivate(Session* session) {
    if (!active_ || active_.get() != session) return;
    active_.reset();
    connected = false;
    failAllPending(notConnected("extension disconnected"));
}

void Core::failAllPending(const RpcError& error) {
    auto taken = std::move(pending_);
    pending_.clear();
    for (auto& [id, entry] : taken) {
        entry.timer->cancel();
        entry.promise->set_value(CallResult(unexpected(error)));
    }
}

void Core::resolve(std::int64_t id, CallResult result) {
    const auto it = pending_.find(id);
    if (it == pending_.end()) return;  // ответ опоздал или чужой id
    it->second.timer->cancel();
    it->second.promise->set_value(std::move(result));
    pending_.erase(it);
}

std::future<CallResult> Core::call(std::string method, Json params, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<CallResult>>();
    auto future = promise->get_future();
    if (stopping) {
        promise->set_value(CallResult(unexpected(notConnected("bridge is shut down"))));
        return future;
    }
    net::post(ioc, [this, promise, method = std::move(method), params = std::move(params), timeout]() mutable {
        if (!active_) {
            promise->set_value(CallResult(unexpected(notConnected("extension is not connected"))));
            return;
        }
        const std::int64_t id = nextId_++;
        auto timer = std::make_shared<net::steady_timer>(ioc, timeout);
        pending_.emplace(id, Pending{promise, timer});
        timer->async_wait([this, id](beast::error_code ec) {
            if (!ec) resolve(id, CallResult(unexpected(RpcError{rpc_code::kTimeout, "extension did not answer in time", nullptr})));
        });
        active_->send(makeRequest(RpcId{id}, method, params));
    });
    return future;
}

}  // namespace

struct BridgeServer::Impl {
    Impl(BridgeConfig config, std::shared_ptr<const RpcRouter> router) : core(std::move(config), std::move(router)) {}
    Core core;
};

BridgeServer::BridgeServer() = default;

BridgeServer::~BridgeServer() {
    if (impl_) impl_->core.stop();
}

Expected<std::unique_ptr<BridgeServer>, std::string> BridgeServer::start(BridgeConfig config,
                                                                         std::shared_ptr<const RpcRouter> router) {
    auto server = std::unique_ptr<BridgeServer>(new BridgeServer());
    server->impl_ = std::make_unique<Impl>(std::move(config), std::move(router));
    if (auto listening = server->impl_->core.listen(); !listening) return unexpected(listening.error());
    return server;
}

std::uint16_t BridgeServer::port() const { return impl_->core.boundPort; }

bool BridgeServer::extensionConnected() const { return impl_->core.connected; }

std::future<CallResult> BridgeServer::call(std::string method, Json params, std::chrono::milliseconds timeout) {
    return impl_->core.call(std::move(method), std::move(params), timeout);
}

}  // namespace dropship::bridge
