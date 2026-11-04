cmake_minimum_required(VERSION 3.25)

project(dropship_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(DROPSHIP_BUILD_TESTS "Build unit tests" ON)
option(DROPSHIP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Boost 1.78 REQUIRED)

function(dropship_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if(DROPSHIP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(DROPSHIP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

add_library(dropship_core STATIC
  src/pricing/PricingEngine.cpp
  src/pricing/RuleRepository.cpp
  src/db/Connection.cpp
  src/db/DatabaseManager.cpp
  src/db/Migrations.cpp
  src/db/TaskQueue.cpp
  src/db/Bootstrap.cpp
  src/update/Updater.cpp
  src/monitor/PageParser.cpp
  src/monitor/ProxyPool.cpp
  src/monitor/DomainLimiter.cpp
  src/monitor/StateApplier.cpp
  src/monitor/PriceSync.cpp
  src/store/RateLimiter.cpp
  src/store/CredentialVault.cpp
  src/store/OzonAdapter.cpp
  src/store/AvitoAdapter.cpp
  src/store/StoreSync.cpp
  src/app/Cli.cpp
  src/orders/Orders.cpp
  src/util/Sha256.cpp
  src/net/CurlTransport.cpp
  src/importer/UrlNormalizer.cpp
  src/importer/ProductImport.cpp
  src/ai/ContentSanitizer.cpp
  src/ai/Listing.cpp
  src/ai/OllamaClient.cpp
  src/ai/CloudClient.cpp
  src/ai/AiFactory.cpp
  src/ai/AiWorker.cpp
  src/bridge/JsonRpc.cpp
  src/bridge/RpcRouter.cpp
  src/bridge/SupplierSessionStore.cpp
  src/bridge/TokenStore.cpp
  src/bridge/BridgeServer.cpp
  src/bridge/Handlers.cpp
)
target_include_directories(dropship_core PUBLIC src)
target_link_libraries(dropship_core PUBLIC SQLite::SQLite3 Threads::Threads OpenSSL::Crypto
  CURL::libcurl nlohmann_json::nlohmann_json Boost::headers)
target_compile_features(dropship_core PUBLIC cxx_std_20)
target_compile_definitions(dropship_core PUBLIC DROPSHIP_VERSION="${PROJECT_VERSION}")
dropship_set_warnings(dropship_core)

add_executable(dropship_core_app src/app/main.cpp)
target_link_libraries(dropship_core_app PRIVATE dropship_core)
dropship_set_warnings(dropship_core_app)

if(DROPSHIP_BUILD_TESTS)
  enable_testing()
  add_executable(dropship_tests
    tests/test_main.cpp
    tests/test_domain.cpp
    tests/test_pricing.cpp
    tests/test_database.cpp
    tests/test_task_queue.cpp
    tests/test_sanitizer.cpp
    tests/test_ollama.cpp
    tests/test_jsonrpc.cpp
    tests/test_import.cpp
    tests/test_session_and_token.cpp
    tests/test_http_transport.cpp
    tests/test_bridge.cpp
    tests/test_cloud.cpp
    tests/test_ai_factory.cpp
    tests/test_ai_worker.cpp
    tests/test_bootstrap.cpp
    tests/test_updater.cpp
    tests/test_page_parser.cpp
    tests/test_proxy_limiter.cpp
    tests/test_monitor_apply.cpp
    tests/test_price_sync.cpp
    tests/test_store_basics.cpp
    tests/test_store_adapters.cpp
    tests/test_store_sync.cpp
    tests/test_store_registry.cpp
    tests/test_cli.cpp
    tests/test_orders.cpp
  )
  target_link_libraries(dropship_tests PRIVATE dropship_core)
  dropship_set_warnings(dropship_tests)
  add_test(NAME unit COMMAND dropship_tests)
endif()
