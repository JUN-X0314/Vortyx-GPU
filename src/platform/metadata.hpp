#pragma once

// Platform metadata model (Phase 11).
//
// DeviceMetadata is how one Vortyx node describes ITSELF to the control
// plane at registration time. Every field is a SELF-REPORTED declaration:
// the control plane stores what the node claims, exactly and without
// interpretation. It never invents or completes values on the node's behalf
// (no fabrication, project-wide honesty rule).
//
// Phase 11 deliberately defines the MINIMAL stable set:
//   - protocol_version : which control-plane contract this node speaks
//                        (kProtocolVersion). A node speaking a different
//                        protocol version is rejected — a version mismatch
//                        must be a loud, explicit error, never a guess.
//   - software_version : the Vortyx version the node runs (e.g. "0.11.0").
//   - operating_system / architecture : honest self-report, optional
//                        (empty = "not reported"), free-form short strings.
//   - backends         : canonical backend names the node believes it can
//                        execute on ("cpu", "vulkan"). Optional; validated
//                        against the contract's known backend names so a
//                        typo cannot become a fake capability.
//   - operations       : workload labels the node supports ("vector_add",
//                        ...). Optional; validated against the Phase 10
//                        ComputeOp labels (the single source of the op
//                        vocabulary — no second enum is defined here).
//   - display_name     : a caller-chosen human label. NOT an identity, NOT
//                        derived from hardware, never used for matching.
//
// Future seams (documented, deliberately ABSENT in Phase 11 — no fake
// metrics): memory/compute capacity, connectivity, battery status, current
// load. Those arrive with Phase 12+ device agents that can actually measure
// them. A field that does not exist cannot lie.
//
// There is no remote heartbeat here: heartbeat is a control-plane OPERATION
// (store/API), metadata is only the description payload.

#include <string>
#include <vector>

#include "platform/status.hpp"

namespace vortyx::platform {

// The control-plane protocol version Phase 11 speaks and accepts. Bumping
// this is a contract change and must be accompanied by explicit migration
// documentation — never done silently.
inline constexpr const char* kProtocolVersion = "1";

// The canonical backend names the Phase 11 API contract recognizes in
// requested_backend / capabilities. This is the CONTRACT vocabulary, not a
// second discovery path: at execution time the Compute Runtime remains the
// only honest source of what is actually usable, and an explicit request for
// a backend the executor cannot verify stays an explicit failure (no silent
// fallback, Phase 5/7 honesty rules, unchanged).
//
// (Sorted alphabetically; the contract is a set, not a priority order.)
const std::vector<std::string>& known_backends();

// The canonical operation labels of the control-plane contract — exactly the
// Phase 10 ComputeOp workload labels ("vector_add", "vector_multiply",
// "vector_scale"), read through vortyx::compute::workload_label() so the op
// vocabulary keeps ONE source of truth.
const std::vector<std::string>& known_operations();

// True when 'name' is one of the canonical backend names above.
bool is_known_backend(const std::string& name);

// True when 'label' is one of the canonical operation labels above.
bool is_known_operation(const std::string& label);

// Self-description of one Vortyx node (see the module documentation).
struct DeviceMetadata {
    std::string protocol_version = kProtocolVersion;
    std::string software_version;      // e.g. VORTYX_VERSION_STRING
    std::string operating_system;      // e.g. "linux", "windows"; "" = not reported
    std::string architecture;          // e.g. "x86_64";      "" = not reported

    std::vector<std::string> backends;     // canonical backend names; may be empty
    std::vector<std::string> operations;   // canonical workload labels; may be empty

    std::string display_name;          // caller-chosen label; never an identity
};

// Validates metadata. Returns Status::Ok, or Status::InvalidInput with
// 'error' filled:
//   - protocol_version missing or != kProtocolVersion (explicit refusal —
//     an incompatible node must never be registered "as if it spoke V1")
//   - software_version empty (a node that cannot say what it runs cannot be
//     operated on honestly)
//   - unknown backend names / unknown operation labels (a typo must not
//     become a fake capability)
//   - duplicate backend or operation entries
//   - field lengths above the documented caps (see the .cpp)
Status validate_device_metadata(const DeviceMetadata& metadata, std::string& error);

}  // namespace vortyx::platform
