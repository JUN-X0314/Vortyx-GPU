// Windows platform backend: CPU discovery via Win32 + CPUID,
// GPU discovery via DXGI adapter enumeration.
// This file compiles only on Windows (_WIN32).

#if defined(_WIN32)

#include "core/device/platform_discovery.hpp"
#include "core/device/vendor_names.hpp"

#include <windows.h>
#include <dxgi.h>

#include <intrin.h>  // __cpuid

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace vortyx::device::detail {
namespace {

// Minimal RAII wrapper for COM objects used here (avoids extra dependencies).
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() { return &ptr_; }
    T* operator->() { return ptr_; }
    T* get() { return ptr_; }

    void reset() {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

std::string utf8_from_wide(const wchar_t* wide) {
    if (wide == nullptr) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};  // 0 = error, 1 = empty string only
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string cpuid_vendor_string() {
    int info[4] = {};
    __cpuid(info, 0);
    char vendor[13] = {};
    std::memcpy(vendor + 0, &info[1], 4);  // EBX
    std::memcpy(vendor + 4, &info[3], 4);  // EDX
    std::memcpy(vendor + 8, &info[2], 4);  // ECX
    return std::string(vendor);
}

std::string cpuid_brand_string() {
    int info[4] = {};
    __cpuid(info, 0x80000000u);
    if (static_cast<unsigned int>(info[0]) < 0x80000004u) return {};

    char brand[49] = {};
    __cpuid(reinterpret_cast<int*>(brand + 0), 0x80000002u);
    __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003u);
    __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004u);

    // The CPUID brand string is space-padded to 48 characters.
    std::string result(brand);
    const std::size_t last = result.find_last_not_of(' ');
    if (last == std::string::npos) return {};
    result.erase(last + 1);
    return result;
}

// Counts physical processor cores via GetLogicalProcessorInformation.
// Returns nullopt if the API fails (value not obtainable).
std::optional<std::uint32_t> count_physical_cores() {
    DWORD length = 0;
    GetLogicalProcessorInformation(nullptr, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return std::nullopt;
    }

    const auto count = length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(count);
    if (!GetLogicalProcessorInformation(buffer.data(), &length)) {
        return std::nullopt;
    }

    std::uint32_t cores = 0;
    for (const auto& item : buffer) {
        if (item.Relationship == RelationProcessorCore) {
            ++cores;
        }
    }
    if (cores == 0) return std::nullopt;
    return cores;
}

std::optional<std::uint64_t> total_system_ram() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return std::nullopt;
    return status.ullTotalPhys;
}

std::string format_hex(std::uint32_t value, int width) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), width == 8 ? "%08X" : "%04X", value);
    return std::string(buffer);
}

}  // namespace

DiscoveryResult discover_cpus_platform() {
    DiscoveryResult result;
    result.ok = true;

    DeviceInfo cpu;
    cpu.type = DeviceType::Cpu;
    cpu.backend = "win32";

    const std::string brand = cpuid_brand_string();
    if (!brand.empty()) cpu.name = brand;

    const std::string vendor = cpu_vendor_name(cpuid_vendor_string());
    if (!vendor.empty()) cpu.vendor = vendor;

    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    if (info.dwNumberOfProcessors > 0) {
        cpu.logical_processors = info.dwNumberOfProcessors;
    }

    cpu.physical_cores = count_physical_cores();
    cpu.memory_bytes = total_system_ram();

    result.devices.push_back(std::move(cpu));
    return result;
}

DiscoveryResult discover_gpus_platform() {
    DiscoveryResult result;

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put()));
    if (FAILED(hr)) {
        result.ok = false;
        char msg[96] = {};
        std::snprintf(msg, sizeof(msg), "CreateDXGIFactory1 failed (HRESULT 0x%08lX)",
                      static_cast<unsigned long>(hr));
        result.error = msg;
        return result;
    }
    result.ok = true;

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enum_hr = factory->EnumAdapters1(index, adapter.put());
        if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_hr)) break;

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;

        DeviceInfo gpu;
        gpu.type = DeviceType::Gpu;
        gpu.backend = "dxgi";

        gpu.name = utf8_from_wide(desc.Description);

        const std::string vendor = pci_vendor_name(desc.VendorId);
        if (!vendor.empty()) {
            gpu.vendor = vendor;
        } else {
            // Real, verifiable information: the raw PCI vendor ID itself.
            gpu.vendor = "Unknown (0x" + format_hex(desc.VendorId, 4) + ")";
        }

        char luid[64] = {};
        std::snprintf(luid, sizeof(luid), "luid-%08lX-%04X",
                      static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                      static_cast<unsigned int>(desc.AdapterLuid.LowPart & 0xFFFFu));
        gpu.id = luid;

        gpu.memory_bytes = desc.DedicatedVideoMemory;
        gpu.shared_memory_bytes = desc.SharedSystemMemory;

        result.devices.push_back(std::move(gpu));
    }

    return result;
}

}  // namespace vortyx::device::detail

#endif  // _WIN32
