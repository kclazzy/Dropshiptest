#include "bridge/SupplierSessionStore.hpp"

#include <algorithm>

namespace dropship::bridge {

namespace {

constexpr std::size_t kMaxCookies = 200;
constexpr std::size_t kMaxNameBytes = 256;
constexpr std::size_t kMaxValueBytes = 4096;
constexpr std::size_t kMaxUserAgentBytes = 512;
constexpr std::size_t kMaxHostBytes = 253;
constexpr std::size_t kMaxLabelBytes = 63;

std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

bool hasControlChars(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F;
    });
}

bool validHostname(std::string_view host) {
    if (host.empty() || host.size() > kMaxHostBytes) return false;
    std::size_t labelLength = 0;
    for (const char c : host) {
        if (c == '.') {
            if (labelLength == 0) return false;
            labelLength = 0;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok || ++labelLength > kMaxLabelBytes) return false;
    }
    return labelLength > 0;  // не заканчивается точкой
}

bool validCookieName(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameBytes || hasControlChars(name)) return false;
    return name.find_first_of(" ()<>@,;:\\\"/[]?={}") == std::string_view::npos;
}

bool validCookieValue(std::string_view value) {
    return value.size() <= kMaxValueBytes && !hasControlChars(value) && value.find(';') == std::string_view::npos;
}

bool matchesDomain(std::string_view host, std::string_view domain) {
    if (host == domain) return true;
    return host.size() > domain.size() && host.substr(host.size() - domain.size()) == domain &&
           host[host.size() - domain.size() - 1] == '.';
}

}  // namespace

Expected<void, std::string> SupplierSessionStore::update(std::string_view rawDomain, const std::vector<Cookie>& cookies,
                                                         std::string_view userAgent, UnixSeconds now) {
    std::string domain = lowerAscii(rawDomain);
    if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
    if (!validHostname(domain)) return unexpected(std::string("invalid cookie domain"));
    if (cookies.size() > kMaxCookies) return unexpected(std::string("too many cookies"));
    if (userAgent.size() > kMaxUserAgentBytes || hasControlChars(userAgent)) {
        return unexpected(std::string("invalid user agent"));
    }

    SupplierSession session;
    for (const auto& cookie : cookies) {
        if (!validCookieName(cookie.name)) return unexpected(std::string("invalid cookie name"));
        if (!validCookieValue(cookie.value)) return unexpected(std::string("invalid cookie value"));
        if (!session.cookieHeader.empty()) session.cookieHeader += "; ";
        session.cookieHeader += cookie.name + "=" + cookie.value;
    }
    session.userAgent = std::string(userAgent);
    session.updatedAt = now;

    std::lock_guard lock(mutex_);
    sessions_.insert_or_assign(std::move(domain), std::move(session));
    return {};
}

std::optional<SupplierSession> SupplierSessionStore::lookup(std::string_view rawHost) const {
    const std::string host = lowerAscii(rawHost);
    std::lock_guard lock(mutex_);
    const SupplierSession* best = nullptr;
    std::size_t bestLength = 0;
    for (const auto& [domain, session] : sessions_) {
        if (matchesDomain(host, domain) && domain.size() >= bestLength) {
            best = &session;
            bestLength = domain.size();
        }
    }
    if (best == nullptr) return std::nullopt;
    return *best;
}

std::size_t SupplierSessionStore::size() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

}  // namespace dropship::bridge
