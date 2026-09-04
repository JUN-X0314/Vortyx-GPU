// Vulkan compute backend implementation (Phase 3).
//
// Real Vulkan compute path implemented here:
//   instance -> physical device enumeration/selection -> logical device +
//   compute queue -> command pool -> descriptor layout -> compute pipeline
//   (embedded SPIR-V) -> per-execution storage buffers -> dispatch -> readback.
//
// Everything is cleaned up in Impl::destroy(), which is idempotent and is
// invoked on any failure path as well as from shutdown().

#include "core/compute/vulkan_backend.hpp"

#include <string>
#include <vector>

#if defined(VORTYX_HAS_VULKAN)

#include "core/compute/vector_add_spv.hpp"
#include "core/device/vendor_names.hpp"
#include "core/logger.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#endif  // VORTYX_HAS_VULKAN

namespace vortyx::compute {

#if defined(VORTYX_HAS_VULKAN)

namespace {

constexpr std::uint32_t kWorkgroupSize = 64;  // must match shaders/vector_add.comp

std::string vk_result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        default: return "VkResult " + std::to_string(static_cast<int>(result));
    }
}

std::string device_type_name(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software/CPU implementation";
        default: return "other";
    }
}

// Selection preference: real GPUs first, software implementations last.
int device_type_score(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
    }
}

std::string format_hex16(unsigned int value) {
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%04X", value);
    return std::string(buffer);
}

}  // namespace

struct VulkanBackend::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;  // freed with the pool

    std::uint32_t queue_family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};

    void destroy() {  // idempotent, reverse order of creation
        if (device != VK_NULL_HANDLE) {
            descriptor_set = VK_NULL_HANDLE;
            if (descriptor_pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
                descriptor_pool = VK_NULL_HANDLE;
            }
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
            if (pipeline_layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
                pipeline_layout = VK_NULL_HANDLE;
            }
            if (descriptor_set_layout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
                descriptor_set_layout = VK_NULL_HANDLE;
            }
            command_buffer = VK_NULL_HANDLE;  // freed with the pool
            if (command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, command_pool, nullptr);
                command_pool = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        physical_device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
    }
};

VulkanBackend::~VulkanBackend() {
    shutdown();
}

