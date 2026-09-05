#pragma once

// API contract codec (Phase 11) — the C++ reference implementation of the
// control-plane HTTP contract.
//
// The Vercel-hosted API layer (platform/api, TypeScript) implements THIS
// contract — same routes, same request/response schemas, same error codes,
// same status-code mapping. This module is that contract's executable
// definition on the C++ side: the Phase 11 test suite runs real request
// bodies through it, so the wire format is pinned by tests on both sides
// of the future deployment.
//
// Scope discipline: this module knows NOTHING about HTTP, sockets or
// servers. It maps JSON text <-> platform models and platform outcomes ->
// documented status codes. The HTTP plumbing lives in the TypeScript layer.
//
// Error schema (every failing response, in BOTH layers):
//   { "error": { "code": "<stable machine code>", "message": "<human text>" } }
//
// Status-code mapping (the single source: contract_http_status below):
//   200 Ok | 400 invalid_json | 401 Unauthenticated | 403 Forbidden
//   404 NotFound | 409 Conflict | 422 semantic InvalidInput (every
//   error code except invalid_json) | 500 Internal.
// The 400/422 split is the documented rule: unparseable JSON is a malformed
// REQUEST (400); a parsed request that violates the schema is a semantic
// failure (422).

#include <cstdint>
#include <optional>
#include <string>

#include "platform/job.hpp"
#include "platform/metadata.hpp"
#include "platform/status.hpp"
#include "platform/store.hpp"  // DeviceRecord / JobRecord / ResultEnvelope

namespace vortyx::platform::contract {

// ---------------------------------------------------------------------------
// Stable error codes (the "error.code" vocabulary of the API contract)
// ---------------------------------------------------------------------------

inline constexpr const char* kErrInvalidJson = "invalid_json";                    // 400
inline constexpr const char* kErrMissingField = "missing_field";                  // 422
inline constexpr const char* kErrInvalidType = "invalid_type";                    // 422
inline constexpr const char* kErrInvalidEnum = "invalid_enum";                    // 422
inline constexpr const char* kErrInvalidId = "invalid_id";                        // 422
inline constexpr const char* kErrInvalidValue = "invalid_value";                  // 422
inline constexpr const char* kErrUnsupportedProtocol = "unsupported_protocol_version";  // 422
inline constexpr const char* kErrUnauthenticated = "unauthenticated";             // 401
inline constexpr const char* kErrForbidden = "forbidden";                         // 403
inline constexpr const char* kErrNotFound = "not_found";                          // 404
inline constexpr const char* kErrConflict = "conflict";                           // 409
inline constexpr const char* kErrMethodNotAllowed = "method_not_allowed";         // 405
inline constexpr const char* kErrInternal = "internal_error";                     // 500

// ---------------------------------------------------------------------------
// HTTP status mapping
// ---------------------------------------------------------------------------

// The documented mapping (see the module header). 'error_code' refines
// Status::InvalidInput into 400 (invalid_json) vs 422 (everything else).
int http_status(Status status, const std::string& error_code);

// ---------------------------------------------------------------------------
// Error responses
// ---------------------------------------------------------------------------

// Builds the unified error body: {"error":{"code":...,"message":...}}.
std::string error_body(const std::string& code, const std::string& message);

// Maps a platform outcome (the stores' vocabulary) to a contract error
// code — used by transport layers so store failures and contract failures
// produce the same schema.
const char* store_error_code(Status status);

// ---------------------------------------------------------------------------
// Request parsing / validation (body -> model)
// ---------------------------------------------------------------------------

// One parse outcome: platform Status plus the stable error code and the
// human message for the error body.
struct ParseOutcome {
    Status status = Status::Ok;
    std::string error_code;  // stable code (see above); empty on success
    std::string message;     // human-readable reason; empty on success
    int http_status_code = 200;  // ready-to-use HTTP status for this outcome

    bool ok() const { return status == Status::Ok; }
};

// POST /api/devices — register a device.
// Schema: { "device_id": string, "protocol_version": string,
//           "software_version": string, "operating_system": string?,
//           "architecture": string?, "display_name": string?,
//           "backends": string[]?, "operations": string[]? }
ParseOutcome parse_register_device(const std::string& body, DeviceId& device_id,
                                   DeviceMetadata& metadata);

// POST /api/jobs — submit a job.
// Schema: { "job_id": string, "operation": string, "element_count": number,
//           "requested_backend": string?, "priority": number?,
//           "protocol_version": string, "created_at_ms": number?,
//           "submitted_by_device_id": string? }
// The optional device reference is NEVER trusted: the store proves it
// exists AND is owned by the authenticated user (Forbidden otherwise).
ParseOutcome parse_create_job(const std::string& body, JobEnvelope& envelope,
                              std::optional<DeviceId>& submitted_by);

// ---------------------------------------------------------------------------
// Response serialization (model -> JSON text, documented field order)
// ---------------------------------------------------------------------------

// Device record as returned by /api/devices* (server fields included).
std::string serialize_device(const DeviceRecord& record);

// Job record as returned by /api/jobs*.
std::string serialize_job(const JobRecord& record);

// Result envelope as returned by /api/jobs/:id/result (via result()).
std::string serialize_result(const ResultEnvelope& result);

// GET /api/platform/info payload: what this control plane speaks.
std::string serialize_platform_info();

}  // namespace vortyx::platform::contract
