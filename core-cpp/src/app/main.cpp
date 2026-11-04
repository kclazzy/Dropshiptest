// Запускаемое ядро: БД + мост для расширения + ИИ-воркер. Остановка — SIGINT/SIGTERM.
// Каталог данных: --data-dir, иначе $DROPSHIP_DATA_DIR, иначе ~/.dropship.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "ai/AiFactory.hpp"
#include "app/Cli.hpp"
#include "store/StoreSync.hpp"
#include "ai/AiWorker.hpp"
#include "bridge/BridgeServer.hpp"
#include "bridge/Handlers.hpp"
#include "bridge/SupplierSessionStore.hpp"
#include "bridge/TokenStore.hpp"
#include "core/Json.hpp"
#include "db/Bootstrap.hpp"
#include "db/DatabaseManager.hpp"
#include "monitor/PriceSync.hpp"
#include "net/CurlTransport.hpp"
#include "update/Updater.hpp"

namespace fs = std::filesystem;
using namespace dropship;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_exitCode{0};
extern "C" void onSignal(int) { g_stop = true; }

void logLine(const std::string& text) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::cerr << stamp << "  " << text << std::endl;
}

UnixSeconds nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

fs::path defaultDataDir() {
    if (const char* dir = std::getenv("DROPSHIP_DATA_DIR"); dir && *dir) return dir;
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    return fs::path(home ? home : ".") / ".dropship";
}

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// Секрет: переменная окружения, затем файл <data>/secrets/<ИМЯ> (удобно для Docker secrets).
ai::ApiKeyLookup makeKeyLookup(const fs::path& dataDir) {
    return [dataDir](const std::string& name) -> std::optional<std::string> {
        if (const char* value = std::getenv(name.c_str()); value && *value) return std::string(value);
        const fs::path file = dataDir / "secrets" / name;
        if (!fs::is_regular_file(file)) return std::nullopt;
        std::string text = readFile(file);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
        if (text.empty()) return std::nullopt;
        return text;
    };
}

}  // namespace

