#pragma once

// Vulkan compute backend (Phase 3).
//
// Performs vector addition on a real Vulkan device using a compute pipeline
// (no graphics, no windowing, no WSI extensions).
//
// The backend is compiled in when CMake finds the Vulkan loader/headers
// (VORTYX_HAS_VULKAN is defined, see VORTYX_ENABLE_VULKAN in CMakeLists.txt).
// Without Vulkan the same class compiles as a stub that reports itself
// unavailable, so the Runtime and all tests keep working on CPU-only systems.
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
    VectorAddResult execute(const VectorAddTask& task) override;

    // Explicit lifecycle. initialize() performs the full GPU setup
    // (instance -> physical device -> logical device -> queue -> compute
    // pipeline). It returns false (instead of throwing) when Vulkan is not
    // usable on this system, leaving the object in a safe state.
    bool initialize();
    void shutdown();

private:
    struct Impl;            // Vulkan state, defined in vulkan_backend.cpp
    Impl* impl_ = nullptr;  // non-null while Vulkan resources exist
    bool initialized_ = false;
    std::string unavailable_reason_ = "Vulkan backend was not initialized";
};

}  // namespace vortyx::compute
