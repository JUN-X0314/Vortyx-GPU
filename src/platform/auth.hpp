#pragma once

// Authentication / authorization boundary (Phase 11).
//
// AuthN ("who are you?") and AuthZ ("may you touch this resource?") are kept
// strictly separate:
//
//   AuthContext  — the AUTHENTICATED subject the transport layer resolved
//                  from credentials (in production: the Supabase Auth JWT
//                  subject; in local/mock mode: the explicitly provided test
//                  subject). A client-CLAIMED user id inside a request body
//                  is NEVER an AuthContext and never trusted as one.
//   ownership    — the AUTHORIZATION rule: a user may access exactly the
//                  records whose owner_user_id equals the authenticated
//                  subject. Nothing else, no exceptions, no admin role in
//                  Phase 11.
//
// The ownership rule lives HERE as pure functions so three layers can agree
// on it and be tested against each other:
//   1. the C++ platform store (InMemoryPlatformStore — the local/mock
//      reference implementation),
//   2. the Vercel API layer (its own mirror of the same rule),
//   3. the Supabase Row Level Security policies (the DB-level backstop that
//      holds even if a server is misconfigured — see
//      platform/supabase/migrations).
// The rules are intentionally identical: the in-memory store is the
// executable specification of what RLS enforces for real.
//
// Refusal honesty: authorization failures distinguish Unauthenticated (no
// usable identity) from Forbidden (an identity that fails a rule). Which
// of the two a foreign resource produces is a SECURITY decision and the
// three layers agree on it:
//   * Single-record operations (device/job lookups, updates, results):
//     a record that is missing OR belongs to someone else produces
//     NotFound — identical to what Row Level Security does (a foreign row
//     is simply invisible). Reporting Forbidden would LEAK which ids exist
//     for other users; anti-enumeration is the stronger property and it is
//     what RLS already enforces.
//   * Cross-record references (submitting a job through a device the
//     caller does not own) produce Forbidden — exactly what the RLS
//     INSERT ... WITH CHECK policy rejects. No existence information is
//     disclosed here either (unknown and foreign devices are the same
//     Forbidden).
// The store layer translates the raw decision of authorize_record_access()
// accordingly; see memory_store.cpp for the concrete mapping.

#include <string>

#include "platform/identity.hpp"  // UserId
#include "platform/status.hpp"

namespace vortyx::platform {

// The authenticated caller identity. Produced by the transport boundary
// AFTER credential verification; platform logic consumes it read-only.
struct AuthContext {
    bool authenticated = false;   // usable, provider-verified identity present
    UserId user_id;               // provider-issued subject ("" when anonymous)
};

// Convenience constructor for transport layers that already verified a
// credential: authenticated with 'user_id'. An empty user_id is refused by
// validate_auth — a "verified" identity that names nobody is a bug.
AuthContext make_authenticated(UserId user_id);

// The anonymous context (used by tests and by transports with no
// credentials at all).
AuthContext anonymous();

// Validates that 'auth' carries a usable identity. Status::Ok, or
// Status::Unauthenticated with 'error' filled. A context with
// authenticated == true and an empty user_id is a caller bug and is
// rejected the same way (it names nobody).
Status validate_auth(const AuthContext& auth, std::string& error);

// True when 'auth' identifies exactly the owner of a record with
// owner_user_id. Both sides must be the same non-empty subject. Pure
// function — the single definition of ownership in the platform layer.
bool is_owner(const AuthContext& auth, const UserId& owner_user_id);

// Full access decision for a record owned by 'owner_user_id' (the RAW
// decision — see the module header for how stores translate foreign access
// into NotFound, mirroring RLS):
//   Status::Ok              — authenticated and owner
//   Status::Unauthenticated — no usable identity
//   Status::Forbidden       — authenticated, but this is not the caller's record
Status authorize_record_access(const AuthContext& auth, const UserId& owner_user_id);

}  // namespace vortyx::platform
