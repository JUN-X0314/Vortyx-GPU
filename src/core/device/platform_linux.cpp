// Linux platform backend: CPU discovery via /proc/cpuinfo + /proc/meminfo,
// GPU discovery via sysfs PCI device scan (display controller classes).
// This file compiles only on Linux (__linux__).

#if defined(__linux__)

#include "core/device/platform_discovery.hpp"
#include "core/device/vendor_names.hpp"

#include <thread>

#include <cstdio>
#include <cstdlib>  // strtoul
#include <fstream>
#include <optional>
#include <set>
#include <sstream>  // istringstream
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

namespace vortyx::device::detail {
namespace {

std::string trim(const std::string& text) {
    const std::string whitespace = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(whitespace);
    if (begin == std::string::npos) return {};
    const std::size_t end = text.find_last_not_of(whitespace);
    return text.substr(begin, end - begin + 1);
}

// Reads "key : value" style entries of /proc-like files.
// Returns true and fills 'value' when the key is found.
bool read_proc_value(const std::string& line, const std::string& key, std::string& value) {
    if (line.rfind(key, 0) != 0) return false;
    const std::size_t sep = line.find(':');
    if (sep == std::string::npos) return false;
    value = trim(line.substr(sep + 1));
    return true;
}

// Reads a hex value like "0x030000" from a sysfs attribute file.
bool read_sysfs_hex(const std::filesystem::path& path, unsigned long& value) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string text;
    std::getline(file, text);
    text = trim(text);
    if (text.empty()) return false;
    value = std::strtoul(text.c_str(), nullptr, 0);  // base 0 handles "0x" prefix
    return true;
}

std::string format_hex16(unsigned long value) {
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%04lX", value);
    return std::string(buffer);
}

std::optional<std::uint64_t> total_system_ram() {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) return std::nullopt;
    std::string line;
    while (std::getline(file, line)) {
        std::string value;
        if (read_proc_value(line, "MemTotal", value)) {
            // Format: "MemTotal:  123456 kB"
            std::istringstream stream(value);
            long long kb = 0;
            std::string unit;
            stream >> kb >> unit;
            if (kb > 0) return static_cast<std::uint64_t>(kb) * 1024ull;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace

DiscoveryResult discover_cpus_platform() {
    DiscoveryResult result;
    result.ok = true;

    DeviceInfo cpu;
    cpu.type = DeviceType::Cpu;
    cpu.backend = "linux-procfs";

    // /proc/cpuinfo parsing. A CPU entry always exists (this code is running
    // on the CPU), but individual attributes may be missing on some
    // architectures or in restricted environments; missing fields stay
    // unset instead of being filled with guesses.
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::uint32_t logical = 0;
    std::string vendor_raw;
    std::string model_name;
    bool have_physical_id = false;
    std::string current_physical_id;
    std::set<std::pair<std::string, std::string>> unique_cores;

    if (cpuinfo.is_open()) {
        std::string line;
        while (std::getline(cpuinfo, line)) {
            std::string value;
            if (read_proc_value(line, "processor", value)) {
                ++logical;
                have_physical_id = false;
                current_physical_id.clear();
            } else if (read_proc_value(line, "vendor_id", value)) {
                vendor_raw = value;
            } else if (read_proc_value(line, "model name", value)) {
                model_name = value;
            } else if (read_proc_value(line, "physical id", value)) {
                current_physical_id = value;
                have_physical_id = true;
            } else if (read_proc_value(line, "core id", value)) {
                if (have_physical_id) {
                    unique_cores.emplace(current_physical_id, value);
                }
            }
        }
    }

    if (logical == 0) {
        // /proc/cpuinfo unavailable or unparsable; fall back to the standard
        // C++ runtime value where obtainable.
        const unsigned int hardware_concurrency = std::thread::hardware_concurrency();
        if (hardware_concurrency > 0) logical = hardware_concurrency;
    }

    if (!model_name.empty()) cpu.name = model_name;
    if (!vendor_raw.empty()) cpu.vendor = cpu_vendor_name(vendor_raw);
    if (logical > 0) cpu.logical_processors = logical;
    if (!unique_cores.empty()) {
        cpu.physical_cores = static_cast<std::uint32_t>(unique_cores.size());
    }
    cpu.memory_bytes = total_system_ram();

    result.devices.push_back(std::move(cpu));
    return result;
}

DiscoveryResult discover_gpus_platform() {
    DiscoveryResult result;

    const std::filesystem::path pci_base = "/sys/bus/pci/devices";
    std::error_code ec;
    if (!std::filesystem::exists(pci_base, ec)) {
        result.ok = false;
        result.error = "PCI device tree not available at /sys/bus/pci/devices";
        return result;
    }

    result.ok = true;

    for (const auto& entry : std::filesystem::directory_iterator(pci_base, ec)) {
        unsigned long device_class = 0;
        if (!read_sysfs_hex(entry.path() / "class", device_class)) continue;

        // Top byte 0x03 = display controller class
        // (0x030000 VGA, 0x030200 3D controller, 0x038000 display other, ...).
        if ((device_class >> 16) != 0x03) continue;

        unsigned long vendor_id = 0;
        unsigned long device_id = 0;
        read_sysfs_hex(entry.path() / "vendor", vendor_id);
        read_sysfs_hex(entry.path() / "device", device_id);

        DeviceInfo gpu;
        gpu.type = DeviceType::Gpu;
        gpu.backend = "linux-sysfs";

        const std::string vendor = pci_vendor_name(static_cast<std::uint16_t>(vendor_id));
        if (!vendor.empty()) {
            gpu.vendor = vendor;
        } else {
            gpu.vendor = "Unknown (0x" + format_hex16(vendor_id) + ")";
        }

        // Product name is not exposed by sysfs without a pci.ids database;
        // it stays "unknown" instead of being guessed.
        gpu.id = entry.path().filename().string() + " [" +
                 format_hex16(vendor_id) + ":" + format_hex16(device_id) + "]";

        result.devices.push_back(std::move(gpu));
    }

    return result;
}

}  // namespace vortyx::device::detail

#endif  // __linux__
