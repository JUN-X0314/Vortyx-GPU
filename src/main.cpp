#include <iostream>
#include "core/version.hpp"
#include "core/logger.hpp"

#ifndef VORTYX_BUILD_CONFIG
#define VORTYX_BUILD_CONFIG "Unknown"
#endif

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Vortyx GPU" << std::endl;
    std::cout << "  Version: " << VORTYX_VERSION_STRING << std::endl;
    std::cout << "  Phase:   1 (Development Foundation)" << std::endl;
    std::cout << "  Build:   " << VORTYX_BUILD_CONFIG << std::endl;
    std::cout << "========================================" << std::endl;

    vortyx::log(vortyx::LogLevel::Info, "Vortyx started.");
    vortyx::log(vortyx::LogLevel::Info, "Runtime: not implemented yet (planned for a future phase).");
    vortyx::log(vortyx::LogLevel::Info, "GPU compute: not implemented yet (no GPU API is used in Phase 1).");
    vortyx::log(vortyx::LogLevel::Info, "Phase 1 status: development environment ready.");

    return 0;
}
