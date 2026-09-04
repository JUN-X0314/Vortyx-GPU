#pragma once

// Basic Scheduler (Phase 7).
//
// The first execution-target SELECTION layer. The Scheduler answers exactly
// one question — "which backend should this work run on?" — with a
// deterministic, explainable decision based on REAL backend availability:
//
//   Application -> Scheduler (select the execution target)
//                           -> Virtual GPU -> Compute Runtime
//                                           -> Resource Manager -> Backend
//
// The selection result is an EXECUTION CONTEXT (backend name + the concrete
// device behind it + the reason). The Scheduler never computes anything: it
// does not execute tasks, does not record Vulkan commands, does not touch
// ResourceManager memory, and does not replace the TaskQueue worker. Actual
// execution keeps flowing through the unchanged Phase 5/6 path — an
// application feeds the selected backend into an explicit VirtualGpuDesc and
// wires that Virtual GPU into its TaskQueue exactly as before:
//
//   Scheduler s; s.initialize();
//   SelectionResult r = s.select(SelectionRequest{});      // automatic
//   VirtualGpuDesc desc; desc.backend = r.backend;         // the choice
//   VirtualGpu gpu; gpu.initialize(desc);
//   TaskQueue queue; queue.initialize(gpu);                // unchanged Phase 6
//
// Responsibilities (exactly these, nothing more):
//   - Probe the REAL availability of the registered compute backends through
//     the one honest source that owns them: a Compute Runtime. The probe
//     Runtime is private, used read-only (availability / device queries),
//     and never executes tasks.
//   - Apply the documented Basic policy (below) to explicit and automatic
//     requests and return the selection with a human-readable reason.
//   - Refuse unusable targets honestly: an unknown or unavailable backend is
//     a failing selection with the real reason — never silently remapped to
//     another backend, never reported as a success.
//
// Explicitly NOT its job (later phases, none of it implemented here):
//   - Multi-GPU aggregation or splitting work across devices.
//   - Load balancing, work stealing, priority scheduling, task graphs.
//   - Load/VRAM/temperature/power/latency-based decisions: the Scheduler
//     consumes only availability it can actually verify; metrics that do
//     not exist in the codebase are never invented.
//   - Network/distributed/remote devices, FPGA/ASIC, Vortyx hardware.
//   - Thread-pool or queue management (the TaskQueue keeps its single FIFO
//     worker unchanged).
//
// The Basic policy (documented, deterministic, no hidden rules):
//   - ExplicitBackend mode: the request names ONE backend (a canonical
//     Runtime name, e.g. "cpu" or "vulkan"). If it is registered AND
//     available on this system, it is selected. If it is registered but
//     unavailable, the selection FAILS with the backend's real reason. If
//     it is unknown, the selection fails listing the registered names.
//     An explicit request is NEVER silently remapped to another backend.
//   - Automatic mode: the request carries no backend. Candidates are
//     evaluated in the fixed priority order returned by
//     Scheduler::automatic_priority() — "vulkan" first, "cpu" second — and
//     the FIRST candidate that is actually available on this system wins.
//     Preferring a verified-usable GPU device when one exists is this
//     platform's functional purpose; it is NOT a performance measurement or
//     a speed assumption. On systems without a usable Vulkan device the
//     policy deterministically selects "cpu", which every system has.
//     Automatic mode may fall back between candidates because falling back
//     IS the policy; explicit mode never does.
//
// Honesty rules (project-wide, restated):
//   - A backend is a success target only when its availability is verified
//     through the Runtime at selection time. A compiled-in but unusable
//     backend (e.g. "vulkan" without a device/driver, or the stub build)
//     is never selected as a success.
//   - Every failing selection carries Status != Ok plus a human-readable
//     reason; every successful selection names the chosen backend and the
//     concrete device it would execute on (Phase 2 DeviceInfo, never
//     fabricated).
//   - The same request against the same system state always produces the
//     same answer (the policy is a pure function of the probed candidates).
//
// Ownership and lifetime:
//   - The Scheduler exclusively owns its probe Runtime (std::unique_ptr),
//     created by initialize() and destroyed by shutdown(). The probe is
//     completely independent of any application Virtual GPU: shutting a
//     Virtual GPU down never touches the Scheduler, and shutting the
//     Scheduler down never touches a Virtual GPU. No object's lifecycle is
//     managed twice — the double-shutdown hazard between queues and Virtual
//     GPUs (Phase 6 contract) does not arise here.
//   - The Scheduler holds no reference to any VirtualGpu or TaskQueue, so it
//     cannot dangle. The documented shutdown order of Phase 6 (queue before
//     its Virtual GPU) is unchanged; the Scheduler can be shut down at any
//     point before or after them because it shares nothing with them.
//   - Copying AND moving are deleted: the Scheduler owns a probe Runtime
//     whose identity is meaningless to transfer, and the object is cheap to
//     create where needed. One scheduler, one address, for life (same rule
//     as TaskQueue).
//
// Threading:
//   - select() may be called from several threads concurrently, and
//     concurrently with shutdown(); one internal mutex serializes probes
//     against shutdown (the probe Runtime is never queried while being
//     destroyed). Probes are cheap constant-time queries (cached backend
//     state — no Vulkan calls, no execution), so no lock is ever held during
//     computation.
//   - As with any blocking primitive, the caller must not destroy the
//     Scheduler while another thread is inside select().
//
// Error handling follows the project-wide result style (Phase 3): no
// exceptions thrown by the Scheduler itself, explicit Status values with
// human-readable 'error' strings, no new status vocabulary (the Phase 3
// Status enum covers every failure here).

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/device/device.hpp"

