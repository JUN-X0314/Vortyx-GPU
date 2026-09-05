#pragma once

// Resource model (Phase 12) — the schedulable quantities of one device.
//
// A ResourceVector is the project's extensible resource abstraction: the
// three kinds Phase 12 schedules with are fixed here (ComputeUnits,
// MemoryBytes, ConcurrentJobs) as explicit named fields — the project style
// prefers honest, self-describing members over opaque maps. A future phase
// adds a kind by extending this struct and the helpers below; nothing else
// in the layer does its own arithmetic on the fields.
//
// Invariants (enforced by the helpers, pinned by tests):
//   no field is ever negative; allocated <= capacity; available >= 0;
//   a release never drives accounting negative (subtract clamps at 0, and
//   the registry never accepts a release that was not reserved).
//
// Units are part of the names: compute units are SELF-REPORTED capacity
// (simulator configuration — never a hardware measurement this code does
// not perform), memory is bytes, concurrency is a count of simultaneous
// shard executions.

#include <cstdint>
#include <string>

#include "core/compute/task.hpp"  // ComputeOp — the shared op vocabulary

namespace vortyx::distributed {

struct ResourceVector {
    // Self-reported compute capacity in abstract compute units. 0 is legal
    // (a device that reports no compute is simply not given compute work).
    std::int64_t compute_units = 0;
    // Memory capacity/usage in bytes (int64: element counts up to 2^31 with
    // several buffers reach 10+ GiB — int32 would overflow accounting).
    std::int64_t memory_bytes = 0;
    // How many shard executions may run on the device simultaneously.
    std::int64_t concurrent_jobs = 0;
};

// True when every field is >= 0. The base invariant of the model.
bool resource_vector_valid(const ResourceVector& vector);

// True when 'capacity' can hold 'used + request' in EVERY field
// (component-wise). Pure function; the registry's atomic reservation gate.
bool resource_vector_fits(const ResourceVector& capacity, const ResourceVector& used,
                          const ResourceVector& request);

// Component-wise sum.
ResourceVector resource_vector_add(const ResourceVector& a, const ResourceVector& b);

// Component-wise difference clamped at 0 (a release never makes accounting
// negative; the registry additionally refuses unknown releases outright).
ResourceVector resource_vector_sub(const ResourceVector& a, const ResourceVector& b);

// True when every field of 'a' is <= the corresponding field of 'b'.
bool resource_vector_le(const ResourceVector& a, const ResourceVector& b);

// Human-readable debug form: "compute=4 memory=8589934592 jobs=2" (stable
// field order; used by the cluster dump and the diagnostic tool).
std::string to_string(const ResourceVector& vector);

// Device-memory bytes one shard of 'element_count' elements of 'operation'
// needs: int32 elements (4 bytes) per buffer — two input buffers (a, b) plus
// the output buffer (c) for VectorAdd/VectorMultiply; input (a) + output (c)
// for VectorScale. Returns false with 'error' when the byte count would
// overflow int64 (absurd sizes are refused, never wrapped).
bool shard_memory_bytes(std::uint64_t element_count, vortyx::compute::ComputeOp operation,
                        std::int64_t& out_bytes, std::string& error);

}  // namespace vortyx::distributed