bool VulkanBackend::initialize() {
    if (initialized_) return true;

    unavailable_reason_ = "Vulkan backend was not initialized";
    Impl* impl = new Impl();
    impl_ = impl;

    std::string fail;        // step description of the first failure
    std::string fail_detail; // low-level reason (VkResult) if any
    bool ok = true;

    do {  // do-while(false): single linear flow with break on failure
        // --- 1. Instance --------------------------------------------------
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Vortyx";
        app.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
        app.pEngineName = "VortyxRuntime";
        app.engineVersion = VK_MAKE_VERSION(0, 3, 0);
        app.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &app;
        // No instance extensions/layers required: compute-only, no WSI.

        VkResult vk = vkCreateInstance(&instance_info, nullptr, &impl->instance);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreateInstance failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        // --- 2. Physical device enumeration and selection ------------------
        std::uint32_t device_count = 0;
        vk = vkEnumeratePhysicalDevices(impl->instance, &device_count, nullptr);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkEnumeratePhysicalDevices failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }
        if (device_count == 0) {
            fail = "Vulkan: no physical devices found";
            fail_detail = "no Vulkan driver (or software Vulkan implementation) installed";
            ok = false;
            break;
        }

        std::vector<VkPhysicalDevice> devices(device_count);
        vk = vkEnumeratePhysicalDevices(impl->instance, &device_count, devices.data());
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkEnumeratePhysicalDevices failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        int best_score = -1;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);

            std::uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            if (family_count == 0) continue;

            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

            for (std::uint32_t family = 0; family < family_count; ++family) {
                if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
                const int score = device_type_score(props.deviceType);
                if (score > best_score) {
                    best_score = score;
                    impl->physical_device = candidate;
                    impl->queue_family = family;
                    impl->properties = props;
                }
                break;  // one compute family per device is enough
            }
        }

        if (impl->physical_device == VK_NULL_HANDLE) {
            fail = "Vulkan: no compute-capable physical device found";
            fail_detail = "devices exist but none exposes a compute queue";
            ok = false;
            break;
        }

        vkGetPhysicalDeviceMemoryProperties(impl->physical_device, &impl->memory_properties);

        // --- 3. Logical device + compute queue ------------------------------
        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = impl->queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        // No device extensions/features needed for storage-buffer compute.

        vk = vkCreateDevice(impl->physical_device, &device_info, nullptr, &impl->device);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreateDevice failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        vkGetDeviceQueue(impl->device, impl->queue_family, 0, &impl->queue);
        if (impl->queue == VK_NULL_HANDLE) {
            fail = "Vulkan: failed to obtain compute queue";
            ok = false;
            break;
        }

        // --- 4. Command pool + command buffer -------------------------------
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = impl->queue_family;

        vk = vkCreateCommandPool(impl->device, &pool_info, nullptr, &impl->command_pool);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreateCommandPool failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        VkCommandBufferAllocateInfo buffer_alloc{};
        buffer_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        buffer_alloc.commandPool = impl->command_pool;
        buffer_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        buffer_alloc.commandBufferCount = 1;

        vk = vkAllocateCommandBuffers(impl->device, &buffer_alloc, &impl->command_buffer);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkAllocateCommandBuffers failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        // --- 5. Descriptor layout, pipeline layout, compute pipeline --------
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (std::uint32_t b = 0; b < 3; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 3;
        layout_info.pBindings = bindings;

        vk = vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr,
                                         &impl->descriptor_set_layout);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreateDescriptorSetLayout failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(std::uint32_t);  // element count

        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &impl->descriptor_set_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;

        vk = vkCreatePipelineLayout(impl->device, &pipeline_layout_info, nullptr,
                                    &impl->pipeline_layout);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreatePipelineLayout failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = detail::kVectorAdd_spv_size;
        shader_info.pCode = reinterpret_cast<const std::uint32_t*>(detail::kVectorAdd_spv);

        VkShaderModule shader_module = VK_NULL_HANDLE;
        vk = vkCreateShaderModule(impl->device, &shader_info, nullptr, &shader_module);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreateShaderModule failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader_module;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage;
        pipeline_info.layout = impl->pipeline_layout;

        vk = vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &pipeline_info,
                                      nullptr, &impl->pipeline);
        vkDestroyShaderModule(impl->device, shader_module, nullptr);  // no longer needed
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreateComputePipelines failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        // --- 6. Descriptor pool + one descriptor set ------------------------
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = 3;

        VkDescriptorPoolCreateInfo desc_pool_info{};
        desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        desc_pool_info.maxSets = 1;
        desc_pool_info.poolSizeCount = 1;
        desc_pool_info.pPoolSizes = &pool_size;

        vk = vkCreateDescriptorPool(impl->device, &desc_pool_info, nullptr,
                                    &impl->descriptor_pool);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreateDescriptorPool failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }

        VkDescriptorSetAllocateInfo set_alloc{};
        set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_alloc.descriptorPool = impl->descriptor_pool;
        set_alloc.descriptorSetCount = 1;
        set_alloc.pSetLayouts = &impl->descriptor_set_layout;

        vk = vkAllocateDescriptorSets(impl->device, &set_alloc, &impl->descriptor_set);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkAllocateDescriptorSets failed";
            fail_detail = vk_result_name(vk);
            ok = false;
            break;
        }
    } while (false);

    if (!ok) {
        unavailable_reason_ = fail_detail.empty() ? fail : fail + " (" + fail_detail + ")";
        vortyx::log(vortyx::LogLevel::Warning, "Vulkan backend unavailable: " + unavailable_reason_);
        impl->destroy();
        delete impl;
        impl_ = nullptr;
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    unavailable_reason_.clear();

    {
        const std::uint32_t major = VK_API_VERSION_MAJOR(impl->properties.apiVersion);
        const std::uint32_t minor = VK_API_VERSION_MINOR(impl->properties.apiVersion);
        vortyx::log(vortyx::LogLevel::Info,
                    std::string("Vulkan backend ready: physical device '") +
                        impl->properties.deviceName + "' (" +
                        device_type_name(impl->properties.deviceType) +
                        ", Vulkan API " + std::to_string(major) + "." +
                        std::to_string(minor) + ")");
    }
    return true;
}

void VulkanBackend::shutdown() {
    if (impl_ != nullptr) {
        impl_->destroy();
        delete impl_;
        impl_ = nullptr;
    }
    initialized_ = false;
    unavailable_reason_ = "Vulkan backend was not initialized";
}

