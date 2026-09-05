// Resource Monitoring implementation (Phase 8).
//
// Everything here is a thin, honest read over sources that already exist:
//   - std::thread::hardware_concurrency() for the system thread count (the
//     only standard-library system fact with no platform-specific code and
//     no duplicated discovery behind it).
//   - Runtime::backend_names() / has_backend() / backend_unavailable_reason()
//     / backend_device() for backend and device facts (the SAME answers an
//     executing Virtual GPU or the Phase 7 Scheduler probe would see — one
//     source of truth, re-queried, never re-implemented).
//   - Runtime::resources().stats() for allocation accounting (Phase 4).
//
// The implementation deliberately contains no platform #ifdefs, no OS API
// calls, no Vulkan calls, and no CPUID/DXGI/procfs access: adding a second
// discovery path here is exactly what the phase rules forbid. When a future
// phase needs more metrics it must extend the owning layer (Runtime/backend
// /discovery) and let the monitor READ that, not scrape it here.
//
// Failure shape: every query below either succeeds or leaves the snapshot's
// explicit unavailable marker in place. There is no error path that could
// end in a fabricated number.

#include "core/monitor/monitor.hpp"

#include <thread>
#include <utility>

#include "core/compute/runtime.hpp"

namespace vortyx::monitor {

// ---------------------------------------------------------------------------
// ResourceSnapshot helpers
// ---------------------------------------------------------------------------

std::size_t ResourceSnapshot::available_backend_count() const {
    std::size_t count = 0;
    for (const BackendObservation& backend : backends) {
        if (backend.available) ++count;
    }
    return count;
}

const BackendObservation* ResourceSnapshot::find_backend(const std::string& name) const {
    for (const BackendObservation& backend : backends) {
        if (backend.name == name) return &backend;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// ResourceMonitor
// ---------------------------------------------------------------------------

ResourceSnapshot ResourceMonitor::snapshot() const {
    // System-only view: one standard-library query, everything Vortyx-
    // specific stays explicitly unobserved (default-initialized flags) —
    // there is no Runtime to ask and none is invented.
    ResourceSnapshot snap;

    const unsigned int threads = std::thread::hardware_concurrency();
    if (threads != 0) {
        snap.hardware_threads = static_cast<std::uint32_t>(threads);
    }
    // threads == 0 (library could not determine it) stays nullopt — the
    // documented honest "unknown", never a fabricated count.
    return snap;
}

ResourceSnapshot ResourceMonitor::snapshot(vortyx::compute::Runtime& runtime) const {
    ResourceSnapshot snap = snapshot();  // system facts first

    if (!runtime.is_initialized()) {
        // An uninitialized Runtime is a legitimate state, not an error:
        // report exactly what was observable (system facts only) and leave
        // the Vortyx sections honestly unobserved instead of probing a
        // half-built source.
        return snap;
    }

    snap.runtime_observed = true;

    // Backend observations in the Runtime's own registration order.
    for (const std::string& name : runtime.backend_names()) {
        BackendObservation observation;
        observation.name = name;
        observation.available = runtime.has_backend(name);
        if (!observation.available) {
            observation.unavailable_reason = runtime.backend_unavailable_reason(name);
        }
        observation.device = runtime.backend_device(name);
        snap.backends.push_back(std::move(observation));
    }

    // ResourceManager accounting (Phase 4 stats), read-only.
    const vortyx::resource::ResourceStats stats = runtime.resources().stats();
    snap.resource_stats_valid = true;
    snap.live_buffers = stats.live_buffers;
    snap.live_bytes = stats.live_bytes;
    snap.total_allocations = stats.total_allocations;

    return snap;
}

// ---------------------------------------------------------------------------
// Output forms
// ---------------------------------------------------------------------------

std::string describe(const ResourceSnapshot& snapshot) {
    std::string text = "Resource snapshot: hardware threads: ";
    text += snapshot.hardware_threads.has_value()
                ? std::to_string(*snapshot.hardware_threads)
                : std::string("unknown");

    if (!snapshot.runtime_observed) {
        text += "; Vortyx runtime: not observed (no Runtime passed to the snapshot)";
        return text;
    }

    text += "; backends observed: " + std::to_string(snapshot.backends.size()) +
            " (" + std::to_string(snapshot.available_backend_count()) + " available)";
    for (const BackendObservation& backend : snapshot.backends) {
        text += "\n  backend '" + backend.name + "': ";
        if (backend.available) {
            text += "available, device: " +
                    (backend.device.name.empty() ? std::string("unknown")
                                                 : backend.device.name);
        } else {
            text += "unavailable (" +
                    (backend.unavailable_reason.empty() ? std::string("no reason reported")
                                                        : backend.unavailable_reason) +
                    ")";
        }
    }
    if (snapshot.resource_stats_valid) {
        text += "\n  resources: " + std::to_string(snapshot.live_buffers) +
                " live buffer(s), " + std::to_string(snapshot.live_bytes) +
                " live byte(s), " + std::to_string(snapshot.total_allocations) +
                " total allocation(s)";
    }
    return text;
}

std::vector<std::pair<std::string, std::string>> to_key_values(const ResourceSnapshot& snapshot) {
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(8 + snapshot.backends.size() * 3);

    if (snapshot.hardware_threads.has_value()) {
        // The key exists only when the value is real: an unknown thread
        // count must not look like a zero-thread machine.
        pairs.emplace_back("hardware_threads", std::to_string(*snapshot.hardware_threads));
    }
    pairs.emplace_back("runtime_observed", snapshot.runtime_observed ? "true" : "false");

    for (const BackendObservation& backend : snapshot.backends) {
        const std::string prefix = "backend_" + backend.name;
        pairs.emplace_back(prefix + "_available", backend.available ? "true" : "false");
        if (!backend.available) {
            pairs.emplace_back(prefix + "_reason", backend.unavailable_reason);
        }
        pairs.emplace_back(prefix + "_device_type", [&] {
            switch (backend.device.type) {
                case vortyx::device::DeviceType::Unknown: return "Unknown";
                case vortyx::device::DeviceType::Cpu: return "Cpu";
                case vortyx::device::DeviceType::Gpu: return "Gpu";
                case vortyx::device::DeviceType::SoftwareGpu: return "SoftwareGpu";
            }
            return "Unknown";
        }());
        pairs.emplace_back(prefix + "_device_name", backend.device.name);
    }

    if (snapshot.resource_stats_valid) {
        pairs.emplace_back("resource_stats_valid", "true");
        pairs.emplace_back("live_buffers", std::to_string(snapshot.live_buffers));
        pairs.emplace_back("live_bytes", std::to_string(snapshot.live_bytes));
        pairs.emplace_back("total_allocations", std::to_string(snapshot.total_allocations));
    } else {
        pairs.emplace_back("resource_stats_valid", "false");
    }
    return pairs;
}

}  // namespace vortyx::monitor
