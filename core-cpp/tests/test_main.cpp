#include <cstring>
#include <iostream>

#include "Harness.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    int failed = 0;
    for (const auto& test : testing::registry()) {
        if (filter && test.name.find(filter) == std::string::npos) continue;
        ++ran;
        testing::currentFailures() = 0;
        try {
            test.fn();
        } catch (const testing::Abort&) {
        } catch (const std::exception& e) {
            testing::fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        }
        if (testing::currentFailures() > 0) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << "\n";
        } else {
            std::cout << "[ ok ] " << test.name << "\n";
        }
    }
    std::cout << ran - failed << "/" << ran << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
