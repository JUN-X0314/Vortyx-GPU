#pragma once

// Compute Resource abstractions (Phase 4).
//
// Phase 4 separates "the calculation" from "the resources the calculation
// needs". A compute task no longer owns raw arrays that backends copy around:
// the Runtime asks the Resource Manager for buffers, writes data into them,
// lets a backend compute directly on them, and reads results back out.
//
// Layering (Phase 4):
//   Application -> Runtime -> Resource Manager -> Backend -> Device
//
// Scope discipline (Phase 4):
//  - Exactly one resource kind exists: Buffer. More kinds can be added later
//    without changing the concepts below.
//  - No memory pool / suballocation, no pinning, no unified memory tricks.
//    CPU memory and GPU device memory are NOT treated as the same thing:
//    MemoryLocation states where a buffer's storage actually lives, and all
//    host <-> buffer data movement goes through explicit upload/download
//    (Buffer::write / Buffer::read).
//  - The Resource Manager manages LIFECYCLE only. It never picks devices,
//    never schedules, never queues. Device selection is explicit by backend
//    name; scheduling arrives in a later phase.

#include <cstddef>
#include <cstdint>
#include <string>

namespace vortyx::resource {

// ---------------------------------------------------------------------------
// Resource identity
// ---------------------------------------------------------------------------

// Opaque handle to a live resource inside a ResourceManager.
// 0 is reserved as "no resource". Ids are never reused (monotonic counter),
// so a stale handle can never accidentally alias a newly created resource.
using ResourceId = std::uint64_t;

inline constexpr ResourceId kInvalidResourceId = 0;

// ---------------------------------------------------------------------------
// Memory location
// ---------------------------------------------------------------------------

// Where a buffer's storage physically lives. CPU memory and GPU memory are
// different things and are reported honestly:
//  - Host:   ordinary CPU memory (the CPU backend's buffers).
//  - Device: memory allocated from the backend's device (e.g. a Vulkan
//            VkDeviceMemory allocation). The host cannot access it directly;
//            all data movement happens through upload/download.
enum class MemoryLocation {
    Unknown = 0,
    Host,
    Device,
};

const char* to_string(MemoryLocation location);

// ---------------------------------------------------------------------------
// Access intent (device/shader side)
// ---------------------------------------------------------------------------

// What a compute operation may do with a buffer. This is the INPUT/OUTPUT
// role from the device's point of view, declared at creation time and
// validated when a task executes (e.g. vector addition inputs need Read,
// the output needs Write). It is independent of the host-side transfer
// operations write()/read(), which stage data in and out of the buffer.
enum class ResourceAccess : std::uint32_t {
    None = 0,
    Read = 1u << 0,   // the device may read this buffer (input)
    Write = 1u << 1,  // the device may write this buffer (output)
};

inline constexpr ResourceAccess operator|(ResourceAccess a, ResourceAccess b) {
    return static_cast<ResourceAccess>(static_cast<std::uint32_t>(a) |
                                       static_cast<std::uint32_t>(b));
}

inline bool has_access(ResourceAccess value, ResourceAccess flag) {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

const char* to_string(ResourceAccess access);

// ---------------------------------------------------------------------------
// Buffer description
// ---------------------------------------------------------------------------

// Safety cap for a single buffer allocation. Phase 4 prioritizes correctness
// and resource safety over capacity: absurd allocation requests must fail
// cleanly instead of wrapping around or bringing the process down. The limit
// is deliberately conservative and will be revisited together with the
// future memory pool / suballocation phase.
inline constexpr std::size_t kMaxBufferBytes = std::size_t{1} << 30;  // 1 GiB

// Description of a buffer resource, fixed at creation time.
//
// A buffer is an array of `element_count` elements, each `element_size`
// bytes wide. Element size (not a full type system) is the minimal data
// representation a compute backend needs; vector addition requires 4-byte
// (int32) elements today. Byte size is always element_count * element_size
// and overflow is rejected during validation, never silently wrapped.
struct BufferDesc {
    std::size_t element_count = 0;
    std::size_t element_size = 0;  // bytes per element
    ResourceAccess access = ResourceAccess::None;

    // Convenience factory: BufferDesc::of<std::int32_t>(16, ResourceAccess::Read)
    template <typename T>
    static constexpr BufferDesc of(std::size_t count, ResourceAccess access) {
        return BufferDesc{count, sizeof(T), access};
    }

    // element_count * element_size. Only meaningful after validate_buffer_desc
    // accepted the description (returns a wrapped value on overflow otherwise).
    std::size_t byte_size() const { return element_count * element_size; }
};

// Validates a buffer description. Returns an empty string when the
// description is usable; otherwise a human-readable reason it is not:
//   - element_count == 0                          -> zero-element buffers are rejected
//   - element_size == 0                           -> invalid element size
//   - access == None                              -> buffers must declare an access role
//   - element_count * element_size overflows      -> overflow rejected, never wrapped
//   - byte size > kMaxBufferBytes                 -> beyond the per-buffer safety limit
std::string validate_buffer_desc(const BufferDesc& desc);

}  // namespace vortyx::resource
