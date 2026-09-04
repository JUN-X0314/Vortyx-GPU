#pragma once

#include <string>
#include <vector>

#include "core/device/device.hpp"

namespace vortyx::device {

// Outcome of one platform discovery pass.
//
// ok == false : the discovery mechanism itself failed or is unavailable
//               (e.g. a required GPU API could not be initialized).
//               'error' holds a human-readable reason.
// ok == true  : the mechanism ran successfully. An empty 'devices' vector
//               then simply means "no matching device found" (which is a
//               normal result, e.g. a machine without any GPU).
struct DiscoveryResult {
    bool ok = false;
    std::string error;
    std::vector<DeviceInfo> devices;
};

// Discovers CPU devices. On most systems this returns a single CPU entry
// describing the processor; platforms that expose multiple sockets may
// return more. Never throws.
DiscoveryResult discover_cpus();

// Discovers GPU devices. Never throws.
//   ok == true,  empty devices : no GPU found (normal, e.g. GPU-less systems)
//   ok == false, error message : GPU discovery backend unavailable/failed
// Discovery succeeding does NOT imply GPU compute capability. GPU compute
// is a separate feature planned for a later phase.
DiscoveryResult discover_gpus();

// Discovers all devices (CPUs and GPUs) and returns them as one collection.
// If GPU discovery fails or is unavailable, the reason is logged (vortyx::log)
// and CPU discovery still runs, so the program keeps working on GPU-less or
// API-less systems. Never throws.
std::vector<DeviceInfo> discover_devices();

}  // namespace vortyx::device
