#include "importer/UrlNormalizer.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace dropship::importer {

namespace {

constexpr std::size_t kMaxUrlBytes = 4096;

std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

bool isTrackingKey(std::string_view rawKey) {
    static constexpr std::array<std::string_view, 7> kExact{"gclid", "fbclid", "yclid", "msclkid",
                                                            "_openstat", "mc_cid", "mc_eid"};
    const std::string key = lowerAscii(rawKey);
    if (key.rfind("utm_", 0) == 0) return true;
    return std::find(kExact.begin(), kExact.end(), key) != kExact.end();
}

bool isPort(std::string_view text, int& value) {
    if (text.empty() || text.size() > 5) return false;
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    return value >= 1 && value <= 65535;
}

std::string normalizedQuery(std::string_view query) {
    std::vector<std::string_view> kept;
    while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string_view segment = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        if (segment.empty()) continue;
        if (isTrackingKey(segment.substr(0, segment.find('=')))) continue;
        kept.push_back(segment);
    }
    std::sort(kept.begin(), kept.end());
    std::string joined;
    for (const auto segment : kept) {
        if (!joined.empty()) joined += '&';
        joined += segment;
    }
    return joined;
}

}  // namespace

std::optional<std::string> normalizeUrl(std::string_view url) {
    if (url.empty() || url.size() > kMaxUrlBytes) return std::nullopt;
    for (const unsigned char c : url) {
        if (c <= 0x20 || c == 0x7F) return std::nullopt;
    }

    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) return std::nullopt;
    const std::string scheme = lowerAscii(url.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https") return std::nullopt;

    std::string_view rest = url.substr(schemeEnd + 3);
    rest = rest.substr(0, rest.find('#'));
    const auto authorityEnd = rest.find_first_of("/?");
    const std::string_view authority = rest.substr(0, authorityEnd);
    const std::string_view tail = authorityEnd == std::string_view::npos ? std::string_view{} : rest.substr(authorityEnd);
    // user@host скрывает настоящий адрес за «доверенным» именем.
    if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;

    std::string_view host = authority;
    std::string_view portText;
    bool hasPort = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = authority.substr(0, close + 1);
        const auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return std::nullopt;
            portText = after.substr(1);
            hasPort = true;
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        portText = authority.substr(colon + 1);
        hasPort = true;
        if (host.find(':') != std::string_view::npos) return std::nullopt;
    }
    if (host.empty()) return std::nullopt;

    int port = 0;
    if (hasPort && !isPort(portText, port)) return std::nullopt;
    const bool defaultPort = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);

    const auto queryStart = tail.find('?');
    std::string path(tail.substr(0, queryStart));
    if (path.empty()) path = "/";
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const std::string query =
        queryStart == std::string_view::npos ? std::string{} : normalizedQuery(tail.substr(queryStart + 1));

    std::string result = scheme + "://" + lowerAscii(host);
    if (hasPort && !defaultPort) result += ":" + std::to_string(port);
    result += path;
    if (!query.empty()) result += "?" + query;
    return result;
}

}  // namespace dropship::importer
