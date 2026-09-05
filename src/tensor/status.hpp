#pragma once

// Tensor layer result vocabulary (Phase 13 — AI/ML Acceleration + Tensor
// Layer).
//
// The project keeps ONE error-model style everywhere (explicit result
// objects, no exceptions) but SEPARATE domain vocabularies per layer, each
// documented with its mapping to the neighbors (see platform/status.hpp for
// the precedent). TensorStatus is the vocabulary of the TENSOR layer:
// tensor math, tensor graph construction/planning and tensor execution.
//
// Relationship to the existing vocabularies (no drift, no duplication):
//   - vortyx::compute::Status (the local execution path) stays UNTOUCHED.
//     Tensor operations that run through the existing Runtime adapter
//     surface its statuses verbatim inside the failure reason;
//     tensor_status_to_compute_status() below is the documented, pure
//     mapping used at that boundary (and pinned by tests).
//   - vortyx::platform::Status is the CONTROL-PLANE vocabulary; the tensor
//     layer never returns it (placement planning reports its rejections as
//     TensorStatus values with stable snake_case codes, mirroring the
//     distributed layer's rejection-code style).
//
// Every value has a stable lowercase snake_case code string (the
// wire/observability vocabulary, same convention as the Phase 11/12 error
// codes). Callers must branch on the enum; the strings are for logs,
// contract payloads and tests.

#include <string>

#include "core/compute/task.hpp"  // vortyx::compute::Status (mapping target)

namespace vortyx::tensor {

enum class TensorStatus {
    Ok,                          // succeeded

    // --- argument / metadata validation (before anything executes) --------
    InvalidInput,                // generic malformed argument (null, empty set)
    InvalidShape,                // shape invalid or violates the op's shape rule
    InvalidStride,               // strides invalid (out of bounds / overflow)
    DtypeMismatch,               // inputs disagree on dtype (or op requires another)
    UnsupportedDtype,            // dtype not supported BY THE TARGET (capability)
    UnsupportedOperation,        // op not supported BY THE TARGET (capability)
    UnsupportedLayout,           // layout requirement not met (e.g. reshape of
                                 // a non-contiguous tensor)
    InvalidPlacement,            // placement metadata invalid or impossible

    // --- resource / capacity ------------------------------------------------
    ResourceLimitExceeded,       // an explicit project limit refused the request
                                 // (rank/nodes/bytes caps — resource-exhaustion defense)
    MemoryAllocationFailure,     // storage allocation failed (resource system
                                 // refused: cap, overflow, provider unavailable)

    // --- graph structure ------------------------------------------------------
    InvalidState,                // structurally impossible graph (cycle, duplicate
                                 // output, reference to an unknown node)

    // --- dispatch / execution ---------------------------------------------------
    DeviceCapabilityMismatch,    // requirements exceed every candidate device's
                                 // declared tensor capabilities
    TransferUnsupported,         // execution would require moving tensor data
                                 // between placements — Phase 13 has no cross-device
                                 // transfer and refuses it explicitly
    ExecutionFailure,            // the kernel/backend failed during execution
    NumericalValidationFailure,  // a defined numerical domain error occurred
                                 // (e.g. integer division by zero)

    NotInitialized,              // executor/backend/context used before setup
    Internal,                    // invariant broken (should never happen; honest marker)
};

const char* to_string(TensorStatus status);

// The stable snake_case error code ("invalid_shape", "unsupported_dtype", ...).
const char* tensor_status_code(TensorStatus status);

// Parses a code string back to the enum. False for unknown names (codes
// crossing a boundary are validated, never guessed).
bool tensor_status_from_code(const std::string& code, TensorStatus& out);

// The documented mapping onto the local execution vocabulary
// (vortyx::compute::Status) — used where a tensor outcome must be expressed
// in the existing compute result style, and pinned by tests:
//   Ok                         -> Ok
//   InvalidInput               -> InvalidInput
//   InvalidShape               -> InvalidInput
//   InvalidStride              -> InvalidInput
//   DtypeMismatch              -> InvalidInput
//   UnsupportedDtype           -> InvalidInput       (refused before execution,
//                                   like the Phase 10 strict operand policy)
//   UnsupportedOperation       -> InvalidInput
//   UnsupportedLayout          -> InvalidInput
//   InvalidPlacement           -> InvalidInput
//   ResourceLimitExceeded      -> InvalidInput       (a tensor-level limit is an
//                                   input refusal, like the Phase 4 size caps)
//   MemoryAllocationFailure    -> BackendError       (allocation happens inside
//                                   the backend-owned resource path)
//   InvalidState               -> InvalidInput
//   DeviceCapabilityMismatch   -> BackendUnavailable (no usable target exists
//                                   on this system for the request)
//   TransferUnsupported        -> BackendUnavailable
//   ExecutionFailure           -> BackendError
//   NumericalValidationFailure -> BackendError
//   NotInitialized             -> NotInitialized
//   Internal                   -> BackendError
vortyx::compute::Status tensor_status_to_compute_status(TensorStatus status);

// The reverse mapping at the same boundary (used by the runtime adapter
// backend when the existing engine reports its own status):
//   Ok                 -> Ok
//   InvalidInput       -> InvalidInput
//   NotInitialized     -> NotInitialized
//   BackendUnavailable -> DeviceCapabilityMismatch
//   BackendError       -> ExecutionFailure
TensorStatus tensor_status_from_compute_status(vortyx::compute::Status status);

}  // namespace vortyx::tensor
