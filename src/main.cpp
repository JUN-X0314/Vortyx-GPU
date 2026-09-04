#include <iostream>
#include "core/version.hpp"
#include "core/logger.hpp"

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Vortyx GPU" << std::endl;
    std::cout << "  Version: " << VORTYX_VERSION_STRING << std::endl;
    std::cout << "========================================" << std::endl;

    vortyx::log(vortyx::LogLevel::Info, "Vortyx runtime initialized.");
    vortyx::log(vortyx::LogLevel::Info, "Phase 1: Development environment ready.");
    vortyx::log(vortyx::LogLevel::Info, "No GPU compute functions are implemented yet.");

    return 0;
}
