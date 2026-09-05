// Tensor device placement (Phase 13) — implementation.

#include "tensor/placement.hpp"

#include "platform/metadata.hpp"  // is_known_backend — the backend vocabulary

namespace vortyx::tensor {

const char* to_string(PlacementLocation location) {
    switch (location) {
        case PlacementLocation::Host: return "host";
        case PlacementLocation::Device: return "device";
    }
    return "unknown";
}

TensorStatus TensorPlacement::validate(std::string& error) const {
    switch (location) {
        case PlacementLocation::Host:
            if (!device_id.empty()) {
                error = "host placement must not carry a device id";
                return TensorStatus::InvalidPlacement;
            }
            break;
        case PlacementLocation::Device:
            if (device_id.empty()) {
                error = "device placement requires a device id";
                return TensorStatus::InvalidPlacement;
            }
            if (!vortyx::platform::is_valid_id(device_id)) {
                error = "device placement carries an invalid device id";
                return TensorStatus::InvalidPlacement;
            }
            break;
    }
    if (!backend.empty() && !vortyx::platform::is_known_backend(backend)) {
        error = "placement backend '" + backend + "' is not a canonical backend name";
        return TensorStatus::InvalidPlacement;
    }
    return TensorStatus::Ok;
}

bool TensorPlacement::same_place_as(const TensorPlacement& other) const {
    if (location != other.location) return false;
    if (location == PlacementLocation::Host) return true;
    return device_id == other.device_id && backend == other.backend;
}

std::string TensorPlacement::describe() const {
    if (location == PlacementLocation::Host) return "host";
    return "device:" + device_id + "/" + (backend.empty() ? std::string("default") : backend);
}

}  // namespace vortyx::tensor
