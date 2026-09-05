#pragma once

// Resource Monitoring (Phase 8).
//
// The observation layer for Vortyx execution resources. Phase 8 is about
// OBSERVABILITY, not control: the monitor answers "what can honestly be
// known about the system, the backends, the devices and the allocated
// resources RIGHT NOW?" — nothing more.
//
//   Monitor -> Compute Runtime / Device discovery / OS standard APIs
//   (read-only queries of sources that already exist; the monitor opens no
//    new discovery path and scrapes no hardware state of its own)
//
// The one hard rule — REAL DATA ONLY:
//   - Every value in a snapshot comes from a real source: the standard
//     library, the Phase 2 device discovery (through the backend's own
//     DeviceInfo), or the Phase 4 ResourceManager statistics (through the
//     Runtime's own API). Nothing is estimated, extrapolated or invented.
//   - Values the platform/API cannot provide are represented as explicitly
//     unavailable (std::optional = nullopt, or a valid-flag = false), NEVER
//     as a fake 0 — 0 can be a real measurement, "unknown" cannot.
//   - Metrics that do not exist in the current stack have NO FIELD AT ALL:
//     GPU utilization, GPU temperature, GPU power draw, instantaneous CPU
//     utilization, current VRAM usage, fan speed, PCIe bandwidth. Their
//     absence IS the honest representation (no field can be misread as a
//     measurement). Exposing any of them would require an API the codebase
//     does not have; a future phase may add them only when a real source
//     exists.
//
// Single source of truth (no duplicated discovery):
//   - System CPU/RAM facts come from the Runtime's backend DeviceInfo
//     (Phase 2 discovery behind it), Vulkan device facts from the Vulkan
//     backend's own DeviceInfo, allocation facts from the Runtime's
//     ResourceManager stats. The monitor re-queries those existing APIs and
//     NEVER re-implements CPUID / DXGI / Vulkan enumeration or OS memory
//     scraping in a second place.
//
// Relationship to the Phase 7 Scheduler (informational independence):
//   - The monitor is a pure observer. It does NOT feed the Scheduler, does
//     not influence selection, and the Scheduler's policy remains exactly
//     what Phase 7 shipped: deterministic availability-based selection
//     (vulkan > cpu). Turning observations into scheduling decisions is
//     future work this phase deliberately does not implement.
//
// Snapshot semantics:
//   - snapshot() reads the current state ONCE and returns it BY VALUE. The
//     returned ResourceSnapshot owns all of its data (strings, vectors,
//     DeviceInfo copies) and stays valid forever afterwards — no reference
//     into any internal object, no dangling DeviceInfo, no string_view into
//     a Runtime, no mutex the caller has to manage. Taking a snapshot never
//     mutates the system and never releases or retains any resource.
//
// Ownership and lifetime:
//   - The monitor OWNS NOTHING: it is a stateless observer with no
//     lifecycle (constructed ready, no initialize/shutdown — inventing a
//     lifecycle for a stateless collector would be fake state). The
//     no-lifecycle rule also means it never keeps a Runtime alive and never
//     needs to be shut down before or after anything.
//   - The Runtime passed to snapshot(runtime&) is borrowed for the duration
//     of the call only: the caller owns it and must keep it alive for the
//     call (the same "caller owns the execution context" rule the
//     TaskQueue/benchmark document). The monitor never shuts it down,
//     never executes on it, never creates or destroys its buffers.
//
// Threading:
//   - The monitor itself is stateless and const: concurrent snapshot()
//     calls on one monitor share nothing mutable.
//   - A snapshot reads the given Runtime read-only. Concurrent READ-ONLY
//     snapshots against an unchanged Runtime are safe (pure reads, no
//     writer). If any thread is MUTATING the Runtime (initializing,
//     shutting down, creating/destroying buffers) that caller must apply
//     the same external serialization the Runtime/ResourceManager have
//     always required — the monitor adds no locking of its own (a private
//     lock could not make a mutating caller safe anyway and would only
//     pretend otherwise).
//
// Error handling follows the project-wide result style: snapshot() cannot
// fail into a half-valid state — it returns a complete snapshot whose
// validity flags describe exactly what could be observed. There is no new
// Status vocabulary here because nothing fails loudly: an unobservable
// quantity is represented by its explicit unavailable marker, which is a
// valid, honest answer.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/device/device.hpp"
#include "core/resource/resource_manager.hpp"

// Forward declaration: snapshots query a live Compute Runtime. The complete
// type is required only in the .cpp, keeping header dependencies
// one-directional (monitor -> compute stays out of this header).
namespace vortyx::compute {
class Runtime;
}

namespace vortyx::monitor {

// ---------------------------------------------------------------------------
// Snapshot data model (all value types; no references, no views)
// ---------------------------------------------------------------------------

// One registered compute backend as observed through the Runtime at snapshot
// time: its real availability, the backend's own unavailable reason when it
// cannot run, and its own DeviceInfo report. Mirrors what the Runtime's
// has_backend() / backend_unavailable_reason() / backend_device() answer —
// the monitor adds no opinion of its own.
struct BackendObservation {
    // Canonical Runtime backend name ("cpu", "vulkan").
    std::string name;

