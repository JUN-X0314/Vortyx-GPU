// Injectable clock implementation (Phase 12).

#include "distributed/clock.hpp"

#include <chrono>

namespace vortyx::distributed {

std::int64_t SteadyClock::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace vortyx::distributed
