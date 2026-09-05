#pragma once

// Deterministic rate limiting (Phase 14).
//
// A fixed-window counter per key, driven by the INJECTED IClock (the same
// determinism-by-injection rule the whole project follows — tests use
// FakeClock and are exact, never timing-dependent). In-memory and local:
// there is NO Redis here and none is claimed; a distributed rate limiter is
// a future provider behind this same interface shape.
//
// ALGORITHM (fixed window): time is divided into windows of 'window_ms'.
// A key's counter counts ATTEMPTS (acquired and refused alike) within the
// current window; a new window resets the counter. try_acquire returns true
// when the attempt (after incrementing) is <= max_per_window.
//
//   - Refused attempts count: hammering the boundary cannot sneak unlimited
//     requests through (the stricter, simpler contract).
//   - Window boundaries are exact multiples of window_ms — no drift, no
//     wall-clock dependence beyond the injected clock.
//   - Memory: one counter per distinct key seen in the CURRENT window;
//     keys from older windows are reset lazily on access (bounded by the
//     number of active callers — user ids, the only key kind the facade
//     uses).
//
// Thread-safe (the service calls it from several threads).

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "distributed/clock.hpp"  // IClock — determinism by injection

namespace vortyx::service {

class RateLimiter final {
public:
    // 'clock' must outlive the limiter. max_per_window >= 1; window_ms > 0
    // (enforced at construction with 1 as the honest floor for the max —
    // a zero limit would refuse everything and is a configuration bug the
    // facade validates before constructing).
    RateLimiter(std::shared_ptr<vortyx::distributed::IClock> clock,
                std::uint32_t max_per_window, std::int64_t window_ms);

    // Records ONE attempt for 'key' and returns whether it is allowed in
    // the current window.
    bool try_acquire(const std::string& key);

    // The attempt count recorded for 'key' in ITS current window
    // (observability / tests; 0 for unknown keys).
    std::uint32_t attempts(const std::string& key) const;

    // Clears one key's counter (tests / administrative reset).
    void reset(const std::string& key);

    std::uint32_t max_per_window() const { return max_per_window_; }
    std::int64_t window_ms() const { return window_ms_; }

private:
    struct Counter {
        std::int64_t window_index = 0;
        std::uint32_t attempts = 0;
    };

    std::shared_ptr<vortyx::distributed::IClock> clock_;
    std::uint32_t max_per_window_;
    std::int64_t window_ms_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Counter> counters_;
};

}  // namespace vortyx::service