vortyx::device::DeviceInfo VulkanBackend::device_info() const {
    vortyx::device::DeviceInfo info;
    info.backend = "vulkan";

    if (impl_ == nullptr || !initialized_) return info;  // unknown device

    const VkPhysicalDeviceProperties& props = impl_->properties;
    info.name = props.deviceName;

    const std::string vendor =
        vortyx::device::detail::pci_vendor_name(props.vendorID);
    info.vendor = vendor.empty() ? "Unknown (0x" + format_hex16(props.vendorID) + ")" : vendor;

    info.id = "vulkan-vendor0x" + format_hex16(props.vendorID) +
              "-device0x" + format_hex16(props.deviceID) +
              "-api" + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
              std::to_string(VK_API_VERSION_MINOR(props.apiVersion));

    // Honest device kind: real GPUs vs software Vulkan implementations
    // (e.g. lavapipe reports VK_PHYSICAL_DEVICE_TYPE_CPU).
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            info.type = vortyx::device::DeviceType::Gpu;
            break;
        default:
            info.type = vortyx::device::DeviceType::SoftwareGpu;
            break;
    }

    // Largest device-local heap reported by the driver.
    for (std::uint32_t heap = 0; heap < impl_->memory_properties.memoryHeapCount; ++heap) {
        const VkMemoryHeap& mem_heap = impl_->memory_properties.memoryHeaps[heap];
        if ((mem_heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
            mem_heap.size > info.memory_bytes.value_or(0)) {
            info.memory_bytes = mem_heap.size;
        }
    }

    return info;
}

