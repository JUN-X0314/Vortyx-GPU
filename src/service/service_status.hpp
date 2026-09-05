#pragma once

// Service layer result vocabulary (Phase 14 — Production GPU Platform /
// Serviceization).
//
// The project keeps ONE error-model style everywhere (explicit result
// objects, no exceptions) but SEPARATE domain vocabularies per layer
// (platform::Status is the Phase 11 control-plane vocabulary, TensorStatus
// the Phase 13 tensor vocabulary). ServiceStatus is the vocabulary of the
// SERVICE layer: the Project / Authorization / Quota / Rate-limit / Queue /
// Job-Service control plane that WRAPS the existing layers (never replaces
// them).
//
// Why a new enum instead of extending platform::Status: the service
// introduces control-plane outcomes Phase 11 has no concept for — quota
// policy refusals and rate-limit refusals. Adding values to the Phase 11
// enum would change a frozen public vocabulary (and every exhaustive switch
// over it); a separate per-layer vocabulary is the established project
// pattern. Mapping to platform::Status is provided where the service
// forwards a platform/store outcome verbatim.
//
// Every value has a stable lowercase snake_case code (the wire /
// observability vocabulary, same convention as every layer's codes). Callers
// branch on the enum; strings are for logs, contract payloads and tests.
//
// HTTP semantics (the service contract's mapping, pinned by tests):
//   Ok                 -> 200
//   InvalidInput       -> 422 (malformed or semantically invalid request)
//   Unauthenticated    -> 401
//   Forbidden          -> 403
//   NotFound           -> 404 (anti-enumeration: unknown AND foreign
//                          resources are indistinguishable)
//   Conflict           -> 409 (duplicate / replayed with different payload /
//                          illegal state change)
//   QuotaExceeded      -> 429 (project policy limit; retry after capacity)
//   RateLimitExceeded  -> 429 (submission rate limit)
//   UnsupportedOperation -> 422 (the request is valid but the target state
//                          forbids it, e.g. submitting to an archived project)
//   Unavailable        -> 503 (wait timeout / queue at capacity)
//   Internal           -> 500

#include <string>

#include "platform/status.hpp"  // the Phase 11 vocabulary (mapping source)

namespace vortyx::service {

enum class ServiceStatus {
    Ok,
    InvalidInput,
    Unauthenticated,
    Forbidden,
    NotFound,
    Conflict,
    QuotaExceeded,
    RateLimitExceeded,
    UnsupportedOperation,
    Unavailable,
    Internal,
};

const char* to_string(ServiceStatus status);

// The stable snake_case error code ("quota_exceeded", ...).
const char* service_status_code(ServiceStatus status);

// Parses a code string back to the enum. False for unknown names (codes
// crossing a boundary are validated, never guessed).
bool service_status_from_code(const std::string& code, ServiceStatus& out);

// The documented mapping at the platform boundary (used wherever the service
// forwards a store/orchestrator outcome):
//   Ok -> Ok; InvalidInput -> InvalidInput; Unauthenticated -> Unauthenticated;
//   Forbidden -> Forbidden; NotFound -> NotFound; Conflict -> Conflict;
//   Internal -> Internal. (platform::Status has no quota/rate concepts, so
//   nothing maps onto those.)
ServiceStatus service_status_from_platform(vortyx::platform::Status status);

// The HTTP status the service contract assigns to each outcome (see the
// module header). Total function.
int service_status_http(ServiceStatus status);

}  // namespace vortyx::service
