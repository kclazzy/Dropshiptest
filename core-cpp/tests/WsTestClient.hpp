#pragma once

// Синхронный WebSocket-клиент для тестов моста (Boost.Beast) с жёсткими таймаутами.

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <optional>
#include <string>

#include "core/Json.hpp"

namespace testnet {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

class WsTestClient {
public:
    explicit WsTestClient(unsigned short port, std::optional<std::string> origin = std::nullopt) : ws_(ioc_) {
        boost::system::error_code ec;
        auto& lowest = beast::get_lowest_layer(ws_);
        lowest.expires_after(3s);
        lowest.socket().connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port), ec);
        if (ec) {
            handshakeError_ = ec.message();
            return;
        }
        ws_.set_option(websocket::stream_base::decorator([origin](websocket::request_type& req) {
            if (origin) req.set(http::field::origin, *origin);
        }));
        websocket::response_type response;
        lowest.expires_after(3s);
        ws_.handshake(response, "127.0.0.1:" + std::to_string(port), "/ws", ec);
        httpStatus_ = static_cast<int>(response.result_int());
        if (ec) {
            handshakeError_ = ec.message();
            return;
        }
        connected_ = true;
    }

    bool connected() const { return connected_; }
    int httpStatus() const { return httpStatus_; }
    bool closed() const { return closed_; }

    bool send(const std::string& text) {
        boost::system::error_code ec;
        beast::get_lowest_layer(ws_).expires_after(3s);
        ws_.text(true);
        ws_.write(net::buffer(text), ec);
        if (ec) closed_ = true;
        return !ec;
    }

    bool sendBinary(const std::string& data) {
        boost::system::error_code ec;
        beast::get_lowest_layer(ws_).expires_after(3s);
        ws_.binary(true);
        ws_.write(net::buffer(data), ec);
        ws_.binary(false);
        if (ec) closed_ = true;
        return !ec;
    }

    // Следующее текстовое сообщение; nullopt при таймауте или закрытии соединения.
    std::optional<std::string> receive(std::chrono::milliseconds timeout = 3s) {
        if (closed_) return std::nullopt;
        beast::flat_buffer buffer;
        boost::system::error_code ec;
        beast::get_lowest_layer(ws_).expires_after(timeout);
        ws_.read(buffer, ec);
        if (ec) {
            if (ec != beast::error::timeout) closed_ = true;
            return std::nullopt;
        }
        return beast::buffers_to_string(buffer.data());
    }

    // Читает до закрытия соединения; true, если оно закрылось в срок.
    bool waitClosed(std::chrono::milliseconds timeout = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!closed_ && std::chrono::steady_clock::now() < deadline) {
            receive(std::max<std::chrono::milliseconds>(
                50ms, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())));
        }
        return closed_;
    }

    const std::string& handshakeError() const { return handshakeError_; }

private:
    net::io_context ioc_;
    websocket::stream<beast::tcp_stream> ws_;
    bool connected_ = false;
    bool closed_ = false;
    int httpStatus_ = 0;
    std::string handshakeError_;
};

}  // namespace testnet
