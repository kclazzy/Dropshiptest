#pragma once

// Проверка обновлений: сравнивает версию ядра с последним релизом на GitHub.
// Сама установка выполняется скриптом-запускающим (scripts/run.sh / run.cmd): ядро лишь
// завершается с кодом kExitCodeUpdate, а скрипт обновляет файлы и запускает ядро снова.
// Так работающий процесс не подменяет собственный исполняемый файл.

#include <optional>
#include <string>
#include <string_view>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "net/HttpTransport.hpp"

namespace dropship::update {

inline constexpr int kExitCodeUpdate = 75;  // EX_TEMPFAIL: «запустите меня снова после обновления»

struct Version {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
};

// "v1.2.3", "1.2" и "1.2.3-beta" (суффикс отбрасывается). Остальное — nullopt.
std::optional<Version> parseVersion(std::string_view text);
std::string toString(const Version& version);

struct UpdateInfo {
    std::string current;
    std::string latest;        // версия из тега последнего релиза
    bool updateAvailable = false;
    std::string releaseUrl;    // страница релиза для пользователя
    std::string notes;         // описание релиза (обрезано)
};

// Ответ GET /repos/{owner}/{repo}/releases/latest. Черновики и пререлизы не предлагаются.
Expected<UpdateInfo, std::string> evaluateRelease(const Json& release, std::string_view currentVersion);

// repo в виде "owner/name"; допустимы только [A-Za-z0-9._-] — адрес строится из этой строки.
Expected<UpdateInfo, std::string> checkForUpdate(net::IHttpTransport& transport, std::string_view repo,
                                                 std::string_view currentVersion);

}  // namespace dropship::update