int main(int argc, char** argv) {
    fs::path dataDir = defaultDataDir();
    std::uint16_t port = 8765;
    std::vector<std::string> commandArgs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (commandArgs.empty() && cli::isCommand(arg)) {
            commandArgs.push_back(arg);
        } else if (!commandArgs.empty()) {
            commandArgs.push_back(arg);
        }
    }
    if (!commandArgs.empty()) {
        std::error_code mkdirError;
        fs::create_directories(dataDir, mkdirError);
        auto database = db::DatabaseManager::open(dataDir / "dropship.db");
        if (!database) {
            std::cerr << "database error: " << database.error().message << "\n";
            return 1;
        }
        auto vault = store::CredentialVault::openOrCreate(dataDir / "secret.key");
        if (!vault) {
            std::cerr << vault.error() << "\n";
            return 1;
        }
        return cli::run(commandArgs, **database, *vault, std::cin, std::cout, std::cerr, nowSeconds());
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else {
            std::cerr << "usage: dropship_core_app [--data-dir DIR] [--port N]\n";
            return arg == "--help" ? 0 : 2;
        }
    }

    std::error_code ec;
    fs::create_directories(dataDir, ec);
    if (ec) {
        logLine("cannot create data dir " + dataDir.string() + ": " + ec.message());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto database = db::DatabaseManager::open(dataDir / "dropship.db");
    if (!database) {
        logLine("database error: " + database.error().message);
        return 1;
    }
    if (const auto store = db::ensureDefaultStore(**database, nowSeconds()); !store) {
        logLine("database error: " + store.error().message);
        return 1;
    }
    auto token = bridge::loadOrCreateToken(dataDir / "bridge.token");
    if (!token) {
        logLine("token error: " + token.error());
        return 1;
    }

    bridge::SupplierSessionStore sessions;
    auto router = std::make_shared<bridge::RpcRouter>();
    monitor::DomainLimiter limiter;
    bridge::registerCoreHandlers(*router, {**database, sessions, nowSeconds, [&](const std::string& domain) {
        // Сессия обновлена (капча пройдена): снимаем ограничение домена и возвращаем карточки в мониторинг.
        limiter.unblock(domain);
        const auto resumed = monitor::resumeBlockedProducts(**database, domain);
        if (resumed && *resumed > 0) logLine("monitor: resumed " + std::to_string(*resumed) + " products of " + domain);
    }});

    // Обновление: проверка по релизам GitHub; установку делает скрипт запуска (код выхода 75).
    const Json settingsForUpdate = parseJson(readFile(dataDir / "settings.json"));
    const std::string updateRepo = settingsForUpdate.is_object() && settingsForUpdate.contains("update") &&
                                           settingsForUpdate["update"].value("repo", std::string{}).size() > 0
                                       ? settingsForUpdate["update"]["repo"].get<std::string>()
                                       : "kclazzy/Dropshiptest";
    const bool underLauncher = std::getenv("DROPSHIP_LAUNCHER") != nullptr;
    auto updateTransport = std::make_shared<net::CurlTransport>();
    router->add("UPDATE_CHECK", [updateTransport, updateRepo, underLauncher](const Json&) -> Expected<Json, bridge::RpcError> {
        const auto info = update::checkForUpdate(*updateTransport, updateRepo, DROPSHIP_VERSION);
        if (!info) return unexpected(bridge::RpcError{bridge::rpc_code::kUpdateFailed, info.error(), nullptr});
        return Json{{"current", info->current}, {"latest", info->latest}, {"update_available", info->updateAvailable},
                    {"release_url", info->releaseUrl}, {"notes", info->notes}, {"can_apply", underLauncher}};
    });
    router->add("UPDATE_APPLY", [underLauncher](const Json&) -> Expected<Json, bridge::RpcError> {
        if (!underLauncher) {
            return unexpected(bridge::RpcError{bridge::rpc_code::kUpdateFailed,
                                               "Ядро запущено без скрипта запуска (run.sh / run.cmd): обновите вручную",
                                               nullptr});
        }
        g_exitCode = update::kExitCodeUpdate;
        g_stop = true;  // ответ уйдёт до остановки: цикл ожидания спит 200 мс
        return Json{{"restarting", true}};
    });

    bridge::BridgeConfig config;
    config.port = port;
    config.authToken = *token;
    auto server = bridge::BridgeServer::start(config, router);
    if (!server) {
        logLine("bridge error: " + server.error());
        return 1;
    }

    // ИИ: настройки читаются из settings.json (создаётся с локальной Ollama по умолчанию).
    const fs::path settingsFile = dataDir / "settings.json";
    if (!fs::exists(settingsFile)) {
        std::ofstream(settingsFile) << "{\n  \"ai\": { \"provider\": \"ollama\", \"language\": \"ru\" }\n}\n";
    }
    const Json settingsJson = parseJson(readFile(settingsFile));
    std::unique_ptr<ai::IContentGenerator> generator;
    std::unique_ptr<ai::AiWorker> worker;
    if (settingsJson.is_discarded()) {
        logLine("settings.json is not valid JSON: AI disabled");
    } else if (const auto settings = ai::parseAiSettings(settingsJson); !settings) {
        logLine("settings.json: " + settings.error() + " — AI disabled");
    } else {
        auto made = ai::makeContentGenerator(*settings, std::make_shared<net::CurlTransport>(), makeKeyLookup(dataDir));
        if (!made) {
            logLine("AI provider: " + made.error() + " — AI disabled");
        } else {
            generator = std::move(*made);
            worker = std::make_unique<ai::AiWorker>(**database, *generator, nowSeconds, logLine);
            logLine("AI provider: " + generator->name());
        }
    }

    // Мониторинг цен и остатков: планировщик + воркеры. Отключается "monitor": {"workers": 0}.
    const int monitorWorkers = settingsJson.is_object() && settingsJson.contains("monitor") && settingsJson["monitor"].is_object()
                                   ? settingsJson["monitor"].value("workers", 4)
                                   : 4;
    monitor::MonitorConfig monitorConfig;
    if (settingsJson.is_object() && settingsJson.contains("monitor") && settingsJson["monitor"].is_object()) {
        monitorConfig.scheduleEvery = std::chrono::seconds(std::clamp(settingsJson["monitor"].value("schedule_seconds", 15), 1, 3600));
    }
    auto monitorTransport = std::make_shared<net::CurlTransport>();
    monitor::ProxyPool proxyPool(**database);
    std::unique_ptr<monitor::MonitorService> monitorService;
    if (monitorWorkers > 0) {
        monitorService = std::make_unique<monitor::MonitorService>(
            monitor::MonitorContext{**database, *monitorTransport, sessions, proxyPool, limiter, monitorConfig}, nowSeconds,
            std::min(monitorWorkers, 32), logLine);
        logLine("Monitoring: " + std::to_string(std::min(monitorWorkers, 32)) + " workers");
    }

    // Отправка цен и остатков на витрины (Ozon, Avito). Учётные данные — в зашифрованном виде в БД.
    auto vault = store::CredentialVault::openOrCreate(dataDir / "secret.key");
    auto storeTransport = std::make_shared<net::CurlTransport>();
    std::unique_ptr<store::StoreRegistry> storeRegistry;
    std::unique_ptr<store::StoreSyncService> storeSync;
    if (!vault) {
        logLine("store sync disabled: " + vault.error());
    } else {
        storeRegistry = std::make_unique<store::StoreRegistry>(
            **database, *vault, *storeTransport,
            [] { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); });
        storeSync = std::make_unique<store::StoreSyncService>(**database, *storeRegistry, nowSeconds, logLine);
    }

    logLine(std::string("Dropship core ") + DROPSHIP_VERSION + " is running. Extension bridge: ws://127.0.0.1:" + std::to_string((*server)->port()) + "/ws");
    logLine("Access token file: " + (dataDir / "bridge.token").string());
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    logLine("stopping...");
    storeSync.reset();
    monitorService.reset();
    worker.reset();  // сначала воркер: он использует БД и генератор
    server->reset();
    return g_exitCode;
}
