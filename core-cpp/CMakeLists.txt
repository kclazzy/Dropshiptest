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
)
target_include_directories(dropship_core PUBLIC src)
target_link_libraries(dropship_core PUBLIC SQLite::SQLite3 Threads::Threads)
target_compile_features(dropship_core PUBLIC cxx_std_20)
dropship_set_warnings(dropship_core)

if(DROPSHIP_BUILD_TESTS)
  enable_testing()
  add_executable(dropship_tests
    tests/test_main.cpp
    tests/test_domain.cpp
    tests/test_pricing.cpp
  )
  target_link_libraries(dropship_tests PRIVATE dropship_core)
  dropship_set_warnings(dropship_tests)
  add_test(NAME unit COMMAND dropship_tests)
endif()
