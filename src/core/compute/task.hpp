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

}  // namespace vortyx::compute
