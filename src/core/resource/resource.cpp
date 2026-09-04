#include "core/resource/resource.hpp"

#include <limits>

namespace vortyx::resource {

const char* to_string(MemoryLocation location) {
    switch (location) {
        case MemoryLocation::Host: return "Host";
        case MemoryLocation::Device: return "Device";
        case MemoryLocation::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* to_string(ResourceAccess access) {
    // Composed flag values (e.g. Read|Write) are valid enum values but not
    // named enumerators, so flag combination is checked with if/else instead
    // of a switch over enumerators.
    if (access == ResourceAccess::None) return "None";
    if (access == ResourceAccess::Read) return "Read";
    if (access == ResourceAccess::Write) return "Write";
    if (access == (ResourceAccess::Read | ResourceAccess::Write)) return "Read|Write";
    return "Invalid";
}

std::string validate_buffer_desc(const BufferDesc& desc) {
    if (desc.element_count == 0) {
        return "zero-element buffers are not supported (element_count must be > 0)";
    }
    if (desc.element_size == 0) {
        return "element_size must be > 0 bytes";
    }
    if (desc.access == ResourceAccess::None) {
        return "buffer access must be declared (ResourceAccess::Read and/or Write), not None";
    }
    // Overflow check BEFORE any size math is trusted: byte size must never
    // silently wrap around.
    if (desc.element_count > std::numeric_limits<std::size_t>::max() / desc.element_size) {
        return "buffer byte size overflows (element_count=" +
               std::to_string(desc.element_count) + " x element_size=" +
               std::to_string(desc.element_size) + ")";
    }
    if (desc.byte_size() > kMaxBufferBytes) {
        return "buffer request of " + std::to_string(desc.byte_size()) +
               " bytes exceeds the per-buffer safety limit of " +
               std::to_string(kMaxBufferBytes) + " bytes (kMaxBufferBytes)";
    }
    return {};
}

}  // namespace vortyx::resource
