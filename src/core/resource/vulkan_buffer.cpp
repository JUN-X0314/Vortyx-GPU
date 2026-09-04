// Vulkan buffer implementation (Phase 4).
//
// Real Vulkan buffer allocation + data transfer, moved here from the
// Phase 3 backend execute() so that ALL GPU storage now flows through the
// Resource layer. Without Vulkan compiled in this file is empty; the
// Runtime simply has no "vulkan" buffer provider then (CPU-only builds keep
// working unchanged).

#include "core/resource/vulkan_buffer.hpp"

#if defined(VORTYX_HAS_VULKAN)

#include <cstring>

namespace vortyx::resource {

namespace {

std::string vk_result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_INVALID_DEVICE_ADDRESS_EXT: return "VK_ERROR_INVALID_DEVICE_ADDRESS";
        default: return "VkResult " + std::to_string(static_cast<int>(result));
    }
}

}  // namespace

VulkanBuffer::VulkanBuffer(const BufferDesc& desc, VkDevice device,
                           const VkPhysicalDeviceMemoryProperties& memory_properties)
    : IBufferImpl(desc), device_(device), memory_properties_(memory_properties) {}

VulkanBuffer::~VulkanBuffer() {
    // Parent-first safety: buffer and memory belong to device_; destroy them
    // before the device itself goes away. The Vulkan backend guarantees this
    // ordering (ResourceManager::shutdown purges all buffers BEFORE the
    // backend's VkDevice is destroyed).
    if (device_ != VK_NULL_HANDLE) {
        if (mapped_ != nullptr) {
            vkUnmapMemory(device_, memory_);
            mapped_ = nullptr;
        }
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer_, nullptr);
            buffer_ = VK_NULL_HANDLE;
        }
        if (memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory_, nullptr);
            memory_ = VK_NULL_HANDLE;
        }
    }
}

bool VulkanBuffer::initialize(std::string& error) {
    if (device_ == VK_NULL_HANDLE) {
        error = "no Vulkan device";
        return false;
    }
    if (buffer_ != VK_NULL_HANDLE || memory_ != VK_NULL_HANDLE) {
        error = "vulkan buffer is already initialized";
        return false;
    }

    // --- 1. Create the buffer (storage buffer usage: compute reads/writes it).
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = static_cast<VkDeviceSize>(byte_size());
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult vk = vkCreateBuffer(device_, &buffer_info, nullptr, &buffer_);
    if (vk != VK_SUCCESS) {
        error = "vkCreateBuffer failed (" + vk_result_name(vk) + ")";
        return false;
    }

    // --- 2. Query allocation requirements (size >= requested due to alignment).
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);

    // --- 3. Find a host-visible + host-coherent memory type (same policy as
    // Phase 3: map once, plain copies, no staging, no flush handling).
    std::int32_t memory_type = -1;
    for (std::uint32_t t = 0; t < memory_properties_.memoryTypeCount; ++t) {
        const bool bits_ok = (requirements.memoryTypeBits & (1u << t)) != 0;
        const bool props_ok =
            (memory_properties_.memoryTypes[t].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) != 0;
        if (bits_ok && props_ok) {
            memory_type = static_cast<std::int32_t>(t);
            break;
        }
    }
    if (memory_type < 0) {
        error = "no host-visible coherent memory type found for storage buffer";
        return false;
    }

    // --- 4. Allocate the device memory. A failure here (e.g.
    // VK_ERROR_OUT_OF_DEVICE_MEMORY) is an explicit error, never a crash;
    // the destructor releases the buffer created above.
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = static_cast<std::uint32_t>(memory_type);

    vk = vkAllocateMemory(device_, &alloc_info, nullptr, &memory_);
    if (vk != VK_SUCCESS) {
        error = "vkAllocateMemory failed (" + vk_result_name(vk) + ")";
        return false;
    }

    // --- 5. Bind the buffer to the memory.
    vk = vkBindBufferMemory(device_, buffer_, memory_, 0);
    if (vk != VK_SUCCESS) {
        error = "vkBindBufferMemory failed (" + vk_result_name(vk) + ")";
        return false;
    }

    // --- 6. Map once for the whole allocation; upload/download copy through
    // this mapping. HOST_COHERENT makes the copies visible without explicit
    // flushes/invalidate.
    vk = vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_);
    if (vk != VK_SUCCESS || mapped_ == nullptr) {
        error = "vkMapMemory failed (" + vk_result_name(vk) + ")";
        mapped_ = nullptr;
        return false;
    }
    return true;
}

bool VulkanBuffer::upload(const void* src, std::size_t bytes, std::string& error) {
    if (mapped_ == nullptr) {
        error = "vulkan buffer has no mapping (not initialized?)";
        return false;
    }
    if (src == nullptr) {
        error = "null source pointer";
        return false;
    }
    if (bytes == 0 || bytes > byte_size()) {
        error = "upload of " + std::to_string(bytes) + " bytes exceeds buffer bounds (" +
                std::to_string(byte_size()) + " bytes)";
        return false;
    }
    std::memcpy(mapped_, src, bytes);
    return true;
}

bool VulkanBuffer::download(void* dst, std::size_t bytes, std::string& error) {
    if (mapped_ == nullptr) {
        error = "vulkan buffer has no mapping (not initialized?)";
        return false;
    }
    if (dst == nullptr) {
        error = "null destination pointer";
        return false;
    }
    if (bytes == 0 || bytes > byte_size()) {
        error = "download of " + std::to_string(bytes) + " bytes exceeds buffer bounds (" +
                std::to_string(byte_size()) + " bytes)";
        return false;
    }
    std::memcpy(dst, mapped_, bytes);
    return true;
}

VulkanBufferProvider::VulkanBufferProvider(
    VkDevice device, const VkPhysicalDeviceMemoryProperties& memory_properties)
    : device_(device), memory_properties_(memory_properties) {}

std::unique_ptr<IBufferImpl> VulkanBufferProvider::create_buffer(const BufferDesc& desc,
                                                                 std::string& error) {
    if (!available()) {
        error = unavailable_reason();
        return nullptr;
    }
    auto buffer = std::make_unique<VulkanBuffer>(desc, device_, memory_properties_);
    if (!buffer->initialize(error)) {
        return nullptr;  // destructor releases any partially created Vulkan state
    }
    return buffer;
}

}  // namespace vortyx::resource

#endif  // VORTYX_HAS_VULKAN
