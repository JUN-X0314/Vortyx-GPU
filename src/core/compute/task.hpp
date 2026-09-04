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

namespace vortyx::compute {

// Result status of a compute operation. Values are ordered so that
// status != Status::Ok always carries a meaningful 'error' string.
enum class Status {
    Ok,                 // operation succeeded
    InvalidInput,       // task data is invalid (size mismatch or empty input)
    NotInitialized,     // runtime/backend was not initialized before use
    BackendUnavailable, // requested backend is unknown or cannot run on this system
    BackendError,       // backend failed during execution (e.g. device API error)
};

const char* to_string(Status status);

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

}  // namespace vortyx::compute
