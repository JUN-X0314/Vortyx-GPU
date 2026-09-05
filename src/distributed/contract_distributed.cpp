// Distributed API contract codec implementation (Phase 12).
//
// Serialization order is part of the contract (byte-for-byte deterministic,
// pinned by tests on both the C++ and TypeScript sides).

#include "distributed/contract_distributed.hpp"

#include <cmath>

#include "core/version.hpp"
#include "platform/identity.hpp"
#include "platform/json.hpp"
#include "platform/metadata.hpp"

namespace vortyx::distributed::contract {

namespace {

using platform::JsonValue;
using platform::contract::ParseOutcome;
using platform::contract::kErrInvalidEnum;
using platform::contract::kErrInvalidType;
using platform::contract::kErrInvalidValue;
using platform::contract::kErrMissingField;
using platform::contract::kErrUnsupportedProtocol;

ParseOutcome ok() {
    ParseOutcome outcome;
    outcome.status = platform::Status::Ok;
    outcome.http_status_code = 200;
    return outcome;
}

ParseOutcome fail(platform::Status status, const std::string& code, const std::string& message) {
    ParseOutcome outcome;
    outcome.status = status;
    outcome.error_code = code;
    outcome.message = message;
    outcome.http_status_code = platform::contract::http_status(status, code);
    return outcome;
}

const JsonValue* field(const JsonValue& object, const std::string& key) {
    return object.find(key);
}

// Unknown-field rejection (the same strictness as the Phase 11 contract's
// reject_unknown_fields — the distributed surface carries metadata ONLY,
// and a payload smuggled under any name is a schema violation).
ParseOutcome reject_unknown_fields(const JsonValue& object,
                                   const std::vector<std::string>& allowed) {
    for (const auto& member : object.members()) {
        bool known = false;
        for (const std::string& name : allowed) {
            if (member.first == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            return fail(platform::Status::InvalidInput, kErrInvalidValue,
                        "unknown field '" + member.first + "'");
        }
    }
    return ok();
}

ParseOutcome require_string(const JsonValue& object, const std::string& key, std::string& value) {
    const JsonValue* item = field(object, key);
    if (item == nullptr) {
        return fail(platform::Status::InvalidInput, kErrMissingField,
                    "missing required field '" + key + "'");
    }
    if (!item->is_string()) {
        return fail(platform::Status::InvalidInput, kErrInvalidType,
                    "field '" + key + "' must be a string");
    }
    value = item->as_string();
    return ok();
}

ParseOutcome require_number(const JsonValue& object, const std::string& key, double& value) {
    const JsonValue* item = field(object, key);
    if (item == nullptr) {
        return fail(platform::Status::InvalidInput, kErrMissingField,
                    "missing required field '" + key + "'");
    }
    if (!item->is_number()) {
        return fail(platform::Status::InvalidInput, kErrInvalidType,
                    "field '" + key + "' must be a number");
    }
    value = item->as_number();
    return ok();
}

// A parsed JSON number that must be a non-negative integer within the
// documented double-exactness domain.
ParseOutcome integral_number(const JsonValue& item, const std::string& key,
                             std::int64_t& value) {
    const double raw = item.as_number();
    if (raw != std::floor(raw) || std::fabs(raw) >= 9007199254740992.0) {
        return fail(platform::Status::InvalidInput, kErrInvalidValue,
                    "field '" + key + "' must be an integer below 2^53");
    }
    if (raw < 0) {
        return fail(platform::Status::InvalidInput, kErrInvalidValue,
                    "field '" + key + "' must not be negative");
    }
    value = static_cast<std::int64_t>(raw);
    return ok();
}

// Resource vector as a JSON object with the documented field order.
JsonValue serialize_resource_vector(const ResourceVector& vector) {
    JsonValue out = JsonValue::make_object();
    out.add("compute_units", JsonValue::make_number(static_cast<double>(vector.compute_units)));
    out.add("memory_bytes", JsonValue::make_number(static_cast<double>(vector.memory_bytes)));
    out.add("concurrent_jobs", JsonValue::make_number(static_cast<double>(vector.concurrent_jobs)));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// parse_create_distributed_job
// ---------------------------------------------------------------------------

platform::contract::ParseOutcome parse_create_distributed_job(
    const std::string& body, vortyx::platform::JobEnvelope& envelope,
    std::uint32_t& requested_shard_count) {
    platform::JsonValue parsed;
    std::string json_error;
    if (!platform::parse_json(body, parsed, json_error)) {
        return fail(platform::Status::InvalidInput, platform::contract::kErrInvalidJson,
                    "request body is not valid JSON: " + json_error);
    }
    if (!parsed.is_object()) {
        return fail(platform::Status::InvalidInput, platform::contract::kErrInvalidType,
                    "request body must be a JSON object");
    }

    ParseOutcome outcome = reject_unknown_fields(parsed, {
        "job_id", "operation", "element_count", "requested_shard_count",
        "requested_backend", "priority", "protocol_version", "created_at_ms",
    });
    if (!outcome.ok()) return outcome;

    requested_shard_count = 1;
    envelope = vortyx::platform::JobEnvelope{};

    outcome = require_string(parsed, "job_id", envelope.job_id);
    if (!outcome.ok()) return outcome;

    std::string operation;
    outcome = require_string(parsed, "operation", operation);
    if (!outcome.ok()) return outcome;
    // The op vocabulary has ONE source: the Phase 10 workload labels.
    bool known = false;
    for (const std::string& label : platform::known_operations()) {
        if (label == operation) known = true;
    }
    if (!known) {
        return fail(platform::Status::InvalidInput, kErrInvalidEnum,
                    "unknown operation '" + operation + "'");
    }
    if (operation == "vector_add") envelope.operation = vortyx::compute::ComputeOp::VectorAdd;
    else if (operation == "vector_multiply")
        envelope.operation = vortyx::compute::ComputeOp::VectorMultiply;
    else if (operation == "vector_scale")
        envelope.operation = vortyx::compute::ComputeOp::VectorScale;

    double number = 0.0;
    outcome = require_number(parsed, "element_count", number);
    if (!outcome.ok()) return outcome;
    if (number != std::floor(number) || number < 0.0 || number >= 9007199254740992.0) {
        return fail(platform::Status::InvalidInput, kErrInvalidValue,
                    "field 'element_count' must be a non-negative integer below 2^53");
    }
    envelope.element_count = static_cast<std::uint64_t>(number);

    // requested_shard_count is REQUIRED on the distributed surface (the
    // single-device vs multi-device choice is explicit).
    outcome = require_number(parsed, "requested_shard_count", number);
    if (!outcome.ok()) return outcome;
    {
        std::int64_t shards = 0;
        // Reuse the integral check through a JsonValue round-trip.
        const JsonValue as_value = JsonValue::make_number(number);
        outcome = integral_number(as_value, "requested_shard_count", shards);
        if (!outcome.ok()) return outcome;
        if (shards == 0 || shards > 4294967295LL) {
            return fail(platform::Status::InvalidInput, kErrInvalidValue,
                        "field 'requested_shard_count' must be between 1 and 2^32-1");
        }
        requested_shard_count = static_cast<std::uint32_t>(shards);
    }

    const JsonValue* backend = field(parsed, "requested_backend");
    if (backend != nullptr) {
        if (!backend->is_string()) {
            return fail(platform::Status::InvalidInput, kErrInvalidType,
                        "field 'requested_backend' must be a string");
        }
        envelope.requested_backend = backend->as_string();
    }

    const JsonValue* priority = field(parsed, "priority");
    if (priority != nullptr) {
        if (!priority->is_number()) {
            return fail(platform::Status::InvalidInput, kErrInvalidType,
                        "field 'priority' must be a number");
        }
        envelope.priority = static_cast<std::int32_t>(priority->as_number());
    }

    outcome = require_string(parsed, "protocol_version", envelope.protocol_version);
    if (!outcome.ok()) return outcome;

    // The id charset/length rules are the platform's (validated here, the
    // same place the Phase 11 contract validates job ids).
    std::string id_error;
    if (vortyx::platform::validate_id("job_id", envelope.job_id, id_error) !=
        platform::Status::Ok) {
        return fail(platform::Status::InvalidInput, platform::contract::kErrInvalidId, id_error);
    }

    if (envelope.protocol_version != platform::kProtocolVersion) {
        return fail(platform::Status::InvalidInput, kErrUnsupportedProtocol,
                    std::string("unsupported protocol version '") + envelope.protocol_version +
                        "' (this control plane speaks '" + platform::kProtocolVersion + "')");
    }

    const JsonValue* created = field(parsed, "created_at_ms");
    if (created != nullptr) {
        if (!created->is_number()) {
            return fail(platform::Status::InvalidInput, kErrInvalidType,
                        "field 'created_at_ms' must be a number");
        }
        envelope.created_at_ms = static_cast<std::int64_t>(created->as_number());
    }

    return ok();
}

// ---------------------------------------------------------------------------
// serialize_cluster_view
// ---------------------------------------------------------------------------

std::string serialize_cluster_view(const ClusterSnapshot& snapshot) {
    JsonValue out = JsonValue::make_object();
    out.add("revision", JsonValue::make_number(static_cast<double>(snapshot.revision)));
    JsonValue devices = JsonValue::make_array();
    for (const DeviceSnapshot& device : snapshot.devices) {
        JsonValue entry = JsonValue::make_object();
        entry.add("device_id", JsonValue::make_string(device.device_id));
        entry.add("state", JsonValue::make_string(to_string(device.state)));
        entry.add("health", JsonValue::make_string(to_string(device.health)));
        entry.add("capacity", serialize_resource_vector(device.capabilities.capacity));
        entry.add("allocated", serialize_resource_vector(device.allocated));
        JsonValue backends = JsonValue::make_array();
        for (const std::string& backend : device.capabilities.metadata.backends) {
            backends.push(JsonValue::make_string(backend));
        }
        entry.add("backends", std::move(backends));
        entry.add("running_shards", JsonValue::make_number(static_cast<double>(device.running_shards)));
        entry.add("last_heartbeat_ms",
                  device.last_heartbeat_ms >= 0
                      ? JsonValue(JsonValue::make_number(static_cast<double>(device.last_heartbeat_ms)))
                      : platform::JsonValue());
        devices.push(std::move(entry));
    }
    out.add("devices", std::move(devices));
    return out.serialize();
}

// ---------------------------------------------------------------------------
// serialize_distributed_job / serialize_shards
// ---------------------------------------------------------------------------

std::string serialize_distributed_job(const DistributedJobRecord& job) {
    JsonValue out = JsonValue::make_object();
    out.add("job_id", JsonValue::make_string(job.job_id));
    out.add("operation", JsonValue::make_string(vortyx::compute::workload_label(job.operation)));
    out.add("element_count", JsonValue::make_number(static_cast<double>(job.element_count)));
    out.add("requested_backend", JsonValue::make_string(job.requested_backend));
    out.add("requested_shard_count",
            JsonValue::make_number(static_cast<double>(job.requested_shard_count)));
    out.add("status", JsonValue::make_string(to_string(job.status)));
    out.add("error", JsonValue::make_string(job.error));

    // The shard table (same rendering as the shards endpoint).
    JsonValue shards = JsonValue::make_array();
    for (const JobShard& shard : job.shards) {
        JsonValue entry = JsonValue::make_object();
        entry.add("shard_id", JsonValue::make_string(shard.shard_id));
        entry.add("index", JsonValue::make_number(static_cast<double>(shard.index)));
        entry.add("state", JsonValue::make_string(to_string(shard.state)));
        entry.add("element_begin",
                  JsonValue::make_number(static_cast<double>(shard.work.element_range.begin)));
        entry.add("element_end",
                  JsonValue::make_number(static_cast<double>(shard.work.element_range.end)));
        entry.add("device_id", JsonValue::make_string(shard.assigned_device));
        entry.add("attempt", JsonValue::make_number(static_cast<double>(shard.attempt)));
        entry.add("retry_count", JsonValue::make_number(static_cast<double>(shard.retry_count)));
        entry.add("failure_code", JsonValue::make_string(shard.last_failure_code));
        shards.push(std::move(entry));
    }
    out.add("shards", std::move(shards));

    // Aggregate counts (honest at every stage; completed only when full).
    out.add("shard_count", JsonValue::make_number(static_cast<double>(job.result.shard_count)));
    out.add("succeeded", JsonValue::make_number(static_cast<double>(job.result.succeeded)));
    out.add("failed", JsonValue::make_number(static_cast<double>(job.result.failed)));
    out.add("cancelled", JsonValue::make_number(static_cast<double>(job.result.cancelled)));
    out.add("duplicates", JsonValue::make_number(static_cast<double>(job.result.duplicates)));
    out.add("completed", JsonValue::make_bool(job.result.completed));

    out.add("created_at_ms",
            job.created_at_ms >= 0
                ? JsonValue(JsonValue::make_number(static_cast<double>(job.created_at_ms)))
                : platform::JsonValue());
    out.add("completed_at_ms",
            job.completed_at_ms > 0
                ? JsonValue(JsonValue::make_number(static_cast<double>(job.completed_at_ms)))
                : platform::JsonValue());
    return out.serialize();
}

std::string serialize_shards(const DistributedJobRecord& job) {
    JsonValue out = JsonValue::make_object();
    out.add("job_id", JsonValue::make_string(job.job_id));
    JsonValue shards = JsonValue::make_array();
    for (const JobShard& shard : job.shards) {
        JsonValue entry = JsonValue::make_object();
        entry.add("shard_id", JsonValue::make_string(shard.shard_id));
        entry.add("index", JsonValue::make_number(static_cast<double>(shard.index)));
        entry.add("state", JsonValue::make_string(to_string(shard.state)));
        entry.add("element_begin",
                  JsonValue::make_number(static_cast<double>(shard.work.element_range.begin)));
        entry.add("element_end",
                  JsonValue::make_number(static_cast<double>(shard.work.element_range.end)));
        entry.add("device_id", JsonValue::make_string(shard.assigned_device));
        entry.add("attempt", JsonValue::make_number(static_cast<double>(shard.attempt)));
        entry.add("retry_count", JsonValue::make_number(static_cast<double>(shard.retry_count)));
        entry.add("failure_code", JsonValue::make_string(shard.last_failure_code));
        shards.push(std::move(entry));
    }
    out.add("shards", std::move(shards));
    return out.serialize();
}

}  // namespace vortyx::distributed::contract
