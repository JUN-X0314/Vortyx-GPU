#pragma once

// Vulkan compute backend (Phase 4).
//
// Performs vector addition on a real Vulkan device using a compute pipeline
// (no graphics, no windowing, no WSI extensions).
//
// The backend is compiled in when CMake finds the Vulkan loader/headers
// (VORTYX_HAS_VULKAN is defined, see VORTYX_ENABLE_VULKAN in CMakeLists.txt).
// Without Vulkan the same class compiles as a stub that reports itself
// unavailable, so the Runtime and all tests keep working on CPU-only systems.
//
// Phase 4 change — buffer resources:
//   - Storage buffers are no longer created per execute() call inside this
//     backend. GPU storage is allocated by the resource layer:
//     vortyx::resource::VulkanBuffer (VkBuffer + VkDeviceMemory, RAII) is
//     created through this backend's VulkanBufferProvider and tracked by the
//     Runtime's Resource Manager. execute() binds whatever buffers it is
//     given to the descriptor set and dispatches — nothing more.
//   - The backend exposes its provider via resource_provider() once
//     initialized; the provider lives inside the backend's Impl so it can
//     never outlive the VkDevice.
//   - The Phase 3 task-based execute() was removed from the backend
//     interface (task->buffer translation now lives once in the Runtime).
//     The public Runtime::execute(VectorAddTask) API is unchanged.
//
// SPIR-V for the kernel is pre-compiled and embedded
// (src/core/compute/vector_add_spv.hpp); no shader compilation or download
// happens at runtime.
//
// All Vulkan state lives in an opaque Impl (PIMPL) owned by this class and
// destroyed by shutdown(), so GPU resources can never leak past the
// backend's lifetime, including when initialize() fails midway.

#include <string>

#include "core/compute/backend.hpp"

namespace vortyx::compute {

class VulkanBackend final : public IComputeBackend {
public:
    VulkanBackend() = default;
    ~VulkanBackend() override;

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    const char* name() const override { return "vulkan"; }
    bool available() const override { return initialized_; }
    std::string unavailable_reason() const override { return unavailable_reason_; }

    vortyx::device::DeviceInfo device_info() const override;

    // Buffer-based execution: a, b, c must be VulkanBuffer resources allocated
    // on THIS backend's device. Binds them to the descriptor set and dispatches
    // the compute kernel; results are left inside c (the caller downloads with
    // c's read()).
    ComputeResult execute(const vortyx::resource::IBufferImpl& a,
                          const vortyx::resource::IBufferImpl& b,
                          vortyx::resource::IBufferImpl& c) override;

    // The backend's buffer provider (allocates real VkBuffer + VkDeviceMemory).
    // Non-null only while the backend is initialized.
    vortyx::resource::IBufferProvider* resource_provider() override;

    // Explicit lifecycle. initialize() performs the full GPU setup
    // (instance -> physical device -> logical device -> queue -> compute
    // pipeline -> buffer provider). It returns false (instead of throwing)
    // when Vulkan is not usable on this system, leaving the object in a
    // safe state.
    bool initialize();
    void shutdown();

private:
    struct Impl;            // Vulkan state, defined in vulkan_backend.cpp
    Impl* impl_ = nullptr;  // non-null while Vulkan resources exist
    bool initialized_ = false;
    std::string unavailable_reason_ = "Vulkan backend was not initialized";
};

}  // namespace vortyx::compute
