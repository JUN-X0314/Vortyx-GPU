#pragma once

// Worker protocol wire contract (Phase 15) — the C++ side of the native
// execution boundary.
//
// The control plane (the Vercel/local TypeScript API) owns the job
// lifecycle; the native worker claims work from it, executes on the real
// Vortyx stack, and reports outcomes. The protocol is JSON over HTTP with
// the SAME strict vocabulary the platform contract has used since Phase 11:
// snake_case fields, epoch-millisecond timestamps, NULL for "not set" (never
// a fabricated 0), unknown fields rejected by readers, and the unified
// {"error":{"code","message"}} body on failures.
//
// PAYLOADS NEVER CROSS THIS BOUNDARY: every message is metadata only
// (operation, element count, shard count, result metadata). The actual data
// an execution touches is synthesized deterministically by the native
// executor (native_executor.hpp) — the honest consequence of the
// metadata-only control plane, not a limitation to hide.
//
// Endpoint map (server side: platform/api; see docs/worker/):
//   POST {base}/api/worker/claim              -> ClaimResponse
//   POST {base}/api/worker/jobs/:id/heartbeat -> HeartbeatResponse
//   POST {base}/api/worker/jobs/:id/complete  -> CompleteResponse
//   POST {base}/api/worker/jobs/:id/fail      -> CompleteResponse (alias)

#include <cstdint>
#include <string>

#include "platform/json.hpp"

namespace vortyx::worker {

// The terminal statuses a worker may report.
inline constexpr const char* kTerminalCompleted = "completed";
inline constexpr const char* kTerminalFailed = "failed";
inline constexpr const char* kTerminalCancelled = "cancelled";

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

struct ClaimRequest {
    std::string worker_id;   // the claiming agent's stable label
    std::int64_t lease_ms = 0;  // the requested lease duration (bounded by the API)
};

struct ClaimedJob {
    std::string job_id;
    std::string project_id;
    std::string operation;  // "vector_add" | "vector_multiply" | "vector_scale"
    std::uint64_t element_count = 0;
    std::string requested_backend;  // "" = no preference
    std::uint32_t requested_shard_count = 1;
    std::uint32_t attempt = 0;
    std::int64_t lease_expires_at_ms = 0;
};

struct ClaimResponse {
    bool claimed = false;
    ClaimedJob job;       // meaningful only when claimed
    bool ok = true;       // false: the server refused the request (error below)
    std::string error_code;
    std::string error_message;
};

struct HeartbeatResponse {
    bool ok = true;
    bool accepted = false;       // false: the lease is gone (expired/reclaimed)
    bool cancel_requested = false;
    std::int64_t lease_expires_at_ms = 0;
    std::string error_code;
    std::string error_message;
};

struct CompletionReport {
    std::string terminal_status;  // completed | failed | cancelled
    std::string error;            // required for failed/cancelled
    std::string backend;          // "" = not reported
    std::uint64_t result_element_count = 0;
    bool has_result_element_count = false;
    // The REAL shard summary of the execution (the orchestrator record's
    // counts). has_shard_summary is false only for reports that never
    // reached the scheduler (pre-execution refusals).
    bool has_shard_summary = false;
    std::uint32_t shards_total = 0;
    std::uint32_t shards_succeeded = 0;
    std::uint32_t shards_failed = 0;
};

struct CompleteResponse {
    bool ok = true;
    bool recorded = false;   // false + ok: idempotent replay of an already-terminal job
    std::string status;      // the job's terminal status (echo/replay)
    std::string error_code;
    std::string error_message;
};

// ---------------------------------------------------------------------------
// Encoders (requests) — deterministic, strict-vocabulary builders
// ---------------------------------------------------------------------------

std::string encode_claim_request(const ClaimRequest& request);
std::string encode_heartbeat_request(const std::string& worker_id);
std::string encode_complete_request(const std::string& worker_id,
                                    const CompletionReport& report, std::string& error);

// ---------------------------------------------------------------------------
// Parsers (responses) — strict: unknown fields, wrong types and invalid
// values are refusals, never guessed around. On failure the 'ok'/'accepted'
// flag stays false and 'error_*' explains why.
// ---------------------------------------------------------------------------

void parse_claim_response(const std::string& body, ClaimResponse& out);
void parse_heartbeat_response(const std::string& body, HeartbeatResponse& out);
void parse_complete_response(const std::string& body, CompleteResponse& out);

// Shared strict parse for every response body: fills 'holder' with the
// parsed value. Returns true when the body is valid JSON AND a JSON object
// (the only shape responses take); false fills 'error' (malformed JSON, too
// deep, non-object) — callers treat that as an invalid response.
bool parse_response_object(const std::string& body, vortyx::platform::JsonValue& holder,
                           std::string& error);

// Extracts {"error":{"code","message"}} from a strict-parsed object (the
// unified failure body). Returns false when the shape is absent.
bool extract_error_body(const vortyx::platform::JsonValue& object, std::string& code,
                        std::string& message);

}  // namespace vortyx::worker
