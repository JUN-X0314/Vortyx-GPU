// API contract codec implementation (Phase 11).
//
// Every parse failure carries its stable error code and its HTTP status, so
// transports cannot improvise mappings. Every serializer writes fields in
// the documented order, so responses are byte-for-byte deterministic.

#include "platform/contract.hpp"

#include <cmath>

#include "core/version.hpp"
#include "platform/identity.hpp"
#include "platform/json.hpp"

namespace vortyx::platform::contract {

namespace {

// ParseOutcome helpers ------------------------------------------------------

ParseOutcome fail(Status status, const std::string& code, const std::string& message) {
    ParseOutcome outcome;
    outcome.status = status;
    outcome.error_code = code;
    outcome.message = message;
    outcome.http_status_code = http_status(status, code);
    return outcome;
}

ParseOutcome fail_invalid_json(const std::string& message) {
    return fail(Status::InvalidInput, kErrInvalidJson, message);
}

ParseOutcome ok() {
    ParseOutcome outcome;
    outcome.status = Status::Ok;
    outcome.http_status_code = 200;
    return outcome;
}

// Field helpers ---------------------------------------------------------------
// Each helper either fills its output and returns true, or returns a
// finished ParseOutcome with the precise error code.

const JsonValue* field(const JsonValue& object, const std::string& key) {
    return object.find(key);
}

ParseOutcome require_string(const JsonValue& object, const std::string& key, std::string& value) {
    const JsonValue* item = field(object, key);
    if (item == nullptr) {
        return fail(Status::InvalidInput, kErrMissingField, "missing required field '" + key + "'");
    }
    if (!item->is_string()) {
        return fail(Status::InvalidInput, kErrInvalidType, "field '" + key + "' must be a string");
    }
    value = item->as_string();
    return ok();
}

ParseOutcome optional_string(const JsonValue& object, const std::string& key,
                             std::string& value) {
    const JsonValue* item = field(object, key);
    if (item == nullptr) return ok();  // absent -> leave the default
    if (!item->is_string()) {
        return fail(Status::InvalidInput, kErrInvalidType, "field '" + key + "' must be a string");
    }
    value = item->as_string();
    return ok();
}

// Integral JSON number -> int64 (documented double-exactness domain).
ParseOutcome integral_number(const JsonValue& item, const std::string& key,
                             std::int64_t& value) {
    if (!item.is_number()) {
        return fail(Status::InvalidInput, kErrInvalidType, "field '" + key + "' must be a number");
    }
    const double raw = item.as_number();
    if (raw != std::floor(raw) || std::fabs(raw) >= 9007199254740992.0) {
        return fail(Status::InvalidInput, kErrInvalidValue,
                    "field '" + key + "' must be an integral number below 2^53");
    }
    value = static_cast<std::int64_t>(raw);
    return ok();
}

ParseOutcome optional_int64(const JsonValue& object, const std::string& key,
                            std::optional<std::int64_t>& value) {
    const JsonValue* item = field(object, key);
    if (item == nullptr) return ok();
    std::int64_t parsed = 0;
    ParseOutcome outcome = integral_number(*item, key, parsed);
    if (!outcome.ok()) return outcome;
    value = parsed;
    return ok();
}

ParseOutcome string_array(const JsonValue& object, const std::string& key,
                          std::vector<std::string>& value) {
    const JsonValue* item = field(object, key);
    if (item == nullptr) return ok();
    if (!item->is_array()) {
        return fail(Status::InvalidInput, kErrInvalidType, "field '" + key + "' must be an array");
    }
    std::vector<std::string> entries;
    for (const JsonValue& entry : item->items()) {
        if (!entry.is_string()) {
            return fail(Status::InvalidInput, kErrInvalidType,
                        "every entry of '" + key + "' must be a string");
        }
        entries.push_back(entry.as_string());
    }
    value = std::move(entries);
    return ok();
}

// Rejects anything the contract does not define — a typo'd field must fail
// loudly instead of being silently ignored (strict Phase 11 contract).
ParseOutcome reject_unknown_fields(const JsonValue& object,
                                   const std::vector<const char*>& known) {
    for (const auto& [key, value] : object.members()) {
        (void)value;
        bool is_known = false;
        for (const char* name : known) {
            if (key == name) {
                is_known = true;
                break;
            }
        }
        if (!is_known) {
            return fail(Status::InvalidInput, kErrInvalidValue, "unknown field '" + key + "'");
        }
    }
    return ok();
}

ParseOutcome parse_root(const std::string& body, JsonValue& root) {
    std::string json_error;
    if (!parse_json(body, root, json_error)) {
        return fail_invalid_json(json_error);
    }
    if (!root.is_object()) {
        return fail(Status::InvalidInput, kErrInvalidType, "request body must be a JSON object");
    }
    return ok();
}

// Timestamps: optional<int64> -> JSON number or null.
JsonValue timestamp(std::optional<std::int64_t> ms) {
    if (ms.has_value()) {
        return JsonValue::make_number(static_cast<double>(*ms));
    }
    return JsonValue::make_null();
}

}  // namespace

// ---------------------------------------------------------------------------
// HTTP status mapping
// ---------------------------------------------------------------------------

int http_status(Status status, const std::string& error_code) {
    switch (status) {
        case Status::Ok: return 200;
        case Status::InvalidInput:
            return error_code == kErrInvalidJson ? 400 : 422;
        case Status::Unauthenticated: return 401;
        case Status::Forbidden: return 403;
        case Status::NotFound: return 404;
        case Status::Conflict: return 409;
        case Status::Internal: return 500;
    }
    return 500;
}

const char* store_error_code(Status status) {
    switch (status) {
        case Status::Ok: return "";
        case Status::InvalidInput: return "invalid_request";
        case Status::Unauthenticated: return kErrUnauthenticated;
        case Status::Forbidden: return kErrForbidden;
        case Status::NotFound: return kErrNotFound;
        case Status::Conflict: return kErrConflict;
        case Status::Internal: return kErrInternal;
    }
    return kErrInternal;
}

// ---------------------------------------------------------------------------
// Error responses
// ---------------------------------------------------------------------------

std::string error_body(const std::string& code, const std::string& message) {
    JsonValue error = JsonValue::make_object();
    error.add("code", JsonValue::make_string(code));
    error.add("message", JsonValue::make_string(message));
    JsonValue body = JsonValue::make_object();
    body.add("error", std::move(error));
    return body.serialize();
}

// ---------------------------------------------------------------------------
// POST /api/devices — register a device
// ---------------------------------------------------------------------------

ParseOutcome parse_register_device(const std::string& body, DeviceId& device_id,
                                   DeviceMetadata& metadata) {
    JsonValue root;
    ParseOutcome outcome = parse_root(body, root);
    if (!outcome.ok()) return outcome;

    outcome = reject_unknown_fields(root, {
        "device_id", "protocol_version", "software_version", "operating_system",
        "architecture", "display_name", "backends", "operations",
    });
    if (!outcome.ok()) return outcome;

    std::string protocol_version;
    std::string software_version;
    outcome = require_string(root, "device_id", device_id);
    if (!outcome.ok()) return outcome;
    outcome = require_string(root, "protocol_version", protocol_version);
    if (!outcome.ok()) return outcome;
    outcome = require_string(root, "software_version", software_version);
    if (!outcome.ok()) return outcome;

    DeviceMetadata parsed;
    parsed.protocol_version = protocol_version;
    parsed.software_version = software_version;
    outcome = optional_string(root, "operating_system", parsed.operating_system);
    if (!outcome.ok()) return outcome;
    outcome = optional_string(root, "architecture", parsed.architecture);
    if (!outcome.ok()) return outcome;
    outcome = optional_string(root, "display_name", parsed.display_name);
    if (!outcome.ok()) return outcome;
    outcome = string_array(root, "backends", parsed.backends);
    if (!outcome.ok()) return outcome;
    outcome = string_array(root, "operations", parsed.operations);
    if (!outcome.ok()) return outcome;

    // Model-level validation (protocol version, unknown capabilities,
    // duplicates, length caps) — one place, shared by every transport.
    std::string model_error;
    if (validate_device_metadata(parsed, model_error) != Status::Ok) {
        // The model error refines into the precise code where possible.
        const std::string prefix = "unsupported protocol version";
        if (model_error.rfind(prefix, 0) == 0) {
            return fail(Status::InvalidInput, kErrUnsupportedProtocol, model_error);
        }
        if (model_error.find("unknown backend") != std::string::npos ||
            model_error.find("unknown operation") != std::string::npos) {
            return fail(Status::InvalidInput, kErrInvalidEnum, model_error);
        }
        if (model_error.find("duplicate") != std::string::npos) {
            return fail(Status::InvalidInput, kErrInvalidValue, model_error);
        }
        return fail(Status::InvalidInput, kErrInvalidValue, model_error);
    }

    // Id syntax is a contract concern: bad ids are invalid_id, not a generic
    // model failure.
    std::string id_error;
    if (validate_id("device_id", device_id, id_error) != Status::Ok) {
        return fail(Status::InvalidInput, kErrInvalidId, id_error);
    }

    metadata = std::move(parsed);
    return ok();
}

// ---------------------------------------------------------------------------
// POST /api/jobs — submit a job
// ---------------------------------------------------------------------------

ParseOutcome parse_create_job(const std::string& body, JobEnvelope& envelope,
                              std::optional<DeviceId>& submitted_by) {
    JsonValue root;
    ParseOutcome outcome = parse_root(body, root);
    if (!outcome.ok()) return outcome;

    outcome = reject_unknown_fields(root, {
        "job_id", "operation", "element_count", "requested_backend",
        "priority", "protocol_version", "created_at_ms", "submitted_by_device_id",
    });
    if (!outcome.ok()) return outcome;

    JobEnvelope parsed;
    outcome = require_string(root, "job_id", parsed.job_id);
    if (!outcome.ok()) return outcome;

    std::string operation_label;
    outcome = require_string(root, "operation", operation_label);
    if (!outcome.ok()) return outcome;

    const JsonValue* element_count_field = field(root, "element_count");
    if (element_count_field == nullptr) {
        return fail(Status::InvalidInput, kErrMissingField,
                    "missing required field 'element_count'");
    }
    std::int64_t element_count = 0;
    outcome = integral_number(*element_count_field, "element_count", element_count);
    if (!outcome.ok()) return outcome;
    if (element_count <= 0) {
        return fail(Status::InvalidInput, kErrInvalidValue,
                    "field 'element_count' must be greater than 0");
    }

    outcome = optional_string(root, "requested_backend", parsed.requested_backend);
    if (!outcome.ok()) return outcome;

    const JsonValue* priority_field = field(root, "priority");
    if (priority_field != nullptr) {
        std::int64_t priority = 0;
        outcome = integral_number(*priority_field, "priority", priority);
        if (!outcome.ok()) return outcome;
        if (priority < -2147483648LL || priority > 2147483647LL) {
            return fail(Status::InvalidInput, kErrInvalidValue,
                        "field 'priority' must fit a signed 32-bit integer");
        }
        parsed.priority = static_cast<std::int32_t>(priority);
    }

    outcome = require_string(root, "protocol_version", parsed.protocol_version);
    if (!outcome.ok()) return outcome;
    outcome = optional_int64(root, "created_at_ms", parsed.created_at_ms);
    if (!outcome.ok()) return outcome;

    std::string submitted_device;
    outcome = optional_string(root, "submitted_by_device_id", submitted_device);
    if (!outcome.ok()) return outcome;

    // Operation label -> ComputeOp (the shared vocabulary; unknown labels
    // are invalid_enum). Iterates the enum itself so the mapping can never
    // depend on the order of known_operations().
    bool known_operation = false;
    const vortyx::compute::ComputeOp all_ops[] = {
        vortyx::compute::ComputeOp::VectorAdd,
        vortyx::compute::ComputeOp::VectorMultiply,
        vortyx::compute::ComputeOp::VectorScale,
    };
    for (const vortyx::compute::ComputeOp op : all_ops) {
        if (operation_label == vortyx::compute::workload_label(op)) {
            parsed.operation = op;
            known_operation = true;
            break;
        }
    }
    if (!known_operation) {
        return fail(Status::InvalidInput, kErrInvalidEnum,
                    "unknown operation '" + operation_label +
                        "' (known: vector_add, vector_multiply, vector_scale)");
    }
    parsed.element_count = static_cast<std::uint64_t>(element_count);

    // Id syntax + model-level envelope validation.
    std::string id_error;
    if (validate_id("job_id", parsed.job_id, id_error) != Status::Ok) {
        return fail(Status::InvalidInput, kErrInvalidId, id_error);
    }
    if (!submitted_device.empty()) {
        if (validate_id("submitted_by_device_id", submitted_device, id_error) != Status::Ok) {
            return fail(Status::InvalidInput, kErrInvalidId, id_error);
        }
    }
    std::string model_error;
    if (validate_job_envelope(parsed, model_error) != Status::Ok) {
        const std::string prefix = "unsupported protocol version";
        if (model_error.rfind(prefix, 0) == 0) {
            return fail(Status::InvalidInput, kErrUnsupportedProtocol, model_error);
        }
        if (model_error.find("requested_backend") != std::string::npos) {
            return fail(Status::InvalidInput, kErrInvalidEnum, model_error);
        }
        return fail(Status::InvalidInput, kErrInvalidValue, model_error);
    }

    envelope = std::move(parsed);
    submitted_by = submitted_device.empty() ? std::nullopt : std::optional<DeviceId>(submitted_device);
    return ok();
}

// ---------------------------------------------------------------------------
// Response serialization
// ---------------------------------------------------------------------------

std::string serialize_device(const DeviceRecord& record) {
    JsonValue device = JsonValue::make_object();
    device.add("device_id", JsonValue::make_string(record.device_id));
    device.add("owner_user_id", JsonValue::make_string(record.owner_user_id));
    device.add("display_name", JsonValue::make_string(record.metadata.display_name));
    device.add("protocol_version", JsonValue::make_string(record.metadata.protocol_version));
    device.add("software_version", JsonValue::make_string(record.metadata.software_version));
    device.add("operating_system", JsonValue::make_string(record.metadata.operating_system));
    device.add("architecture", JsonValue::make_string(record.metadata.architecture));

    JsonValue backends = JsonValue::make_array();
    for (const std::string& backend : record.metadata.backends) {
        backends.push(JsonValue::make_string(backend));
    }
    device.add("backends", std::move(backends));

    JsonValue operations = JsonValue::make_array();
    for (const std::string& operation : record.metadata.operations) {
        operations.push(JsonValue::make_string(operation));
    }
    device.add("operations", std::move(operations));

    device.add("status", JsonValue::make_string(to_string(record.status)));
    device.add("last_seen_ms", timestamp(record.last_seen_ms));
    device.add("created_at_ms", timestamp(record.created_at_ms));
    return device.serialize();
}

std::string serialize_job(const JobRecord& record) {
    JsonValue job = JsonValue::make_object();
    job.add("job_id", JsonValue::make_string(record.job.job_id));
    job.add("owner_user_id", JsonValue::make_string(record.owner_user_id));
    job.add("submitted_by_device_id",
            record.submitted_by_device_id.has_value()
                ? JsonValue::make_string(*record.submitted_by_device_id)
                : JsonValue::make_null());
    job.add("operation",
            JsonValue::make_string(vortyx::compute::workload_label(record.job.operation)));
    job.add("element_count", JsonValue::make_number(static_cast<double>(record.job.element_count)));
    job.add("requested_backend", JsonValue::make_string(record.job.requested_backend));
    job.add("priority", JsonValue::make_number(static_cast<double>(record.job.priority)));
    job.add("protocol_version", JsonValue::make_string(record.job.protocol_version));
    job.add("status", JsonValue::make_string(to_string(record.status)));
    job.add("error", JsonValue::make_string(record.error));
    job.add("created_at_ms", timestamp(record.created_at_ms));
    job.add("started_at_ms", timestamp(record.started_at_ms));
    job.add("completed_at_ms", timestamp(record.completed_at_ms));
    return job.serialize();
}

std::string serialize_result(const ResultEnvelope& result) {
    JsonValue body = JsonValue::make_object();
    body.add("job_id", JsonValue::make_string(result.job_id));
    body.add("status", JsonValue::make_string(to_string(result.status)));
    body.add("backend", JsonValue::make_string(result.backend));
    body.add("error", JsonValue::make_string(result.error));
    body.add("result_element_count",
             result.result_element_count.has_value()
                 ? JsonValue::make_number(static_cast<double>(*result.result_element_count))
                 : JsonValue::make_null());
    return body.serialize();
}

std::string serialize_platform_info() {
    JsonValue info = JsonValue::make_object();
    info.add("protocol_version", JsonValue::make_string(kProtocolVersion));
    info.add("software_version", JsonValue::make_string(VORTYX_VERSION_STRING));

    JsonValue operations = JsonValue::make_array();
    for (const std::string& label : known_operations()) {
        operations.push(JsonValue::make_string(label));
    }
    info.add("operations", std::move(operations));

    JsonValue backends = JsonValue::make_array();
    for (const std::string& backend : known_backends()) {
        backends.push(JsonValue::make_string(backend));
    }
    info.add("backends", std::move(backends));
    return info.serialize();
}

}  // namespace vortyx::platform::contract
