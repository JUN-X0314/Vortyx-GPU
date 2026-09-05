// Deterministic rate limiting (Phase 14) — implementation.

#include "service/ratelimit.hpp"

namespace vortyx::service {

RateLimiter::RateLimiter(std::shared_ptr<vortyx::distributed::IClock> clock,
                         std::uint32_t max_per_window, std::int64_t window_ms)
    : clock_(std::move(clock)), max_per_window_(max_per_window < 1 ? 1 : max_per_window),
      window_ms_(window_ms > 0 ? window_ms : 1) {}

bool RateLimiter::try_acquire(const std::string& key) {
    const std::int64_t now = clock_ ? clock_->now_ms() : 0;
    // Floor division for negative clocks too (defensive; a test clock never
    // goes backwards, but the math must not depend on that).
    const std::int64_t window_index = now >= 0 ? now / window_ms_ : (now - window_ms_ + 1) / window_ms_;

    std::lock_guard<std::mutex> lock(mutex_);
    Counter& counter = counters_[key];
    if (counter.window_index != window_index) {
        counter.window_index = window_index;
        counter.attempts = 0;
    }
    ++counter.attempts;
    return counter.attempts <= max_per_window_;
}

std::uint32_t RateLimiter::attempts(const std::string& key) const {
    const std::int64_t now = clock_ ? clock_->now_ms() : 0;
    const std::int64_t window_index = now >= 0 ? now / window_ms_ : (now - window_ms_ + 1) / window_ms_;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = counters_.find(key);
    if (it == counters_.end() || it->second.window_index != window_index) return 0;
    return it->second.attempts;
}

void RateLimiter::reset(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_.erase(key);
}

}  // namespace vortyx::service
