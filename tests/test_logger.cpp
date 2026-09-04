#include <iostream>
#include <sstream>
#include <string>
#include "core/logger.hpp"

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// Verifies that vortyx::log() writes the expected "[LEVEL] message" line
// for every log level. Output is captured by redirecting std::cout.
int main() {
    std::ostringstream captured;
    std::streambuf* original = std::cout.rdbuf(captured.rdbuf());

    vortyx::log(vortyx::LogLevel::Info, "hello");
    vortyx::log(vortyx::LogLevel::Warning, "careful");
    vortyx::log(vortyx::LogLevel::Error, "boom");

    std::cout.rdbuf(original);

    const std::string out = captured.str();
    int failures = 0;

    if (!contains(out, "[INFO] hello")) {
        std::cerr << "FAIL: expected '[INFO] hello' in logger output\n";
        ++failures;
    }
    if (!contains(out, "[WARN] careful")) {
        std::cerr << "FAIL: expected '[WARN] careful' in logger output\n";
        ++failures;
    }
    if (!contains(out, "[ERROR] boom")) {
        std::cerr << "FAIL: expected '[ERROR] boom' in logger output\n";
        ++failures;
    }

    if (failures == 0) {
        std::cout << "Logger test passed.\n";
        return 0;
    }
    return 1;
}