// Forward declaration: the Scheduler probes through a Compute Runtime but
// its API surface never exposes Runtime types (header dependencies stay
// one-directional; the complete type is only needed in the .cpp).
namespace vortyx::compute {
class Runtime;
}

namespace vortyx::scheduler {

// The Scheduler layer speaks the project-wide result vocabulary from Phase 3
// (one unified error model, no second status system).
using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;

// ---------------------------------------------------------------------------
// Lifecycle state
// ---------------------------------------------------------------------------

// The Scheduler's lifecycle state (mirrors the Virtual GPU's shape).
//  - Uninitialized: constructed (or re-initializable); initialize() has not
//    succeeded yet. select() fails with Status::NotInitialized.
//  - Ready: probe Runtime alive; select() available.
//  - ShutDown: shutdown() was called; select() fails with
//    Status::NotInitialized until initialize() is called again.
enum class State {
    Uninitialized,
    Ready,
    ShutDown,
};

const char* to_string(State state);

// ---------------------------------------------------------------------------
// Selection request
// ---------------------------------------------------------------------------

// What the caller asks the Scheduler for.
enum class SelectionMode {
    // No backend named: the documented automatic policy picks the first
    // AVAILABLE candidate in the fixed priority order ("vulkan", "cpu").
    Automatic,

    // The request names one backend in SelectionRequest::backend. That exact
    // backend is evaluated and either selected or the selection FAILS — it
    // is never silently remapped to a different backend.
    ExplicitBackend,
};

const char* to_string(SelectionMode mode);

struct SelectionRequest {
    // How the backend should be chosen (see SelectionMode).
    SelectionMode mode = SelectionMode::Automatic;

    // The explicit backend name for ExplicitBackend mode. Must use a
    // canonical Runtime backend name ("cpu", "vulkan"); the same strings the
    // Runtime and VirtualGpuDesc already use — no new naming system. Must be
    // empty in Automatic mode (a conflicting request is refused with
    // Status::InvalidInput instead of being guessed about).
    std::string backend;
};

// ---------------------------------------------------------------------------
// Selection result
// ---------------------------------------------------------------------------

// The chosen EXECUTION CONTEXT. The Scheduler's entire output: which backend
// to run on and what the concrete device behind that choice is. Actual
// execution happens through the unchanged Virtual GPU / Runtime / TaskQueue
// path, never here.
//
// 'backend' and 'device' are meaningful only when status == Status::Ok;
// 'error' explains any failure; 'reason' always explains the decision in
// human-readable form on success (the explainability contract of the Basic
// policy).
struct SelectionResult {
    Status status = Status::Ok;
    std::string error;  // empty when status == Ok

    // The selected canonical backend name (e.g. "cpu", "vulkan"). Feed it
    // into VirtualGpuDesc::backend to execute on the chosen target.
    std::string backend;

    // The concrete device the selected backend executes on (Phase 2
    // DeviceInfo, probed live from the Runtime — never fabricated).
    vortyx::device::DeviceInfo device;

