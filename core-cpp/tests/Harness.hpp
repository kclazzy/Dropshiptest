#pragma once

// Минимальный тестовый каркас без внешних зависимостей.

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& currentFailures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

// Бросается REQUIRE: прерывает текущий тест.
struct Abort : std::exception {};

template <class T>
std::string show(const T& v) {
    if constexpr (requires(std::ostream& o) { o << v; }) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "<unprintable>";
    }
}

inline void fail(const char* file, int line, const std::string& what) {
    ++currentFailures();
    std::cerr << "    " << file << ":" << line << ": " << what << "\n";
}

}  // namespace testing

#define TEST(name)                                                  \
    static void name();                                             \
    static ::testing::Registrar registrar_##name(#name, name);      \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        const auto& va_ = (a);                                                   \
        const auto& vb_ = (b);                                                   \
        if (!(va_ == vb_))                                                       \
            ::testing::fail(__FILE__, __LINE__,                                  \
                            std::string("CHECK_EQ(" #a ", " #b "): ") +          \
                                ::testing::show(va_) + " != " + ::testing::show(vb_)); \
    } while (0)

#define REQUIRE(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::testing::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");         \
            throw ::testing::Abort{};                                          \
        }                                                                      \
    } while (0)