    // True when the backend exists AND is usable on this system right now.
    bool available = false;

    // The backend's own real reason when !available; empty otherwise.
    std::string unavailable_reason;

    // The concrete device the backend executes on (Phase 2 DeviceInfo,
    // probed live — never fabricated). A default (Unknown) DeviceInfo means
    // the backend has no device to report (e.g. unavailable/stub Vulkan);
    // it is NOT a fake device entry.
    vortyx::device::DeviceInfo device;
};

// One moment's observation of the system and Vortyx's execution resources.
// A value type end to end: copyable, movable, safe to keep forever, safe to
// compare (repeated snapshots of an unchanged system compare equal).
//
// Validity pattern: fields that describe a source the snapshot did NOT
// observe are marked by flags/nullopt — never by invented values.
struct ResourceSnapshot {
    // --- System (always observed; standard library only) -----------------

    // std::thread::hardware_concurrency(): the number of hardware threads
    // the standard library reports. nullopt when the library cannot
    // determine it (a documented 0 return value) — never a fake count.
    std::optional<std::uint32_t> hardware_threads;

    // --- Vortyx Runtime (observed only via snapshot(runtime&)) -----------

    // True when the snapshot observed a live Runtime ('backends' is filled
    // from it). False for a system-only snapshot; 'backends' is then empty
    // and the resource stats below are invalid — there was no source to
    // observe, and no value is invented for the missing source.
    bool runtime_observed = false;

    // Every backend registered with the Runtime, in the Runtime's own
    // registration order (filled only when runtime_observed).
    std::vector<BackendObservation> backends;

    // --- ResourceManager accounting (Phase 4 stats, via the Runtime) ----

    // True when the Runtime's ResourceManager statistics were reachable and
    // are filled below. (The manager exists for the Runtime's whole
    // lifetime, so this mirrors runtime_observed in practice; the flag
    // keeps the honest "was this observed?" question separate from "which
    // source".) After a Runtime shutdown the stats simply reflect whatever
    // the manager last reported — the monitor never guesses.
    bool resource_stats_valid = false;

    // Live (not yet released) buffers and their total byte footprint.
    std::size_t live_buffers = 0;
    std::size_t live_bytes = 0;

    // Every successful allocation since the manager was created (never
    // reset by releases) — useful for leak observation.
    std::uint64_t total_allocations = 0;

    // Number of backends that are really available (convenience derived
    // value; computed from 'backends', never stored twice as state).
    std::size_t available_backend_count() const;

    // The observation named 'name', or nullptr when the snapshot has no
    // such backend (system-only snapshot, or unknown name — an unknown
    // name is an absence, not an error).
    const BackendObservation* find_backend(const std::string& name) const;
};

// ---------------------------------------------------------------------------
// ResourceMonitor
// ---------------------------------------------------------------------------

class ResourceMonitor final {
public:
    // Stateless: no configuration, no lifecycle, no initialize/shutdown.
    ResourceMonitor() = default;

    ResourceMonitor(const ResourceMonitor&) = delete;
    ResourceMonitor& operator=(const ResourceMonitor&) = delete;
    // Moving would suggest transferable internal state; there is none.
    ResourceMonitor(ResourceMonitor&&) = delete;
    ResourceMonitor& operator=(ResourceMonitor&&) = delete;

    // System-only snapshot: what the standard library can honestly report
    // with no Runtime involved. hardware_threads is filled; everything
    // Vortyx-specific stays explicitly unobserved (runtime_observed ==
    // false, empty backends, resource_stats_valid == false).
    ResourceSnapshot snapshot() const;

    // Full snapshot: system facts PLUS the live Runtime's real backend
    // state (availability, unavailable reasons, device info per backend)
    // and the ResourceManager accounting. The Runtime is borrowed for the
    // duration of the call and is never modified.
    ResourceSnapshot snapshot(vortyx::compute::Runtime& runtime) const;
};

// ---------------------------------------------------------------------------
// Output forms
// ---------------------------------------------------------------------------

// Human-readable multi-line description of a snapshot, e.g.:
//   "Resource snapshot: hardware threads: 8"
//   "  backend 'cpu': available, device: CPU: Intel ... | RAM 3.9 GiB"
//   "  backend 'vulkan': unavailable (no Vulkan device ...)"
//   "  resources: 2 live buffer(s), 3072 live byte(s), 6 total allocation(s)"
// Values that were not observed are shown as "not observed" (never as a
// zero pretending to be a measurement).
std::string describe(const ResourceSnapshot& snapshot);

// Machine-readable stable key=value pairs (same contract as the benchmark's
// exporter: stable keys, units in the key name, fixed-point doubles, values
// only for what was really observed). Keys: hardware_threads (when known),
// runtime_observed, backend_<name>_available, backend_<name>_reason (only
// when unavailable), backend_<name>_device_type, backend_<name>_device_name,
// resource_stats_valid, live_buffers, live_bytes, total_allocations (the
// resource keys only when resource_stats_valid).
std::vector<std::pair<std::string, std::string>> to_key_values(const ResourceSnapshot& snapshot);

}  // namespace vortyx::monitor
