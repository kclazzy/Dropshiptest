#include "net/CurlTransport.hpp"

#include <curl/curl.h>

#include <memory>
#include <mutex>

namespace dropship::net {

namespace {

void ensureCurlInitialized() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct EasyDeleter {
    void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
};
struct HeaderListDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};

struct Sink {
    std::string body;
    std::size_t limit = 0;
    bool overflowed = false;
};

std::size_t onBody(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto* sink = static_cast<Sink*>(userdata);
    const std::size_t bytes = size * count;
    if (sink->body.size() + bytes > sink->limit) {
        sink->overflowed = true;
        return 0;  // сообщаем libcurl об ошибке записи — передача прерывается
    }
    sink->body.append(data, bytes);
    return bytes;
}

TransportError failure(std::string message) { return TransportError{std::move(message)}; }

}  // namespace

CurlTransport::CurlTransport(std::size_t maxResponseBytes) : maxResponseBytes_(maxResponseBytes) {
    ensureCurlInitialized();
}

Expected<HttpResponse, TransportError> CurlTransport::send(const HttpRequest& request) {
    std::unique_ptr<CURL, EasyDeleter> easy(curl_easy_init());
    if (!easy) return unexpected(failure("cannot initialize libcurl"));

    Sink sink;
    sink.limit = maxResponseBytes_;
    char errorBuffer[CURL_ERROR_SIZE] = {0};

    CURL* handle = easy.get();
    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);  // обязательно для многопоточных программ
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min<std::int64_t>(request.timeout.count(), 10'000)));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &onBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);

    std::unique_ptr<curl_slist, HeaderListDeleter> headers;
    if (request.method == HttpRequest::Method::Post) {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        headers.reset(curl_slist_append(headers.release(), "Content-Type: application/json"));
    }
    for (const auto& [name, value] : request.headers) {
        headers.reset(curl_slist_append(headers.release(), (name + ": " + value).c_str()));
    }
    if (headers) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());

    const CURLcode rc = curl_easy_perform(handle);
    if (sink.overflowed) return unexpected(failure("response is too large"));
    if (rc != CURLE_OK) {
        return unexpected(failure(errorBuffer[0] != '\0' ? std::string(errorBuffer) : std::string(curl_easy_strerror(rc))));
    }

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    return HttpResponse{static_cast<int>(status), std::move(sink.body)};
}

}  // namespace dropship::net
