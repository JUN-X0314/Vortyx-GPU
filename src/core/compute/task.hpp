#pragma once

// Compute Task abstraction (Phase 3).
//
// A compute task is a single calculation Vortyx should execute. Phase 3
// ships exactly one task kind: Vector Addition (C[i] = A[i] + B[i] over
// int32 arrays). It is the simplest task that can run on both CPU and GPU
// with bit-exact comparable results (integer math, no floating point).
//
// Error handling style: the project (Phase 1/2) avoids exceptions and uses
// explicit result objects with a status + human-readable error message.
// Every failing operation returns a result whose 'error' explains why.

#include <cstdint>
#include <string>
#include <vector>

#include "core/resource/resource.hpp"

namespace vortyx::compute {

// Result status of a compute operation. Values are ordered so that
// status != Status::Ok always carries a meaningful 'error' string.
// Since Phase 4 this is also the status vocabulary of the resource layer
// (buffer creation, upload/download), so the whole project keeps one
// unified error model instead of mixing two systems.
enum class Status {
    Ok,                 // operation succeeded
    InvalidInput,       // task data is invalid (size mismatch or empty input)
    NotInitialized,     // runtime/backend/resource system was not initialized before use
    BackendUnavailable, // requested backend is unknown or cannot run on this system
    BackendError,       // backend failed during execution (e.g. device API error)
};

const char* to_string(Status status);

// Generic result for compute/resource operations that carry no data payload
// (buffer-based execution, upload/download, resource management).
struct ComputeResult {
    Status status = Status::Ok;
    std::string error;  // empty when status == Ok
};

// Vector Addition task: C[i] = A[i] + B[i].
// Both arrays must be non-empty and of equal size.
// Note: the addition is 32-bit modular addition; callers must avoid values
// whose sum overflows int32 (tests use safe ranges).
struct VectorAddTask {
    std::vector<std::int32_t> a;
    std::vector<std::int32_t> b;
};

// Result of executing a VectorAddTask.
// 'data' is valid only when status == Status::Ok.
struct VectorAddResult {
    Status status = Status::Ok;
    std::string error;               // empty when status == Ok
    std::vector<std::int32_t> data;  // C values when status == Ok
};

// Validates a task. Returns Status::Ok, or the reason the task is invalid:
//  - size mismatch between a and b        -> InvalidInput
//  - empty input (a.size() == 0 == b.size()-> InvalidInput ("empty")
//    a zero-element task has nothing to compute; it is rejected explicitly
//    instead of being treated as a silent success.
Status validate_vector_add(const VectorAddTask& task);

// Validates three buffer resources for buffer-based vector addition
// (Phase 4): a and b are inputs, c is the output. Enforced by BOTH backends
// and the Runtime so the rules cannot be bypassed:
//  - all three must be int32 element buffers (element_size == 4 bytes)
//  - element counts must be equal and non-zero
//  - a and b must have been created with ResourceAccess::Read
//  - c must have been created with ResourceAccess::Write
// On failure returns a status != Ok and fills 'error' with the reason.
Status validate_vector_add_buffers(const vortyx::resource::BufferDesc& a,
                                   const vortyx::resource::BufferDesc& b,
                                   const vortyx::resource::BufferDesc& c,
                                   std::string& error);

// ---------------------------------------------------------------------------
// Generic compute tasks (Phase 10 — Compute Engine)
// ---------------------------------------------------------------------------

// The operations Vortyx can execute today. The list is explicit and closed:
// a future operation is added here together with its shader/backend policy,
// never guessed from data. All current ops are elementwise int32 and
// therefore bit-exact on every backend (integer arithmetic is exact modulo
// 2^32 on CPU and GPU alike — overflow included).
enum class ComputeOp {
    VectorAdd,      // C[i] = A[i] + B[i]              (two inputs)
    VectorMultiply, // C[i] = A[i] * B[i]              (two inputs)
    VectorScale,    // C[i] = A[i] * scalar            (one input + scalar)
};

const char* to_string(ComputeOp op);

// Stable lowercase workload label used by exporters/logs
// ("vector_add", "vector_multiply", "vector_scale").
const char* workload_label(ComputeOp op);

// A generic elementwise compute task: WHICH operation, and its host-side
// inputs. This is the value-type workload description for the Phase 10
// compute engine — VectorAddTask (Phase 3) remains unchanged and supported.
//
// Data-parallel domain (Phase 13 partitioning seam):
//   Every current op is elementwise over [0, element_count()) — the task
//   already expresses the WHOLE workload as an explicit element count, and
//   each element is computed independently of the others. That is exactly
//   the property a future device/distributed phase needs to partition one
//   task into logical sub-ranges across workers. No partitioning API exists
//   yet (Phase 10 is single-Runtime by design); this domain definition is
//   the documented structural seam.
//
// Strict operand policy (invalid input is refused, never guessed about):
//   - VectorAdd / VectorMultiply: a and b must be non-empty and equal size;
//     scalar must be 0 (it is not an operand of these ops).
//   - VectorScale: a must be non-empty; scalar is the scale factor; b must
//     be empty (carrying an unused second input is a caller bug, refused).
//   - Integer semantics, identical on every backend (bit-exact):
//       VectorAdd follows the Phase 3 policy — callers keep sums inside
//       int32 range (documented at VectorAddTask).
//       VectorMultiply / VectorScale are DEFINED modular arithmetic: the
//       product is the low 32 bits of the true product (uint32 multiply,
//       two's complement), overflow included — the CPU backend computes it
//       with well-defined unsigned arithmetic, the GPU kernels define it
//       naturally, so cross-backend results are bit-exact by construction.
struct ComputeTask {
    ComputeOp op = ComputeOp::VectorAdd;

