#pragma once

// Vulkan device-memory buffer (Phase 4).
//
// The GPU-side implementation of the shared Buffer abstraction. This class
// OWNS one real Vulkan allocation:
//
//   VkBuffer + VkDeviceMemory (host-visible + host-coherent) + mapping
//
// and absorbs the buffer creation/teardown code that Phase 3 kept inside the
// backend's execute() (the per-execution ScopedBuffer pair). Owning RAII
// guarantees that buffer and memory are destroyed whenever the object dies —
// success, error, early return or shutdown — so GPU memory can never leak.
//
// Honesty notes:
//   - memory_location() is Device: the storage is a VkDeviceMemory
//     allocation from the device, not host memory. On integrated GPUs and
//     software implementations it may physically live in system RAM, but
//     from Vortyx's point of view it is device memory accessed through
//     explicit upload/download only.
//   - The memory type search requires HOST_VISIBLE | HOST_COHERENT (same
//     policy as Phase 3). Host-visible coherent memory is mapped once at
//     allocation time; upload/download are plain copies into/out of that
//     mapping. No staging buffers, no manual flushes — correctness first;
//     staging-based transfers are a future optimization, not a Phase 4 goal.
//   - Vulkan allocation failures (vkCreateBuffer / vkAllocateMemory /
//     vkBindBufferMemory) become explicit errors through the provider; a
//     partially created object destroys what it created.

#include <cstddef>
#include <memory>
#include <string>

#include "core/resource/backend_buffer.hpp"

#if defined(VORTYX_HAS_VULKAN)

#include <vulkan/vulkan.h>

namespace vortyx::resource {

class VulkanBuffer final : public IBufferImpl {
public:
    VulkanBuffer(const BufferDesc& desc, VkDevice device,
                 const VkPhysicalDeviceMemoryProperties& memory_properties);

    // RAII: destroys buffer + memory in the correct order (and unmaps).
    ~VulkanBuffer() override;

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    // Performs the real allocation: create buffer -> query requirements ->
    // select host-visible coherent memory type -> allocate -> bind -> map.
    // Returns false and fills 'error' on failure; the destructor then
    // releases whatever subset was created.
    bool initialize(std::string& error);

    const char* backend_name() const override { return "vulkan"; }
    MemoryLocation memory_location() const override { return MemoryLocation::Device; }

    bool upload(const void* src, std::size_t bytes, std::string& error) override;
    bool download(void* dst, std::size_t bytes, std::string& error) override;

    // API-specific handles for the Vulkan backend (descriptor binding and
    // device ownership checks). No generic "raw handle" is exposed through
    // the shared abstraction on purpose: Vulkan specifics stay in Vulkan code.
    VkBuffer vk_buffer() const { return buffer_; }
    VkDevice vk_device() const { return device_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;  // persistent mapping of host-visible coherent memory
    VkPhysicalDeviceMemoryProperties memory_properties_{};  // captured at construction
};

// Provider bound to one initialized Vulkan backend. Lives inside the
// backend's Impl so its lifetime can never exceed the VkDevice it needs.
class VulkanBufferProvider final : public IBufferProvider {
public:
    VulkanBufferProvider(VkDevice device,
                         const VkPhysicalDeviceMemoryProperties& memory_properties);

    const char* name() const override { return "vulkan"; }
    bool available() const override { return device_ != VK_NULL_HANDLE; }
    std::string unavailable_reason() const override {
        return "vulkan buffer provider has no initialized device";
    }

    std::unique_ptr<IBufferImpl> create_buffer(const BufferDesc& desc,
                                               std::string& error) override;

private:
    VkDevice device_;
    VkPhysicalDeviceMemoryProperties memory_properties_;
};

}  // namespace vortyx::resource

#endif  // VORTYX_HAS_VULKAN
