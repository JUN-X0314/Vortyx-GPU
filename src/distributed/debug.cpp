// Debug / diagnostic text rendering implementation (Phase 12).

#include "distributed/debug.hpp"

namespace vortyx::distributed {

std::string to_debug_string(const ClusterSnapshot& snapshot) {
    std::string out = "cluster revision " + std::to_string(snapshot.revision) + ", " +
                      std::to_string(snapshot.devices.size()) + " device(s)";
    for (const DeviceSnapshot& device : snapshot.devices) {
        out += "\n  device '" + device.device_id + "'";
        out += " state=" + std::string(to_string(device.state));
        out += " health=" + std::string(to_string(device.health));
        out += " capacity[" + to_string(device.capabilities.capacity) + "]";
        out += " allocated[" + to_string(device.allocated) + "]";
        out += " backends=[";
        for (std::size_t i = 0; i < device.capabilities.metadata.backends.size(); ++i) {
            if (i > 0) out += ",";
            out += device.capabilities.metadata.backends[i];
        }
        out += "]";
    }
    return out;
}

std::string to_debug_string(const DistributedJobRecord& job) {
    std::string out = "job '" + job.job_id + "'";
    out += " status=" + std::string(to_string(job.status));
    out += " op=" + std::string(vortyx::compute::workload_label(job.operation));
    out += " elements=" + std::to_string(job.element_count);
    out += " shards=" + std::to_string(job.shards.size());
    if (!job.error.empty()) out += " error='" + job.error + "'";
    if (!job.platform_error.empty()) out += " platform_error='" + job.platform_error + "'";
    for (const JobShard& shard : job.shards) {
        out += "\n  shard " + shard.shard_id;
        out += " state=" + std::string(to_string(shard.state));
        out += " range=[" + std::to_string(shard.work.element_range.begin) + "," +
               std::to_string(shard.work.element_range.end) + ")";
        out += " device=" + (shard.assigned_device.empty() ? std::string("-") : shard.assigned_device);
        out += " attempt=" + std::to_string(shard.attempt);
        if (!shard.last_failure_code.empty()) {
            out += " failure=" + shard.last_failure_code;
        }
        if (!shard.last_error.empty()) {
            out += " error='" + shard.last_error + "'";
        }
    }
    return out;
}

}  // namespace vortyx::distributed