    // Deterministic explanation of WHY this target was chosen (or why the
    // selection failed, mirroring 'error').
    std::string reason;
};

// ---------------------------------------------------------------------------
// The Basic policy — a pure function (unit-testable without any hardware)
// ---------------------------------------------------------------------------

// One execution-backend candidate as the policy sees it. The Scheduler fills
// these ONLY from real probe queries; tests may construct synthetic
// candidate sets to pin the policy's decisions deterministically without
// involving any hardware.
struct PolicyCandidate {
    std::string name;                // canonical backend name ("cpu", "vulkan")
    bool available = false;          // verified usable on THIS system right now
    std::string unavailable_reason;  // the backend's own reason when !available
};

// The Phase 7 Basic selection policy. PURE: no I/O, no probing, no global
// state — the same candidates and request always produce the same decision.
//
//   ExplicitBackend: selects the candidate whose name equals 'backend'
//     (exact canonical match). Registered-but-unavailable and unknown names
//     fail with Status::BackendUnavailable and the real reason; an empty
//     name fails with Status::InvalidInput. Never remaps.
//   Automatic: selects the FIRST available candidate in 'candidates' order
//     (the Scheduler passes them in automatic_priority() order). Fails with
//     Status::BackendUnavailable when nothing is available.
//
// Returns Status::Ok with 'selected_index' set (always < candidates.size()),
// or a failing Status with 'error' (and 'reason') filled. 'selected_index'
// is only meaningful on success.
Status basic_scheduler_select(const std::vector<PolicyCandidate>& candidates,
                              SelectionMode mode,
                              const std::string& requested_backend,
                              std::size_t& selected_index,
                              std::string& reason,
                              std::string& error);

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

class Scheduler final {
public:
    // Creates an Uninitialized Scheduler. initialize() before any use.
    // Defined out-of-line (like the destructor) because the Runtime type is
    // deliberately incomplete in this header (same pattern as VirtualGpu).
    Scheduler();

    // Releases the probe Runtime. Safe to call multiple times and on
    // Uninitialized objects (same no-op-lands-on-ShutDown rule as the
    // Virtual GPU).
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Creates and initializes the private probe Runtime (which initializes
    // every built-in backend and registers the buffer providers — the exact
    // same path a Virtual GPU uses — so availability here is the availability
    // an executing Virtual GPU would see).
    //
    // Calling initialize() while Ready is accepted with Status::Ok
    // (idempotent: the Scheduler has no configuration to change, mirroring
    // the unchanged-description re-initialization rule of the Virtual GPU).
    // Re-initializing after shutdown() builds a fresh probe Runtime.
    // Returns Status::Ok, or Status::NotInitialized if the probe Runtime
    // cannot be created (the Scheduler then stays unusable, no half state).
    Status initialize();

    // Destroys the probe Runtime. Safe to call multiple times, on
    // Uninitialized objects, and concurrently with select() (a concurrent
    // caller waits; an in-flight select finishes first). Never touches any
    // Virtual GPU, TaskQueue or other subsystem.
    void shutdown();

    // Current lifecycle state (never fabricated).
    State state() const noexcept;

    // True exactly when state() == State::Ready.
    bool is_ready() const noexcept { return state() == State::Ready; }

    // Selects the execution target for a request. Never blocks on compute,
    // never executes anything, and (in ExplicitBackend mode) never falls
    // back. See the policy documentation above for the exact rules and the
    // class documentation for the ownership/thread-safety contract.
    SelectionResult select(const SelectionRequest& request) const;

    // The fixed automatic-mode priority order: {"vulkan", "cpu"}. Exposed as
    // part of the API so applications and tests can verify the documented
    // policy instead of guessing it. Automatic mode considers exactly these
    // backends; explicit requests can address any registered backend.
    static const std::vector<std::string>& automatic_priority();

private:
    // The private probe source. Owns the real backends and answers
    // availability/device questions; never executes tasks. Non-null exactly
    // while the Scheduler is Ready.
    std::unique_ptr<vortyx::compute::Runtime> probe_;

    // Serializes select() probes and state checks against shutdown().
    // Mutable: select() is a const query (same pattern as TaskQueue's
    // const methods locking its mutex).
    mutable std::mutex mutex_;

    State state_ = State::Uninitialized;
};

}  // namespace vortyx::scheduler
