// Basic Scheduler implementation (Phase 7).
//
// Design notes (kept next to the code they explain):
//
// Probe ownership — the Scheduler owns a private Compute Runtime used
// strictly read-only: backend_names() / has_backend() /
// backend_unavailable_reason() / backend_device(). The Runtime is the one
// layer that owns the real backends, so its answers ARE the real backend
// state — the same state an executing Virtual GPU would see (a Virtual GPU
// builds its Runtime through the identical initialize() path). The probe
// never executes tasks; creating it twice (once here, once in the
// application's Virtual GPU) is deliberate Phase 7 minimalism: sharing one
// Runtime between the probe and the executing Virtual GPU would require
// changing the Phase 5 VirtualGpu ownership contract, which this phase is
// not allowed to break.
//
// Pure policy — every decision goes through basic_scheduler_select(), a
// pure function over probed candidates. The Scheduler adds no hidden rules
// on top of it, which is what makes the policy testable without hardware
// and deterministic on real hardware.
//
// Lock discipline — one mutex serializes select() and shutdown(). It is
// held only across cheap cached-state queries (no Vulkan calls, no
// execution), so it never turns a long operation into a bottleneck and
// cannot deadlock: nothing below this layer ever calls back into the
// Scheduler.

#include "core/scheduler/scheduler.hpp"

#include <algorithm>
#include <utility>

#include "core/compute/runtime.hpp"
#include "core/logger.hpp"

namespace vortyx::scheduler {

const char* to_string(State state) {
    switch (state) {
        case State::Uninitialized: return "Uninitialized";
        case State::Ready: return "Ready";
        case State::ShutDown: return "ShutDown";
    }
    return "Unknown";
}

const char* to_string(SelectionMode mode) {
    switch (mode) {
        case SelectionMode::Automatic: return "Automatic";
        case SelectionMode::ExplicitBackend: return "ExplicitBackend";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// The Basic policy (pure function)
// ---------------------------------------------------------------------------

Status basic_scheduler_select(const std::vector<PolicyCandidate>& candidates,
                              SelectionMode mode,
                              const std::string& requested_backend,
                              std::size_t& selected_index,
                              std::string& reason,
                              std::string& error) {
    selected_index = 0;
    reason.clear();
    error.clear();

    if (mode == SelectionMode::ExplicitBackend) {
        // An explicit request that names nothing is a caller bug, not an
        // environment condition — refuse it as invalid input.
        if (requested_backend.empty()) {
            error = "explicit backend request names no backend (empty name)";
            reason = error;
            return Status::InvalidInput;
        }

        // Unknown name: list the registered candidates so the caller can
        // correct the request. (Same shape as Runtime's own
        // backend_unavailable_reason() for unknown names.)
        const bool registered = std::any_of(candidates.begin(), candidates.end(),
            [&requested_backend](const PolicyCandidate& c) {
                return c.name == requested_backend;
            });
        if (!registered) {
            std::string known;
            for (const PolicyCandidate& c : candidates) {
                if (!known.empty()) known += ", ";
                known += c.name;
            }
            error = "unknown backend '" + requested_backend +
                    "' (registered backends: " + known + ")";
            reason = error;
            return Status::BackendUnavailable;
        }

        // Registered: the requested backend is evaluated EXACTLY as
        // requested. An unavailable backend fails with its own real reason —
        // it is never swapped for a different, working backend (an explicit
        // request is a requirement, not a hint).
        const auto it = std::find_if(candidates.begin(), candidates.end(),
            [&requested_backend](const PolicyCandidate& c) {
                return c.name == requested_backend;
            });
        if (!it->available) {
            error = "requested backend '" + requested_backend +
                    "' is unavailable on this system: " + it->unavailable_reason;
            reason = error;
            return Status::BackendUnavailable;
        }

        selected_index = static_cast<std::size_t>(std::distance(candidates.begin(), it));
        reason = "explicit request honored: backend '" + requested_backend +
                 "' is registered and available on this system";
        return Status::Ok;
    }

    // Automatic mode: walk the candidates in the priority order the caller
    // provided (the Scheduler passes automatic_priority() order) and select
    // the first one that is REALLY available. No load, no timing, no
    // hardware guessing — verified availability only.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].available) continue;
        selected_index = i;
        std::string skipped;
        for (std::size_t j = 0; j < i; ++j) {
            skipped += "'" + candidates[j].name + "' is not usable on this system (" +
                       (candidates[j].unavailable_reason.empty()
                            ? std::string("unavailable")
                            : candidates[j].unavailable_reason) +
                       ")" + (j + 1 < i ? "; " : "");
        }
        reason = "automatic policy: '" + candidates[i].name +
                 "' is the highest-priority available backend (priority order: ";
        for (std::size_t j = 0; j < candidates.size(); ++j) {
            reason += "'" + candidates[j].name + "'" +
                      (j + 1 < candidates.size() ? " > " : ")");
        }
        if (!skipped.empty()) {
            reason += "; higher-priority candidates skipped: " + skipped;
        }
        return Status::Ok;
    }

    // Nothing available. This cannot happen with the built-in backends (the
    // CPU backend is always available), but the policy must still answer
    // honestly instead of pretending if that invariant ever changes.
    std::string detail;
    for (const PolicyCandidate& c : candidates) {
        detail += "'" + c.name + "': " +
                  (c.unavailable_reason.empty() ? std::string("unavailable")
                                                : c.unavailable_reason) +
                  " ";
    }
    error = "no available backend (candidates: " + detail + ")";
    reason = error;
    return Status::BackendUnavailable;
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

Scheduler::Scheduler() = default;

Scheduler::~Scheduler() {
    shutdown();
}

const std::vector<std::string>& Scheduler::automatic_priority() {
    // Documented fixed order: prefer a verified-usable GPU device when one
    // exists (this platform's functional purpose — a selection rule, not a
    // performance claim); the CPU is the guaranteed fallback every system
    // has. New backends extend this list explicitly; nothing is inferred.
    static const std::vector<std::string> priority = {"vulkan", "cpu"};
    return priority;
}

Status Scheduler::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ == State::Ready) {
        // Idempotent: the Scheduler has no configuration, so a re-init
        // cannot be a reconfiguration (mirrors VirtualGpu's
        // unchanged-description rule, minus the refusal branch).
        return Status::Ok;
    }

    // Fresh probe on every (re-)initialization; after a shutdown() the old
    // one is gone, so there is never a half-initialized probe around.
    std::unique_ptr<vortyx::compute::Runtime> probe =
        std::make_unique<vortyx::compute::Runtime>();
    if (probe->initialize() != Status::Ok) {
        // Cannot happen with the built-in backends (Runtime::initialize is
        // designed to always succeed), but stay defensive and honest.
        vortyx::log(vortyx::LogLevel::Error,
                    "Scheduler: probe Compute Runtime failed to initialize.");
        state_ = State::Uninitialized;
        return Status::NotInitialized;
    }

    probe_ = std::move(probe);
    state_ = State::Ready;

    std::string available;
    for (const std::string& name : automatic_priority()) {
        if (probe_->has_backend(name)) {
            if (!available.empty()) available += ", ";
            available += name;
        }
    }
    vortyx::log(vortyx::LogLevel::Info,
                "Scheduler initialized (probe Runtime ready, available backends: " +
                    (available.empty() ? std::string("none") : available) + ").");
    return Status::Ok;
}

void Scheduler::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    // In-flight select() calls have finished by the time the lock is held,
    // so destroying the probe below can never race a query. An Uninitialized
    // scheduler lands on ShutDown too (safe no-op, mirroring VirtualGpu).
    if (probe_ != nullptr) {
        probe_->shutdown();
        probe_.reset();
        vortyx::log(vortyx::LogLevel::Info, "Scheduler shut down.");
    }
    state_ = State::ShutDown;
}

