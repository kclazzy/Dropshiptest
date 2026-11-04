#pragma once

// Крошечный HTTP-сервер для тестов: отдаёт заданный ответ и запоминает запросы.
// Слушает 127.0.0.1 на свободном порту.

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace testnet {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct RecordedRequest {
    std::string method;
    std::string target;
    std::string body;
    std::string contentType;
    std::string authorization;
    std::string xApiKey;
};

class TestHttpServer {
public:
    int status = 200;
    std::string responseBody = "{}";
    std::chrono::milliseconds delay{0};   // задержка перед ответом (для проверки таймаута)

    TestHttpServer() : acceptor_(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)) {
        thread_ = std::thread([this] { serve(); });
    }
    ~TestHttpServer() {
        stopping_ = true;
        // close() не пробуждает блокирующий accept() в другом потоке — будим его подключением.
        boost::system::error_code ec;
        tcp::socket waker(ioc_);
        waker.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port()), ec);
        if (thread_.joinable()) thread_.join();
        acceptor_.close(ec);
    }
    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    unsigned short port() const { return acceptor_.local_endpoint().port(); }
    std::string url(const std::string& path = "") const { return "http://127.0.0.1:" + std::to_string(port()) + path; }

    std::vector<RecordedRequest> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void serve() {
        while (!stopping_) {
            boost::system::error_code ec;
            tcp::socket socket(ioc_);
            acceptor_.accept(socket, ec);
            if (ec || stopping_) return;
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(socket, buffer, req, ec);
            if (ec) continue;
            {
                std::lock_guard lock(mutex_);
                requests_.push_back({std::string(req.method_string()), std::string(req.target()), req.body(),
                                     std::string(req[http::field::content_type]),
                                     std::string(req[http::field::authorization]), std::string(req["x-api-key"])});
            }
            std::this_thread::sleep_for(delay);
            http::response<http::string_body> res{static_cast<http::status>(status), req.version()};
            res.set(http::field::content_type, "application/json");
            res.keep_alive(false);
            res.body() = responseBody;
            res.prepare_payload();
            http::write(socket, res, ec);
            socket.shutdown(tcp::socket::shutdown_both, ec);
        }
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::vector<RecordedRequest> requests_;
};

}  // namespace testnet
