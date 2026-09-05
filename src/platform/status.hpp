#pragma once

// Platform layer result vocabulary (Phase 11 — Platform / Cloud Layer
// Foundation).
//
// The platform layer is the boundary between the local Vortyx GPU engine and
// the future cloud control plane (Supabase-backed store, Vercel-hosted API).
// Its outcomes are CONTROL-PLANE outcomes and they map one-to-one onto HTTP
// semantics the API contract must express:
//
//   Status::Ok              -> 200 (success)
//   Status::InvalidInput    -> 400 (malformed JSON) or 422 (semantic
//                              validation failure — the contract layer's
//                              error code decides which)
//   Status::Unauthenticated -> 401 (no / invalid credentials)
//   Status::Forbidden       -> 403 (authenticated, but not the resource owner)
//   Status::NotFound        -> 404 (no such resource for this owner)
//   Status::Conflict        -> 409 (duplicate id / already-terminal state)
//   Status::Internal        -> 500 (unexpected store failure)
//
// This enum deliberately does NOT reuse vortyx::compute::Status. That enum is
// the vocabulary of the LOCAL compute path (task/resource/backend failures)
// and stays untouched and unchanged; the control plane needs authorization
// and conflict outcomes that the compute path has no concept for. This is a
// SEPARATE domain vocabulary for a separate layer — not a second copy of the
// compute error model. No platform type leaks into the compute layers and no
// compute type is redefined here.
//
// Everything below the platform layer (core Runtime, Virtual GPU, Scheduler,
// TaskQueue, backends) never sees this enum.

namespace vortyx::platform {

enum class Status {
    Ok,              // operation succeeded
    InvalidInput,    // request/record rejected before touching state
                     // (malformed, missing field, invalid id/enum/value,
                     // unsupported protocol version, illegal transition)
    Unauthenticated, // no credentials, or credentials that identify nobody
    Forbidden,       // authenticated user is not the resource owner (or the
                     // referenced resource belongs to someone else)
    NotFound,        // the requested resource does not exist (for this owner)
    Conflict,        // duplicate id with a different payload, or an update
                     // that collides with an already-terminal state
    Internal,        // the store itself failed (should never happen in the
                     // in-memory store; reserved for real backends)
};

const char* to_string(Status status);

}  // namespace vortyx::platform
