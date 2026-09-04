#pragma once

// Internal header: platform backend contract.
// Exactly one of platform_windows.cpp / platform_linux.cpp /
// platform_fallback.cpp compiles and provides these two functions.

#include "core/device/discovery.hpp"

namespace vortyx::device::detail {

DiscoveryResult discover_cpus_platform();
DiscoveryResult discover_gpus_platform();

}  // namespace vortyx::device::detail
