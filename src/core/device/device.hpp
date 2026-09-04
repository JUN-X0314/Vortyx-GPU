#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vortyx::device {

// Kind of computing device. Extensible for future device kinds
// (network workers, Vortyx hardware, ...) without changing existing values.
enum class DeviceType {
    Unknown,
    Cpu,
    Gpu,
};

// Representation of a single computing device found on the system.
//
// Fields that a given platform/API cannot reliably provide are left empty
// (strings) or nullopt (numbers). No value is ever fabricated.
struct DeviceInfo {
    DeviceType type = DeviceType::Unknown;

    // Product/adapter name (e.g. "AMD Ryzen 9 7950X", "NVIDIA GeForce RTX 4090").
    // Empty string means "not obtainable on this platform".
    std::string name;

    // Vendor name (e.g. "NVIDIA", "AMD", "Intel"). Empty means unknown name.
    std::string vendor;

    // Backend-specific stable identifier (e.g. PCI address, DXGI LUID).
    // Empty means not obtainable.
    std::string id;

    // Discovery backend that produced this entry (e.g. "dxgi", "win32",
    // "linux-procfs", "linux-sysfs", "generic").
    std::string backend;

    // CPU fields (nullopt for non-CPU devices or when not obtainable).
    std::optional<std::uint32_t> logical_processors;
    std::optional<std::uint32_t> physical_cores;

    // Memory in bytes.
    // CPU: total system RAM. GPU: dedicated video memory.
    std::optional<std::uint64_t> memory_bytes;

    // GPU only: shared/system memory reported by the backend.
    std::optional<std::uint64_t> shared_memory_bytes;
};

// Human-readable one-line description of a device, e.g.:
//   "CPU: Intel(R) Xeon(R) Processor | vendor: Intel | 2 logical processors | 2 physical cores | RAM 4.1 GiB"
//   "GPU: NVIDIA GeForce RTX 4090 | vendor: NVIDIA | VRAM 24.0 GiB | shared 8.0 GiB | id: ..."
// Values that are not obtainable are shown as "unknown".
std::string describe(const DeviceInfo& device);

}  // namespace vortyx::device
