#pragma once
#include <string>

namespace vortyx {
    enum class LogLevel { Info, Warning, Error };
    void log(LogLevel level, const std::string& message);
}
