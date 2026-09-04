// Vulkan compute backend implementation (Phase 4).
//
// Real Vulkan compute path implemented here:
//   instance -> physical device enumeration/selection -> logical device +
//   compute queue -> command pool -> descriptor layout -> compute pipeline
//   (embedded SPIR-V) -> buffer provider (Phase 4) -> dispatch on
//   Resource-Manager-owned buffers -> wait idle.
//
// Phase 4: storage buffers no longer live in this file. They are
// vortyx::resource::VulkanBuffer objects (VkBuffer + VkDeviceMemory, RAII)
// created through this backend's VulkanBufferProvider and tracked by the
// Runtime's Resource Manager. execute() only binds the given buffers to the
// descriptor set and dispatches; upload/download happen in the resource
// layer (Buffer::write / Buffer::read).
//
// Everything is cleaned up in Impl::destroy(), which is idempotent and is
// invoked on any failure path as well as from shutdown(). The provider is
// reset FIRST so no new buffers can appear on a dying device.

#include "core/compute/vulkan_backend.hpp"

#include <string>
#include <vector>

#if defined(VORTYX_HAS_VULKAN)

#include "core/compute/vector_add_spv.hpp"
#include "core/device/vendor_names.hpp"
#include "core/version.hpp"
#include "core/logger.hpp"
#include "core/resource/vulkan_buffer.hpp"

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

    // Phase 4: buffer provider for the Resource Manager. Owns no Vulkan
    // objects itself; the buffers it creates are owned by the resource
    // registry and freed BEFORE this device is destroyed (Runtime::shutdown
    // purges the ResourceManager first).
    std::unique_ptr<vortyx::resource::VulkanBufferProvider> provider;

    std::uint32_t queue_family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};

    void destroy() {  // idempotent, reverse order of creation
        // Stop buffer creation on the dying device before anything else.
        provider.reset();
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
        app.applicationVersion = VK_MAKE_VERSION(VORTYX_VERSION_MAJOR, VORTYX_VERSION_MINOR, VORTYX_VERSION_PATCH);
        app.pEngineName = "VortyxRuntime";
        app.engineVersion = VK_MAKE_VERSION(VORTYX_VERSION_MAJOR, VORTYX_VERSION_MINOR, VORTYX_VERSION_PATCH);
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

        // --- 7. Buffer resource provider (Phase 4) --------------------------
        // Lets the Resource Manager allocate GPU storage (VkBuffer +
        // VkDeviceMemory) on this device. Lives inside the Impl, so it can
        // never outlive the VkDevice.
        impl->provider =
            std::make_unique<vortyx::resource::VulkanBufferProvider>(impl->device,
                                                                    impl->memory_properties);
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
        vortyx::log(vortyx::LogLevel::Info,
                    "Vulkan buffer resource provider registered (Phase 4 resource layer)");
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

vortyx::resource::IBufferProvider* VulkanBackend::resource_provider() {
    if (impl_ == nullptr || !initialized_ || impl_->provider == nullptr) return nullptr;
    return impl_->provider.get();
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

ComputeResult VulkanBackend::execute(const vortyx::resource::IBufferImpl& a,
                                     const vortyx::resource::IBufferImpl& b,
                                     vortyx::resource::IBufferImpl& c) {
    if (impl_ == nullptr || !initialized_) {
        return ComputeResult{Status::NotInitialized,
                             "Vulkan backend is not initialized (call Runtime::initialize first)"};
    }
    Impl* impl = impl_;

    // The buffers must belong to the vulkan backend. Anything else means a
    // routing bug; reject it instead of binding foreign memory.
    const vortyx::resource::VulkanBuffer* vk_a =
        dynamic_cast<const vortyx::resource::VulkanBuffer*>(&a);
    const vortyx::resource::VulkanBuffer* vk_b =
        dynamic_cast<const vortyx::resource::VulkanBuffer*>(&b);
    vortyx::resource::VulkanBuffer* vk_c = dynamic_cast<vortyx::resource::VulkanBuffer*>(&c);
    if (vk_a == nullptr || vk_b == nullptr || vk_c == nullptr) {
        return ComputeResult{Status::BackendError,
                             "buffer does not belong to the vulkan backend"};
    }

    // The buffers must have been allocated on THIS backend's device (Phase 4
    // guards against mixing devices; multi-GPU aggregation is not a Phase 4
    // feature).
    if (vk_a->vk_device() != impl->device || vk_b->vk_device() != impl->device ||
        vk_c->vk_device() != impl->device) {
        return ComputeResult{Status::BackendError,
                             "buffer was not allocated on this backend's Vulkan device"};
    }

    // Shared task rules: int32 elements, equal non-zero counts, Read inputs,
    // Write output. Enforced here so direct backend users cannot bypass it.
    std::string validation_error;
    const Status validation =
        validate_vector_add_buffers(a.desc(), b.desc(), c.desc(), validation_error);
    if (validation != Status::Ok) {
        return ComputeResult{validation, validation_error};
    }

    const std::uint32_t count_u32 = static_cast<std::uint32_t>(a.desc().element_count);

    // Bind the resource-owned buffers to the descriptor set. The set itself
    // is the same fixed 3-binding set as Phase 3; only the bindings' targets
    // (Resource-Manager-owned storage) change per execution.
    const VkDescriptorBufferInfo buffer_infos[3] = {
        {vk_a->vk_buffer(), 0, VK_WHOLE_SIZE},
        {vk_b->vk_buffer(), 0, VK_WHOLE_SIZE},
        {vk_c->vk_buffer(), 0, VK_WHOLE_SIZE},
    };

    VkWriteDescriptorSet writes[3] = {};
    for (std::uint32_t binding = 0; binding < 3; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = impl->descriptor_set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &buffer_infos[binding];
    }
    vkUpdateDescriptorSets(impl->device, 3, writes, 0, nullptr);

    // Record + submit the compute dispatch (synchronous execution).
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult vk = vkResetCommandBuffer(impl->command_buffer, 0);
    if (vk != VK_SUCCESS) {
        return ComputeResult{Status::BackendError, "Vulkan: vkResetCommandBuffer failed"};
    }
    vk = vkBeginCommandBuffer(impl->command_buffer, &begin_info);
    if (vk != VK_SUCCESS) {
        return ComputeResult{Status::BackendError, "Vulkan: vkBeginCommandBuffer failed"};
    }

    vkCmdBindPipeline(impl->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl->pipeline);
    vkCmdBindDescriptorSets(impl->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            impl->pipeline_layout, 0, 1, &impl->descriptor_set, 0, nullptr);

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
        return ComputeResult{Status::BackendError, "Vulkan: vkQueueSubmit failed"};
    }
    vk = vkQueueWaitIdle(impl->queue);
    if (vk != VK_SUCCESS) {
        return ComputeResult{Status::BackendError, "Vulkan: vkQueueWaitIdle failed"};
    }

    // The result now lives in the output buffer's device memory; the caller
    // downloads it explicitly through Buffer::read (Phase 4 separates compute
    // from data movement).
    return ComputeResult{Status::Ok, {}};
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

vortyx::resource::IBufferProvider* VulkanBackend::resource_provider() {
    return nullptr;  // stub build offers no GPU buffer provider
}

vortyx::device::DeviceInfo VulkanBackend::device_info() const {
    vortyx::device::DeviceInfo info;
    info.backend = "vulkan";
    return info;
}

ComputeResult VulkanBackend::execute(const vortyx::resource::IBufferImpl& a,
                                     const vortyx::resource::IBufferImpl& b,
                                     vortyx::resource::IBufferImpl& c) {
    (void)a;
    (void)b;
    (void)c;
    return ComputeResult{Status::BackendUnavailable, unavailable_reason_};
}

#endif  // VORTYX_HAS_VULKAN

}  // namespace vortyx::compute