VectorAddResult VulkanBackend::execute(const VectorAddTask& task) {
    VectorAddResult result;
    if (impl_ == nullptr || !initialized_) {
        result.status = Status::NotInitialized;
        result.error = "Vulkan backend is not initialized (call Runtime::initialize first)";
        return result;
    }

    const Status validation = validate_vector_add(task);
    if (validation != Status::Ok) {
        result.status = validation;
        result.error = "invalid vector addition task (a.size=" +
                       std::to_string(task.a.size()) + ", b.size=" +
                       std::to_string(task.b.size()) + "); arrays must be non-empty and equal size";
        return result;
    }

    Impl* impl = impl_;
    const std::size_t count = task.a.size();
    const VkDeviceSize byte_size = static_cast<VkDeviceSize>(count) * sizeof(std::int32_t);

    // Per-execution RAII buffer pair: destroyed when the scope ends, on
    // every path (success or error), so GPU memory never leaks.
    struct ScopedBuffer {
        VkDevice device = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        ~ScopedBuffer() {
            if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
        }
    };

    const char* fail = nullptr;
    std::string fail_detail;

    auto create_buffer = [&](ScopedBuffer& out) -> bool {
        out.device = impl->device;  // required by the RAII destructor
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = byte_size;
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult vk = vkCreateBuffer(impl->device, &buffer_info, nullptr, &out.buffer);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkCreateBuffer failed";
            fail_detail = vk_result_name(vk);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(impl->device, out.buffer, &requirements);

        std::int32_t memory_type = -1;
        for (std::uint32_t t = 0; t < impl->memory_properties.memoryTypeCount; ++t) {
            const bool bits_ok = (requirements.memoryTypeBits & (1u << t)) != 0;
            const bool props_ok =
                (impl->memory_properties.memoryTypes[t].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
                0;
            if (bits_ok && props_ok) {
                memory_type = static_cast<std::int32_t>(t);
                break;
            }
        }
        if (memory_type < 0) {
            fail = "Vulkan: no host-visible coherent memory type found for storage buffer";
            return false;
        }

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = requirements.size;
        alloc_info.memoryTypeIndex = static_cast<std::uint32_t>(memory_type);

        vk = vkAllocateMemory(impl->device, &alloc_info, nullptr, &out.memory);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkAllocateMemory failed";
            fail_detail = vk_result_name(vk);
            return false;
        }

        vk = vkBindBufferMemory(impl->device, out.buffer, out.memory, 0);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkBindBufferMemory failed";
            fail_detail = vk_result_name(vk);
            return false;
        }
        return true;
    };

    ScopedBuffer buf_a, buf_b, buf_c;
    if (!create_buffer(buf_a) || !create_buffer(buf_b) || !create_buffer(buf_c)) {
        result.status = Status::BackendError;
        result.error = fail_detail.empty() ? std::string(fail) : std::string(fail) + " (" + fail_detail + ")";
        return result;
    }

    // Upload inputs (host-visible + host-coherent memory: plain memcpy).
    auto upload = [&](const ScopedBuffer& buf, const std::vector<std::int32_t>& data) -> bool {
        void* mapped = nullptr;
        const VkResult vk = vkMapMemory(impl->device, buf.memory, 0, byte_size, 0, &mapped);
        if (vk != VK_SUCCESS) {
            fail = "Vulkan: vkMapMemory (upload) failed";
            fail_detail = vk_result_name(vk);
            return false;
        }
        std::memcpy(mapped, data.data(), static_cast<std::size_t>(byte_size));
        vkUnmapMemory(impl->device, buf.memory);
        return true;
    };

    if (!upload(buf_a, task.a) || !upload(buf_b, task.b)) {
        result.status = Status::BackendError;
        result.error = fail_detail.empty() ? std::string(fail) : std::string(fail) + " (" + fail_detail + ")";
        return result;
    }

    // Bind buffers to the descriptor set.
    VkDescriptorBufferInfo buffer_infos[3] = {
        {buf_a.buffer, 0, VK_WHOLE_SIZE},
        {buf_b.buffer, 0, VK_WHOLE_SIZE},
        {buf_c.buffer, 0, VK_WHOLE_SIZE},
    };

    VkWriteDescriptorSet writes[3] = {};
    for (std::uint32_t b = 0; b < 3; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = impl->descriptor_set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &buffer_infos[b];
    }
    vkUpdateDescriptorSets(impl->device, 3, writes, 0, nullptr);

    // Record + submit the compute dispatch (synchronous execution).
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult vk = vkResetCommandBuffer(impl->command_buffer, 0);
    if (vk != VK_SUCCESS) {
        result.status = Status::BackendError;
        result.error = "Vulkan: vkResetCommandBuffer failed";
        return result;
    }
    vk = vkBeginCommandBuffer(impl->command_buffer, &begin_info);
    if (vk != VK_SUCCESS) {
        result.status = Status::BackendError;
        result.error = "Vulkan: vkBeginCommandBuffer failed";
        return result;
    }

    vkCmdBindPipeline(impl->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl->pipeline);
    vkCmdBindDescriptorSets(impl->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            impl->pipeline_layout, 0, 1, &impl->descriptor_set, 0, nullptr);

    const std::uint32_t count_u32 = static_cast<std::uint32_t>(count);
    vkCmdPushConstants(impl->command_buffer, impl->pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t), &count_u32);

    const std::uint32_t groups = (count_u32 + kWorkgroupSize - 1) / kWorkgroupSize;
    vkCmdDispatch(impl->command_buffer, groups, 1, 1);
    vkEndCommandBuffer(impl->command_buffer);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &impl->command_buffer;

    vk = vkQueueSubmit(impl->queue, 1, &submit_info, VK_NULL_HANDLE);
    if (vk != VK_SUCCESS) {
        result.status = Status::BackendError;
        result.error = "Vulkan: vkQueueSubmit failed";
        return result;
    }
    vk = vkQueueWaitIdle(impl->queue);
    if (vk != VK_SUCCESS) {
        result.status = Status::BackendError;
        result.error = "Vulkan: vkQueueWaitIdle failed";
        return result;
    }

    // Read back C.
    {
        void* mapped = nullptr;
        vk = vkMapMemory(impl->device, buf_c.memory, 0, byte_size, 0, &mapped);
        if (vk != VK_SUCCESS) {
            result.status = Status::BackendError;
            result.error = "Vulkan: vkMapMemory (readback) failed";
            return result;
        }
        result.data.resize(count);
        std::memcpy(result.data.data(), mapped, static_cast<std::size_t>(byte_size));
        vkUnmapMemory(impl->device, buf_c.memory);
    }

    result.status = Status::Ok;
    result.error.clear();
    return result;
}

#else  // !VORTYX_HAS_VULKAN — stub keeps Runtime/tests working without Vulkan

VulkanBackend::~VulkanBackend() = default;

bool VulkanBackend::initialize() {
    unavailable_reason_ =
        "Vulkan support was not compiled into this build "
        "(VORTYX_ENABLE_VULKAN disabled or Vulkan SDK/loader not found)";
    return false;
}

void VulkanBackend::shutdown() {}

vortyx::device::DeviceInfo VulkanBackend::device_info() const {
    vortyx::device::DeviceInfo info;
    info.backend = "vulkan";
    return info;
}

VectorAddResult VulkanBackend::execute(const VectorAddTask& task) {
    (void)task;
    VectorAddResult result;
    result.status = Status::BackendUnavailable;
    result.error = unavailable_reason_;
    return result;
}

#endif  // VORTYX_HAS_VULKAN

}  // namespace vortyx::compute
