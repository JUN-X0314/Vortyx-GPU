#pragma once

// Service contract serialization (Phase 14).
//
// JSON views of the service's public value types, written with the EXISTING
// strict platform JSON writer (the Phase 11 dependency policy holds: no new
// serialization stack, no payload data — job/project records are METADATA;
// compute payloads and tensor data never appear here).
//
// The schemas mirror the Phase 11/12 contract style: stable field order,
// snake_case names, stable error codes from service_status_code(). Unknown
// field rejection stays a READER concern of whichever transport adopts
// these views (the same rule the platform contract applies).

#include <string>

#include "platform/auth.hpp"
#include "service/health.hpp"
#include "service/metrics.hpp"
#include "service/platform_service.hpp"
#include "service/project.hpp"
#include "service/service_status.hpp"

namespace vortyx::service {

// {"project_id":"...","owner_user_id":"...","name":"...","status":"active",
//  "created_at_ms":N,"updated_at_ms":N}
std::string serialize_project(const ProjectRecord& project);

// {"job_id":"...","project_id":"...","submitted_by":"...","operation":"...",
//  "element_count":N,"requested_shard_count":N,"requested_backend":"...",
//  "status":"queued|running|completed|failed|cancelled","error":"...",
//  "submitted_at_ms":N,"terminal_at_ms":N|null,
//  "total_shards":N|null,"succeeded_shards":N|null,"failed_shards":N|null}
std::string serialize_service_job(const ServiceJobView& job);

// {"error":{"code":"...","message":"..."}} — the machine-readable error
// body; the message is the caller's reason text (already secret-free by
// construction — error strings never carry tokens or payloads).
std::string serialize_service_error(ServiceStatus status, const std::string& message);

// {"submit_attempts":N,...,"jobs_queued":N,"jobs_running":N} — the real
// counters only (see metrics.hpp).
std::string serialize_metrics(const ServiceMetricsSnapshot& metrics);

// HealthReport::serialize() is declared on the struct itself (health.hpp).

}  // namespace vortyx::service
