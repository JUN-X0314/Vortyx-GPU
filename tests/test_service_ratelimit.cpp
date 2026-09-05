// Rate limiter tests (Phase 14) — deterministic fixed windows.
//
// The FakeClock makes every boundary exact: no sleeps, no timing flake.

#include <iostream>
#include <string>

#include "distributed/clock.hpp"
#include "service/ratelimit.hpp"

using namespace vortyx::service;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

}  // namespace

int main() {
    // 1. The limit and the counting rule (refused attempts count).
    {
        auto clock = std::make_shared<vortyx::distributed::FakeClock>(1000);
        RateLimiter limiter(clock, 3, 100);  // 3 attempts per 100 ms
        check(limiter.try_acquire("user-a"), "attempt 1 allowed");
        check(limiter.try_acquire("user-a"), "attempt 2 allowed");
        check(limiter.try_acquire("user-a"), "attempt 3 allowed");
        check(!limiter.try_acquire("user-a"), "attempt 4 REFUSED");
        check(!limiter.try_acquire("user-a"), "attempt 5 still refused");
        check(limiter.attempts("user-a") == 5, "refused attempts count too");
        check(limiter.max_per_window() == 3 && limiter.window_ms() == 100,
              "configuration readable");
    }

    // 2. Per-key isolation.
    {
        auto clock = std::make_shared<vortyx::distributed::FakeClock>(0);
        RateLimiter limiter(clock, 1, 1000);
        check(limiter.try_acquire("user-a"), "user-a allowed");
        check(!limiter.try_acquire("user-a"), "user-a limited");
        check(limiter.try_acquire("user-b"), "user-b independent");
        check(limiter.attempts("user-unknown") == 0, "unknown key reports zero");
    }

    // 3. Exact window boundaries (no drift: multiples of window_ms).
    {
        auto clock = std::make_shared<vortyx::distributed::FakeClock>(1000);
        RateLimiter limiter(clock, 2, 100);
        check(limiter.try_acquire("k") && limiter.try_acquire("k") && !limiter.try_acquire("k"),
              "window filled");
        clock->advance(99);  // 1099 — same window (1000..1099)
        check(!limiter.try_acquire("k"), "99 ms later: same window");
        check(limiter.attempts("k") == 4, "count accumulated (refused attempts count)");
        clock->advance(1);   // 1100 — the next window
        check(limiter.attempts("k") == 0, "boundary crossed lazily");
        check(limiter.try_acquire("k") && limiter.try_acquire("k") && !limiter.try_acquire("k"),
              "the new window refills");
    }

    // 4. reset() clears one key without touching others.
    {
        auto clock = std::make_shared<vortyx::distributed::FakeClock>(0);
        RateLimiter limiter(clock, 1, 1000);
        check(limiter.try_acquire("a") && limiter.try_acquire("b"), "both keys used");
        limiter.reset("a");
        check(limiter.try_acquire("a"), "reset key allowed again");
        check(!limiter.try_acquire("b"), "other key still limited");
    }

    if (failures == 0) {
        std::cout << "Service rate limit tests passed.\n";
        return 0;
    }
    std::cerr << failures << " rate limit test(s) failed.\n";
    return 1;
}
