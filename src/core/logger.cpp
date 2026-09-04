#include "core/logger.hpp"
#include <iostream>

namespace vortyx {
    void log(LogLevel level, const std::string& message) {
        const char* prefix = "[INFO] ";
        if (level == LogLevel::Warning) prefix = "[WARN] ";
        if (level == LogLevel::Error)  prefix = "[ERROR] ";
        std::cout << prefix << message << std::endl;
    }
}
