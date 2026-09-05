#pragma once

// Distributed API contract codec (Phase 12) — the C++ reference of the
// control-plane wire vocabulary for the DISTRIBUTED endpoints.
//
// Relationship to the Phase 11 contract (src/platform/contract.*): the
// same wire conventions — the unified error body, the same stable error
// codes, the same HTTP status mapping (contract.hpp/http_status), the same
// strict JSON module (platform/json.hpp) and the same deterministic
// field-order serialization. This module EXTENDS the contract for the
// distributed surface; it does not modify or duplicate it. It lives in
// vortyx::distributed (not vortyx::platform) because it serializes the
// distributed types — and the platform layer must not know them.
//
// SCOPE (metadata only — the Phase 11 payload rule holds):
//   - Requests carry NO compute payload. A distributed job submission over
//     the control plane is its JobEnvelope metadata + the shard count.
//     The local ComputeTask never appears on the wire.
//   - Responses carry placement/lifecycle metadata: shard states, ranges,
//     device ids, counts. No result DATA ever travels the control plane
//     (the assembled output lives with the local caller).
//
// Endpoints described here (mirrored by the TypeScript layer in
// platform/api):
//   GET  /api/cluster                     — the owner's cluster view
//   POST /api/distributed/jobs            — submit a distributed job
//   GET  /api/distributed/jobs/:id        — one distributed job
//   GET  /api/distributed/jobs/:id/shards — the job's shard table
//   POST /api/distributed/jobs/:id/cancel — cancel a distributed job

#include <string>

#include "distributed/cluster.hpp"
#include "distributed/job.hpp"
#include "distributed/orchestrator.hpp"
#include "distributed/shard.hpp"
#include "platform/contract.hpp"  // ParseOutcome, error codes, status mapping
#include "platform/status.hpp"

namespace vortyx::distributed::contract {

// ---------------------------------------------------------------------------
// Request parsing (JSON text -> model). platform::contract::ParseOutcome.
// ---------------------------------------------------------------------------

// POST /api/distributed/jobs.
// Schema: { "job_id": string, "operation": string, "element_count": number,
//           "requested_shard_count": number, "requested_backend": string?,
//           "priority": number?, "protocol_version": string,
//           "created_at_ms": number? }
// The parsed result is a control-plane submission: the JobEnvelope (the
// Phase 11 contract type, reused) plus the shard count. NO payload field
// exists — a request carrying one is rejected as an unknown field.
platform::contract::ParseOutcome parse_create_distributed_job(
    const std::string& body, vortyx::platform::JobEnvelope& envelope,
    std::uint32_t& requested_shard_count);

// ---------------------------------------------------------------------------
// Response serialization (model -> JSON text, documented field order)
// ---------------------------------------------------------------------------

// GET /api/cluster payload: the owner's cluster view.
std::string serialize_cluster_view(const ClusterSnapshot& snapshot);

// One distributed job (GET /api/distributed/jobs/:id and list entries):
// lifecycle metadata + the shard table.
std::string serialize_distributed_job(const DistributedJobRecord& job);

// The shard table alone (GET /api/distributed/jobs/:id/shards).
std::string serialize_shards(const DistributedJobRecord& job);

}  // namespace vortyx::distributed::contract
