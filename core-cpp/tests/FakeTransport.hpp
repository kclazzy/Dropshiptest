#pragma once

#include <deque>
#include <vector>

#include "net/HttpTransport.hpp"

namespace testfakes {

// Транспорт-заглушка: отдаёт заранее заданные ответы и запоминает запросы.
class FakeTransport : public dropship::net::IHttpTransport {
public:
    using Reply = dropship::Expected<dropship::net::HttpResponse, dropship::net::TransportError>;

    void reply(int status, std::string body) { replies.emplace_back(dropship::net::HttpResponse{status, std::move(body)}); }
    void fail(std::string message) {
        replies.emplace_back(dropship::unexpected(dropship::net::TransportError{std::move(message)}));
    }

    Reply send(const dropship::net::HttpRequest& request) override {
        requests.push_back(request);
        if (replies.empty()) return dropship::unexpected(dropship::net::TransportError{"no reply queued"});
        Reply next = std::move(replies.front());
        replies.pop_front();
        return next;
    }

    std::deque<Reply> replies;
    std::vector<dropship::net::HttpRequest> requests;
};

}  // namespace testfakes
