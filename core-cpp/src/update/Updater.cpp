#include "update/Updater.hpp"

#include <charconv>

namespace dropship::update {

namespace {

bool validRepo(std::string_view repo) {
    const auto slash = repo.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= repo.size()) return false;
    if (repo.find('/', slash + 1) != std::string_view::npos) return false;
    for (const char c : repo) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-' || c == '/';
        if (!ok) return false;
    }
    return repo.find("..") == std::string_view::npos;
}

bool parseNumber(std::string_view text, int& out) {
    if (text.empty() || text.size() > 6) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && end == text.data() + text.size();
}

std::string stringField(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

}  // namespace

std::optional<Version> parseVersion(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    if (const auto cut = text.find_first_of("-+"); cut != std::string_view::npos) text = text.substr(0, cut);
    int parts[3] = {0, 0, 0};
    int count = 0;
    while (!text.empty()) {
        if (count == 3) return std::nullopt;
        const auto dot = text.find('.');
        if (!parseNumber(text.substr(0, dot), parts[count])) return std::nullopt;
        ++count;
        if (dot == std::string_view::npos) break;
        text.remove_prefix(dot + 1);
        if (text.empty()) return std::nullopt;  // «1.2.»
    }
    if (count == 0) return std::nullopt;
    return Version{parts[0], parts[1], parts[2]};
}

std::string toString(const Version& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

Expected<UpdateInfo, std::string> evaluateRelease(const Json& release, std::string_view currentVersion) {
    const auto current = parseVersion(currentVersion);
    if (!current) return unexpected("current version is not valid: " + std::string(currentVersion));
    if (!release.is_object()) return unexpected(std::string("unexpected release response"));

    UpdateInfo info;
    info.current = toString(*current);
    info.latest = info.current;  // нет подходящего релиза — обновлять нечего

    const bool unpublished = release.value("draft", false) || release.value("prerelease", false);
    const auto latest = parseVersion(stringField(release, "tag_name"));
    if (unpublished || !latest) return info;

    info.latest = toString(*latest);
    info.updateAvailable = *latest > *current;
    info.releaseUrl = stringField(release, "html_url");
    info.notes = stringField(release, "body").substr(0, 2000);
    return info;
}

Expected<UpdateInfo, std::string> checkForUpdate(net::IHttpTransport& transport, std::string_view repo,
                                                 std::string_view currentVersion) {
    if (!validRepo(repo)) return unexpected(std::string("repository must look like owner/name"));
    net::HttpRequest request;
    request.url = "https://api.github.com/repos/" + std::string(repo) + "/releases/latest";
    request.headers = {{"Accept", "application/vnd.github+json"}, {"User-Agent", "dropship-core"}};
    request.timeout = std::chrono::seconds(10);
    const auto response = transport.send(request);
    if (!response) return unexpected("no connection to GitHub: " + response.error().message);
    if (response->status == 404) {
        // Релизов ещё нет: это не ошибка, просто обновлений нет.
        return evaluateRelease(Json::object(), currentVersion);
    }
    if (response->status != 200) return unexpected("GitHub answered HTTP " + std::to_string(response->status));
    const Json body = parseJson(response->body);
    if (body.is_discarded()) return unexpected(std::string("GitHub returned invalid JSON"));
    return evaluateRelease(body, currentVersion);
}

}  // namespace dropship::update
