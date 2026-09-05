// Phase 12 configuration implementation (Phase 12).

#include "distributed/config.hpp"

#include <cstdlib>
#include <string>

namespace vortyx::distributed {

namespace {

// Reads one variable. absent -> false; present (even empty) -> true.
bool read_env(const char* name, std::string& value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr) return false;
    value = raw;
    return true;
}

// Parses a non-negative integer. Returns false on garbage, negatives or
// overflow (a wrong value is a configuration error, never a silent 0).
bool parse_nonnegative(const std::string& value, std::int64_t& out) {
    if (value.empty()) return false;
    std::int64_t parsed = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return false;
        if (parsed > (INT64_MAX - (c - '0')) / 10) return false;
        parsed = parsed * 10 + (c - '0');
    }
    out = parsed;
    return true;
}

bool parse_bool(const std::string& value, bool& out) {
    if (value == "1" || value == "true" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace

vortyx::platform::Status DistributedConfig::validate(std::string& error) const {
    bool policy_known = false;
    for (const std::string& name : known_scheduling_policies()) {
        if (name == scheduler_policy) policy_known = true;
    }
    if (!policy_known) {
        error = "unknown scheduler policy '" + scheduler_policy +
                "' (known: round_robin, least_loaded, capability_fit)";
        return vortyx::platform::Status::InvalidInput;
    }
    if (heartbeat_timeout_ms <= 0) {
        error = "heartbeat_timeout_ms must be positive";
        return vortyx::platform::Status::InvalidInput;
    }
    if (retry_backoff_ms < 0) {
        error = "retry_backoff_ms must not be negative";
        return vortyx::platform::Status::InvalidInput;
    }
    if (lease_ttl_ms <= 0) {
        error = "lease_ttl_ms must be positive";
        return vortyx::platform::Status::InvalidInput;
    }
    if (max_retries > 1000) {
        error = "max_retries is unreasonably large (bounded by design)";
        return vortyx::platform::Status::InvalidInput;
    }
    error.clear();
    return vortyx::platform::Status::Ok;
}

bool distributed_config_from_environment(DistributedConfig& out, std::string& error) {
    std::string value;

    if (read_env("VORTYX_DISTRIBUTED_ENABLED", value)) {
        if (!parse_bool(value, out.enabled)) {
            error = "VORTYX_DISTRIBUTED_ENABLED must be 1/0/true/false/on/off (got '" + value +
                    "')";
            return false;
        }
    }
    if (read_env("VORTYX_SCHEDULER_POLICY", value)) {
        out.scheduler_policy = value;  // validated by validate()
    }
    if (read_env("VORTYX_MAX_DEVICES", value)) {
        std::int64_t parsed = 0;
        if (!parse_nonnegative(value, parsed)) {
            error = "VORTYX_MAX_DEVICES must be a non-negative integer (got '" + value + "')";
            return false;
        }
        out.max_devices = static_cast<std::uint32_t>(parsed);
    }
    if (read_env("VORTYX_HEARTBEAT_TIMEOUT_MS", value)) {
        std::int64_t parsed = 0;
        if (!parse_nonnegative(value, parsed)) {
            error = "VORTYX_HEARTBEAT_TIMEOUT_MS must be a non-negative integer (got '" + value +
                    "')";
            return false;
        }
        out.heartbeat_timeout_ms = parsed;
    }
    if (read_env("VORTYX_MAX_RETRIES", value)) {
        std::int64_t parsed = 0;
        if (!parse_nonnegative(value, parsed)) {
            error = "VORTYX_MAX_RETRIES must be a non-negative integer (got '" + value + "')";
            return false;
        }
        out.max_retries = static_cast<std::uint32_t>(parsed);
    }
    if (read_env("VORTYX_RETRY_BACKOFF_MS", value)) {
        std::int64_t parsed = 0;
        if (!parse_nonnegative(value, parsed)) {
            error = "VORTYX_RETRY_BACKOFF_MS must be a non-negative integer (got '" + value +
                    "')";
            return false;
        }
        out.retry_backoff_ms = parsed;
    }
    if (read_env("VORTYX_LEASE_TTL_MS", value)) {
        std::int64_t parsed = 0;
        if (!parse_nonnegative(value, parsed)) {
            error = "VORTYX_LEASE_TTL_MS must be a non-negative integer (got '" + value + "')";
            return false;
        }
        out.lease_ttl_ms = parsed;
    }
    if (read_env("VORTYX_ALLOW_SINGLE_DEVICE_FALLBACK", value)) {
        if (!parse_bool(value, out.allow_single_device_fallback)) {
            error = "VORTYX_ALLOW_SINGLE_DEVICE_FALLBACK must be 1/0/true/false/on/off (got '" +
                    value + "')";
            return false;
        }
    }
    if (read_env("VORTYX_MAX_SHARDS_PER_JOB", value)) {
        std::int64_t parsed = 0;
        if (!parse_nonnegative(value, parsed)) {
            error = "VORTYX_MAX_SHARDS_PER_JOB must be a non-negative integer (got '" + value +
                    "')";
            return false;
        }
        out.max_shards_per_job = static_cast<std::uint32_t>(parsed);
    }
    if (read_env("VORTYX_SHARD_THREADS", value)) {
        std::int64_t parsed = 0;
        if (!parse_nonnegative(value, parsed)) {
            error = "VORTYX_SHARD_THREADS must be a non-negative integer (got '" + value + "')";
            return false;
        }
        out.shard_threads = static_cast<std::uint32_t>(parsed);
    }

    // The parsed configuration must itself be valid (e.g. the policy name).
    return out.validate(error) == vortyx::platform::Status::Ok;
}

const std::vector<std::string>& distributed_config_variables() {
    static const std::vector<std::string> kVariables = {
        "VORTYX_DISTRIBUTED_ENABLED",
        "VORTYX_SCHEDULER_POLICY",
        "VORTYX_MAX_DEVICES",
        "VORTYX_HEARTBEAT_TIMEOUT_MS",
        "VORTYX_MAX_RETRIES",
        "VORTYX_RETRY_BACKOFF_MS",
        "VORTYX_LEASE_TTL_MS",
        "VORTYX_ALLOW_SINGLE_DEVICE_FALLBACK",
        "VORTYX_MAX_SHARDS_PER_JOB",
        "VORTYX_SHARD_THREADS",
    };
    return kVariables;
}

}  // namespace vortyx::distributed
