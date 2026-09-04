#pragma once

// Internal header: shared PCI vendor-id to vendor-name mapping.
// Contains only well-known PCI vendor IDs (plus software-implementation
// pseudo-IDs such as Mesa's 0x10005, which does not fit in 16 bits);
// unknown IDs are reported as an empty string so callers can display
// "unknown" instead of a guess.

#include <cstdint>
#include <string>

namespace vortyx::device::detail {

inline std::string pci_vendor_name(std::uint32_t vendor_id) {
    switch (vendor_id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        case 0x13B5: return "ARM";
        case 0x5143: return "Qualcomm";
        case 0x1010: return "Imagination Technologies";
        case 0x15AD: return "VMware";
        case 0x1AF4: return "Red Hat (virtio)";
        case 0x1B36: return "QEMU";
        case 0x1013: return "Cirrus Logic";
        case 0x1234: return "Bochs/QEMU (VGA)";
        case 0x10005: return "Mesa";  // software implementations (lavapipe/llvmpipe)
        default: return "";
    }
}

// Maps the x86 CPUID vendor string (from /proc/cpuinfo "vendor_id" or
// __cpuid leaf 0) to a friendly name. Unknown strings are returned as-is.
inline std::string cpu_vendor_name(const std::string& cpuid_vendor) {
    if (cpuid_vendor == "GenuineIntel") return "Intel";
    if (cpuid_vendor == "AuthenticAMD") return "AMD";
    return cpuid_vendor;
}

}  // namespace vortyx::device::detail