State Scheduler::state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

SelectionResult Scheduler::select(const SelectionRequest& request) const {
    SelectionResult result;

    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != State::Ready || probe_ == nullptr) {
        result.status = Status::NotInitialized;
        result.error = "Scheduler is not initialized (state: " +
                       std::string(to_string(state_)) +
                       "; call initialize() before select())";
        result.reason = result.error;
        return result;
    }

    // Strict request shape: Automatic mode must not carry a backend name —
    // a conflicting request is refused instead of being guessed about.
    if (request.mode == SelectionMode::Automatic && !request.backend.empty()) {
        result.status = Status::InvalidInput;
        result.error = "SelectionRequest sets mode=Automatic but also names a backend ('" +
                       request.backend + "'); leave 'backend' empty for automatic "
                       "selection or use mode=ExplicitBackend";
        result.reason = result.error;
        return result;
    }

    // Build candidates ONLY from real probe queries, in the documented
    // automatic priority order. Every candidate reflects the backend's own
    // current state — compiled-in-but-unusable backends surface here as
    // available=false with their real reason.
    const std::vector<std::string>& priority = automatic_priority();

    // An explicit request may name any registered backend, so the candidate
    // list is the priority order PLUS any registered backend the priority
    // list does not cover (with the current built-ins this list is exactly
    // {"vulkan", "cpu"} — the extra scan is future-proofing, not new
    // behavior).
    std::vector<PolicyCandidate> candidates;
    candidates.reserve(priority.size() + 2);
    const auto push_candidate = [this, &candidates](const std::string& name) {
        const bool already = std::any_of(candidates.begin(), candidates.end(),
            [&name](const PolicyCandidate& c) { return c.name == name; });
        if (already) return;
        PolicyCandidate candidate;
        candidate.name = name;
        candidate.available = probe_->has_backend(name);
        if (!candidate.available) {
            candidate.unavailable_reason = probe_->backend_unavailable_reason(name);
        }
        candidates.push_back(std::move(candidate));
    };
    for (const std::string& name : priority) {
        push_candidate(name);
    }
    for (const std::string& name : probe_->backend_names()) {
        push_candidate(name);
    }

    // The entire decision happens in the pure policy function.
    std::size_t index = 0;
    result.status = basic_scheduler_select(candidates, request.mode, request.backend,
                                           index, result.reason, result.error);
    if (result.status != Status::Ok) {
        vortyx::log(vortyx::LogLevel::Warning,
                    "Scheduler: selection refused (" + std::string(to_string(result.status)) +
                        "): " + result.error);
        return result;
    }

    // Fill the execution context from the probed truth: the canonical name
    // and the concrete device the backend actually executes on.
    result.backend = candidates[index].name;
    result.device = probe_->backend_device(result.backend);

    vortyx::log(vortyx::LogLevel::Info,
                "Scheduler selected backend '" + result.backend + "' (" + result.reason + ").");
    return result;
}

}  // namespace vortyx::scheduler
