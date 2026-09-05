#pragma once

// Platform identity model (Phase 11).
//
// One "device" is one installation/execution node of Vortyx that can register
// itself with the control plane (the platform layer of Phase 12). Phase 11
// defines ONLY the identity vocabulary and its validation rules:
//
//   DeviceId — client-generated unique id of one node/installation.
//              Generated as a random UUID (v4-format string) or supplied
//              explicitly by the caller. NO hardware fingerprint, NO MAC
//              address, NO serial number, NO personally identifying
//              information is collected or derived anywhere in Phase 11.
//   JobId    — client-generated unique id of one submitted job. Doubles as
//              the IDEMPOTENCY KEY of job submission (resubmitting the same
//              id with the same payload must not create a second job).
//   UserId   — the control-plane identity of a human user, ISSUED BY THE
//              AUTHENTICATION PROVIDER (Supabase Auth's user id). Phase 11
//              never generates a UserId and never stores credentials; it
//              only carries the authenticated subject through ownership
//              checks.
//
// Id syntax (all three): 1..128 characters of [A-Za-z0-9._-] — safe in URLs,
// JSON and logs, and strictly validated BEFORE anything is stored. The
// random generators below produce canonical lowercase UUID v4 strings, which
// satisfy the same rule.
//
// Uniqueness honesty: the UUID generator uses std::random_device. That is a
// uniqueness mechanism for identifiers, not a security or identity claim —
// ids are only ever collision-avoidance labels, never proof of anything.

#include <cstdint>
#include <string>

#include "platform/status.hpp"

namespace vortyx::platform {

// Id types are plain strings on purpose: the control plane treats them as
// opaque tokens (the DATABASE may later constrain the format; the C++ model
// stays provider-neutral). Strong validation is applied at every boundary.
using DeviceId = std::string;
using JobId = std::string;
using UserId = std::string;

// Maximum id length accepted by every validator.
inline constexpr std::size_t kMaxIdLength = 128;

// True when 'value' is a syntactically valid id (1..kMaxIdLength chars of
// [A-Za-z0-9._-]). Pure function; used by every platform validator.
bool is_valid_id(const std::string& value);

// Validates an id and fills 'error' with a human-readable reason when
// invalid. Returns Status::Ok or Status::InvalidInput. 'label' names the id
// kind in the error ("device_id", "job_id", ...).
Status validate_id(const std::string& label, const std::string& value, std::string& error);

// Generates a random UUID (v4 format, lowercase, e.g.
// "3f2b8a1c-9d4e-4a7b-8c2f-0e5d6a9b1c3d") for use as a DeviceId or JobId.
// Deterministic tests never depend on the concrete value — only on its
// format and uniqueness.
DeviceId generate_device_id();
JobId generate_job_id();

}  // namespace vortyx::platform
