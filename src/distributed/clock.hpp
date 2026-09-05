#pragma once

// Injectable clock for the distributed layer (Phase 12).
//
// WHY: every timeout in a distributed system (heartbeat expiry, lease
// expiry, retry backoff) is time-dependent, and time-dependent tests that
// read the real clock directly are flaky by construction. The project
// forbids sleep-based flaky tests, so every component that needs time takes
// an IClock instead of calling std::chrono itself.
//
// Time vocabulary: MONOTONIC milliseconds ("steady" time — immune to wall
// clock adjustments). The value has no defined epoch; components may only
// compare values obtained from the SAME clock instance. Wall-clock
// timestamps that travel the control plane stay in the platform layer
// (platform::now_epoch_ms) — this clock is for scheduling/liveness logic
// only.

#include <cstdint>

namespace vortyx::distributed {

// The time source abstraction. One method; implementations are cheap and
// must be thread-safe (components call now_ms() while holding internal
// locks).
class IClock {
public:
    virtual ~IClock() = default;

    // Current monotonic time in milliseconds.
    virtual std::int64_t now_ms() const = 0;
};

// The production clock: std::chrono::steady_clock. Thread-safe (the
// underlying call is).
class SteadyClock final : public IClock {
public:
    std::int64_t now_ms() const override;
};

// The test clock: a manual, fully deterministic time source. Tests advance
// it explicitly, so heartbeat/lease/backoff behavior is verified without a
// single sleep. Not internally synchronized — a FakeClock is test state and
// tests drive it from one thread at a time (the same rule as any other test
// fixture).
class FakeClock final : public IClock {
public:
    FakeClock() = default;
    explicit FakeClock(std::int64_t start_ms) : now_ms_(start_ms) {}

    std::int64_t now_ms() const override { return now_ms_; }

    // Sets the absolute time (forward or backward — tests may jump).
    void set(std::int64_t ms) { now_ms_ = ms; }
    // Advances the time by 'delta_ms' (may be negative; tests may jump).
    void advance(std::int64_t delta_ms) { now_ms_ += delta_ms; }

private:
    std::int64_t now_ms_ = 0;
};

}  // namespace vortyx::distributed
