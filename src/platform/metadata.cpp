// Platform metadata implementation (Phase 11).

#include "platform/metadata.hpp"

#include <set>

#include "core/compute/task.hpp"  // ComputeOp workload labels (single source)

namespace vortyx::platform {

// Documented length caps (plain sanity bounds; ids have their own rule in
// identity.hpp). A node reporting more than this is malformed, not exotic.
namespace {
constexpr std::size_t kMaxVersionLength = 128;
constexpr std::size_t kMaxOsArchLength = 64;
constexpr std::size_t kMaxDisplayNameLength = 256;
constexpr std::size_t kMaxCapabilityEntries = 64;
}  // namespace

const std::vector<std::string>& known_backends() {
    static const std::vector<std::string> names = {"cpu", "vulkan"};
    return names;
}

const std::vector<std::string>& known_operations() {
    static const std::vector<std::string> labels = {
        vortyx::compute::workload_label(vortyx::compute::ComputeOp::VectorAdd),
        vortyx::compute::workload_label(vortyx::compute::ComputeOp::VectorMultiply),
        vortyx::compute::workload_label(vortyx::compute::ComputeOp::VectorScale),
    };
    return labels;
}

bool is_known_backend(const std::string& name) {
    for (const std::string& known : known_backends()) {
        if (known == name) return true;
    }
    return false;
}

bool is_known_operation(const std::string& label) {
    for (const std::string& known : known_operations()) {
        if (known == label) return true;
    }
    return false;
}

Status validate_device_metadata(const DeviceMetadata& metadata, std::string& error) {
    if (metadata.protocol_version != kProtocolVersion) {
        error = "unsupported protocol version '" + metadata.protocol_version +
                "' (this control plane speaks '" + kProtocolVersion + "')";
        return Status::InvalidInput;
    }
    if (metadata.software_version.empty()) {
        error = "software_version is required (a node must honestly report what it runs)";
        return Status::InvalidInput;
    }
    if (metadata.software_version.size() > kMaxVersionLength) {
        error = "software_version exceeds " + std::to_string(kMaxVersionLength) + " characters";
        return Status::InvalidInput;
    }
    if (metadata.operating_system.size() > kMaxOsArchLength) {
        error = "operating_system exceeds " + std::to_string(kMaxOsArchLength) + " characters";
        return Status::InvalidInput;
    }
    if (metadata.architecture.size() > kMaxOsArchLength) {
        error = "architecture exceeds " + std::to_string(kMaxOsArchLength) + " characters";
        return Status::InvalidInput;
    }
    if (metadata.display_name.size() > kMaxDisplayNameLength) {
        error = "display_name exceeds " + std::to_string(kMaxDisplayNameLength) + " characters";
        return Status::InvalidInput;
    }

    // Capability lists: known names only, no duplicates. A duplicate entry
    // is a data-quality defect, not extra capability — refuse it.
    std::set<std::string> seen;
    if (metadata.backends.size() > kMaxCapabilityEntries) {
        error = "backends exceeds " + std::to_string(kMaxCapabilityEntries) + " entries";
        return Status::InvalidInput;
    }
    for (const std::string& backend : metadata.backends) {
        if (!is_known_backend(backend)) {
            error = "unknown backend '" + backend + "' in backends (known: cpu, vulkan)";
            return Status::InvalidInput;
        }
        if (!seen.insert(backend).second) {
            error = "duplicate backend entry '" + backend + "'";
            return Status::InvalidInput;
        }
    }
    if (metadata.operations.size() > kMaxCapabilityEntries) {
        error = "operations exceeds " + std::to_string(kMaxCapabilityEntries) + " entries";
        return Status::InvalidInput;
    }
    seen.clear();
    for (const std::string& op : metadata.operations) {
        if (!is_known_operation(op)) {
            error = "unknown operation '" + op + "' in operations (known: vector_add, vector_multiply, vector_scale)";
            return Status::InvalidInput;
        }
        if (!seen.insert(op).second) {
            error = "duplicate operation entry '" + op + "'";
            return Status::InvalidInput;
        }
    }

    error.clear();
    return Status::Ok;
}

}  // namespace vortyx::platform
