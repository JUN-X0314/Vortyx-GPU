#pragma once

// Tensor device placement (Phase 13).
//
// WHERE a tensor's data lives / where an execution is TARGETED, expressed
// with the EXISTING Vortyx identity and backend vocabulary — no second ID
// scheme:
//   - Host:            host memory through the Phase 4 resource system
//                      (the "cpu" buffer provider).
//   - Device:          execution/storage bound to one registered cluster
//                      device (vortyx::platform::DeviceId — the Phase 11/12
//                      identity, reused verbatim) and one backend name
//                      ("cpu"/"vulkan" — the platform contract vocabulary).
//
// HONEST TRANSFER SEMANTICS (documented, tested, never faked):
//   Phase 13 implements NO cross-device tensor transfer. An execution whose
//   input tensors live on a DIFFERENT non-host placement than the execution
//   target fails with TensorStatus::TransferUnsupported — it never fabricates
//   a copy, never shares pointers across "devices", never silently re-homes
//   data. Host-placed inputs are readable by any local execution (the host is
//   the one place every Phase 13 execution can read).
//
// HONEST SYNCHRONY: Phase 13 has no asynchronous copy engine. All storage
// reads/writes are synchronous (documented at TensorStorage); nothing here
// pretends to be async.

#include <string>

#include "platform/identity.hpp"  // DeviceId — the Phase 11/12 identity, reused
#include "tensor/status.hpp"

namespace vortyx::tensor {

enum class PlacementLocation : std::uint8_t {
    Host = 0,
    Device = 1,
};

const char* to_string(PlacementLocation location);

struct TensorPlacement {
    PlacementLocation location = PlacementLocation::Host;

    // Meaningful when location == Device: the registered cluster device this
    // tensor is bound to (vortyx::platform::DeviceId — reused, never a new
    // identity scheme). Empty for Host.
    vortyx::platform::DeviceId device_id;

    // The execution/storage backend ("cpu" today for tensor storage; a
    // device placement names the backend its execution will use). Empty =
    // the executor's default. Validated against the platform backend
    // vocabulary when non-empty (see validate below).
    std::string backend;

    // The canonical Host placement (host memory, default backend).
    static TensorPlacement host() { return TensorPlacement{}; }

    // A device-bound placement. 'device_id' must be a syntactically valid
    // platform id (validated in validate()).
    static TensorPlacement on_device(const vortyx::platform::DeviceId& device_id,
                                     const std::string& backend) {
        TensorPlacement p;
        p.location = PlacementLocation::Device;
        p.device_id = device_id;
        p.backend = backend;
        return p;
    }

    // Validation. Returns Ok, or the failing TensorStatus with 'error':
    //   - Device placement with an invalid/empty device id -> InvalidPlacement
    //   - non-empty backend that is not a canonical platform backend name
    //     -> InvalidPlacement (a typo must not become a fake backend)
    //   - Host placement carrying a device id               -> InvalidPlacement
    TensorStatus validate(std::string& error) const;

    // True when both placements are the same place (same location and, for
    // Device, the same device id + backend). Pure.
    bool same_place_as(const TensorPlacement& other) const;

    // Human-readable deterministic form ("host", "device:<id>/<backend>").
    std::string describe() const;

    friend bool operator==(const TensorPlacement& a, const TensorPlacement& b) {
        return a.location == b.location && a.device_id == b.device_id && a.backend == b.backend;
    }
    friend bool operator!=(const TensorPlacement& a, const TensorPlacement& b) {
        return !(a == b);
    }
};

}  // namespace vortyx::tensor