    // Primary input (all ops).
    std::vector<std::int32_t> a;

    // Second input (VectorAdd, VectorMultiply); must be empty for VectorScale.
    std::vector<std::int32_t> b;

    // Scalar operand (VectorScale); must be 0 for VectorAdd/VectorMultiply.
    std::int32_t scalar = 0;

    // Size of the data-parallel domain [0, element_count()). Returns
    // a.size() (the primary input drives the workload for every op).
    std::size_t element_count() const noexcept { return a.size(); }
};

// Validates a generic compute task against the strict operand policy above.
// Returns Status::Ok, or the failing Status and fills 'error' with a
// human-readable, operation-specific reason.
Status validate_compute_task(const ComputeTask& task, std::string& error);

// Result of executing one generic compute task. Same shape as
// VectorAddResult (the Phase 3 result), deliberately a SEPARATE named type:
// VectorAddResult stays the vocabulary of the unchanged Phase 3 API, while
// ComputeTaskResult is the payload type the generic engine (and its batch
// results) can evolve without touching the legacy API.
struct ComputeTaskResult {
    Status status = Status::Ok;
    std::string error;               // empty when status == Ok
    std::vector<std::int32_t> data;  // output values when status == Ok
};

// Result of ONE batch execution (Phase 10). A batch is a SYNCHRONOUS list of
// independent ComputeTasks executed in submission order on ONE backend —
// this is NOT the TaskQueue (which is the asynchronous FIFO layer with its
// own worker thread and its own lifecycle).
//
// Honest per-item semantics (implemented and tested, never improvised):
//   - Every task is attempted: invalid tasks fail as their own item
//     (InvalidInput, not executed); valid tasks execute even if an earlier
//     task failed. Successful results are NEVER discarded, failures are
//     NEVER hidden.
//   - results has exactly one entry per submitted task, in submission order
//     (empty only for wholesale refusals before any task ran: uninitialized
//     runtime, unknown/unavailable backend, empty batch).
//   - status is Ok only when every item succeeded; otherwise it is the
//     FIRST failing item's own status, and 'error' summarizes the counts
//     and the first failure. No new status vocabulary is invented.
struct BatchResult {
    Status status = Status::Ok;
    std::string error;                        // empty when status == Ok
    std::vector<ComputeTaskResult> results;   // one per task, submission order
    std::size_t succeeded = 0;                // items with status == Ok
    std::size_t failed = 0;                   // items with status != Ok
};

// Validates the buffer-level dispatch shape for one op (shared by the
// Runtime and BOTH backends, like validate_vector_add_buffers before it):
//   - all involved buffers are int32 element buffers (element_size == 4)
//   - element counts equal and non-zero across a, (b) and c
//   - inputs created with ResourceAccess::Read, output with Write
//   - op shape: VectorAdd/VectorMultiply require 'b_desc' (non-null);
//     VectorScale requires 'b_desc' == nullptr.
// 'b_desc' may be null exactly when the op takes a single input.
Status validate_compute_dispatch_buffers(ComputeOp op,
                                         const vortyx::resource::BufferDesc& a_desc,
                                         const vortyx::resource::BufferDesc* b_desc,
                                         const vortyx::resource::BufferDesc& c_desc,
                                         std::string& error);

}  // namespace vortyx::compute
