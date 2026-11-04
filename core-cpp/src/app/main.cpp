// Запускаемое ядро: БД + мост для расширения + ИИ-воркер. Остановка — SIGINT/SIGTERM.
// Каталог данных: --data-dir, иначе $DROPSHIP_DATA_DIR, иначе ~/.dropship.

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
#include "ai/AiWorker.hpp"
#include "bridge/BridgeServer.hpp"
#include "bridge/Handlers.hpp"
#include "bridge/SupplierSessionStore.hpp"
#include "bridge/TokenStore.hpp"
#include "core/Json.hpp"
#include "db/DatabaseManager.hpp"
#include "net/CurlTransport.hpp"

namespace fs = std::filesystem;
using namespace dropship;

namespace {

std::atomic<bool> g_stop{false};
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
    auto token = bridge::loadOrCreateToken(dataDir / "bridge.token");
    if (!token) {
        logLine("token error: " + token.error());
        return 1;
    }

    bridge::SupplierSessionStore sessions;
    auto router = std::make_shared<bridge::RpcRouter>();
    bridge::registerCoreHandlers(*router, {**database, sessions, nowSeconds});

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

    logLine("Dropship core is running. Extension bridge: ws://127.0.0.1:" + std::to_string((*server)->port()));
    logLine("Access token file: " + (dataDir / "bridge.token").string());
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    logLine("stopping...");
    worker.reset();  // сначала воркер: он использует БД и генератор
    server->reset();
    return 0;
}
