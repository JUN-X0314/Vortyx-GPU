// Worker protocol wire contract (Phase 15) — implementation.

#include "worker/worker_protocol.hpp"

#include <vector>

#include "platform/contract.hpp"  // the vocabulary/id helpers where useful

namespace vortyx::worker {

namespace {

bool get_u64(const vortyx::platform::JsonValue& object, const std::string& key,
             std::uint64_t& out) {
    const vortyx::platform::JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_number()) return false;
    const double number = value->as_number();
    if (number < 0 || number > 18446744073709551615.0) return false;
    out = static_cast<std::uint64_t>(number);
    return true;
}

bool get_i64(const vortyx::platform::JsonValue& object, const std::string& key,
             std::int64_t& out) {
    const vortyx::platform::JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_number()) return false;
    const double number = value->as_number();
    if (number < -9223372036854775808.0 || number > 9223372036854775807.0) return false;
    out = static_cast<std::int64_t>(number);
    return true;
}

bool get_string(const vortyx::platform::JsonValue& object, const std::string& key,
                std::string& out) {
    const vortyx::platform::JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_string()) return false;
    out = value->as_string();
    return true;
}

bool get_bool(const vortyx::platform::JsonValue& object, const std::string& key, bool& out) {
    const vortyx::platform::JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_bool()) return false;
    out = value->as_bool();
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared parsing helpers
// ---------------------------------------------------------------------------

bool parse_response_object(const std::string& body, vortyx::platform::JsonValue& holder,
                           std::string& error) {
    if (!vortyx::platform::parse_json(body, holder, error)) {
        error = "response is not valid JSON: " + error;
        return false;
    }
    if (!holder.is_object()) {
        error = "response is not a JSON object";
        return false;
    }
    return true;
}

bool extract_error_body(const vortyx::platform::JsonValue& object, std::string& code,
                        std::string& message) {
    const vortyx::platform::JsonValue* error = object.find("error");
    if (error == nullptr || !error->is_object()) return false;
    if (!get_string(*error, "code", code)) return false;
    if (!get_string(*error, "message", message)) return false;
    return true;
}

namespace {

// The strict-vocabulary check every parsed object runs: unknown fields are
// a refusal (the project's reader rule — a schema evolution adds fields
// here explicitly, never silently).
bool only_known_fields(const vortyx::platform::JsonValue& object,
                       const std::vector<const char*>& known) {
    for (const auto& member : object.members()) {
        bool found = false;
        for (const char* name : known) {
            if (member.first == name) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------

std::string encode_claim_request(const ClaimRequest& request) {
    vortyx::platform::JsonValue body = vortyx::platform::JsonValue::make_object();
    body.add("worker_id", vortyx::platform::JsonValue::make_string(request.worker_id));
    body.add("lease_ms", vortyx::platform::JsonValue::make_number(
                             static_cast<double>(request.lease_ms)));
    return body.serialize();
}

std::string encode_heartbeat_request(const std::string& worker_id) {
    vortyx::platform::JsonValue body = vortyx::platform::JsonValue::make_object();
    body.add("worker_id", vortyx::platform::JsonValue::make_string(worker_id));
    return body.serialize();
}

std::string encode_complete_request(const std::string& worker_id,
                                    const CompletionReport& report, std::string& error) {
    if (report.terminal_status != kTerminalCompleted &&
        report.terminal_status != kTerminalFailed &&
        report.terminal_status != kTerminalCancelled) {
        error = "terminal_status must be completed | failed | cancelled";
        return "";
    }
    if ((report.terminal_status == kTerminalFailed ||
         report.terminal_status == kTerminalCancelled) &&
        report.error.empty()) {
        error = "a failed/cancelled report requires its reason";
        return "";
    }
    vortyx::platform::JsonValue body = vortyx::platform::JsonValue::make_object();
    body.add("worker_id", vortyx::platform::JsonValue::make_string(worker_id));
    body.add("status", vortyx::platform::JsonValue::make_string(report.terminal_status));
    body.add("error", vortyx::platform::JsonValue::make_string(report.error));
    body.add("backend", vortyx::platform::JsonValue::make_string(report.backend));
    if (report.has_result_element_count) {
        body.add("result_element_count",
                 vortyx::platform::JsonValue::make_number(
                     static_cast<double>(report.result_element_count)));
    } else {
        body.add("result_element_count", vortyx::platform::JsonValue::make_null());
    }
    if (report.has_shard_summary) {
        body.add("shards_total",
                 vortyx::platform::JsonValue::make_number(report.shards_total));
        body.add("shards_succeeded",
                 vortyx::platform::JsonValue::make_number(report.shards_succeeded));
        body.add("shards_failed",
                 vortyx::platform::JsonValue::make_number(report.shards_failed));
    }
    return body.serialize();
}

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

void parse_claim_response(const std::string& body, ClaimResponse& out) {
    out = ClaimResponse{};
    std::string error;
    vortyx::platform::JsonValue holder;
    if (!parse_response_object(body, holder, error)) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = error;
        return;
    }
    const vortyx::platform::JsonValue& parsed = holder;
    // A failure body is the unified error shape.
    std::string code;
    std::string message;
    if (extract_error_body(parsed, code, message)) {
        out.ok = false;
        out.error_code = code;
        out.error_message = message;
        return;
    }
    if (!only_known_fields(parsed, {"claimed", "job"})) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "claim response has unknown fields";
        return;
    }
    if (!get_bool(parsed, "claimed", out.claimed)) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "claim response is missing the 'claimed' flag";
        return;
    }
    if (!out.claimed) return;  // no work available: a valid, honest outcome

    const vortyx::platform::JsonValue* job = parsed.find("job");
    if (job == nullptr || !job->is_object()) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "claimed=true requires the 'job' object";
        return;
    }
    ClaimedJob claimed;
    if (!only_known_fields(*job, {"job_id", "project_id", "operation", "element_count",
                                  "requested_backend", "requested_shard_count", "attempt",
                                  "lease_expires_at_ms"})) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "claimed job has unknown fields";
        return;
    }
    if (!get_string(*job, "job_id", claimed.job_id) || claimed.job_id.empty()) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "claimed job is missing its job_id";
        return;
    }
    if (!get_string(*job, "project_id", claimed.project_id)) claimed.project_id = "";
    if (!get_string(*job, "operation", claimed.operation) || claimed.operation.empty()) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "claimed job is missing its operation";
        return;
    }
    if (!get_u64(*job, "element_count", claimed.element_count) ||
        claimed.element_count == 0) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "claimed job has an invalid element_count";
        return;
    }
    if (!get_string(*job, "requested_backend", claimed.requested_backend)) {
        claimed.requested_backend = "";
    }
    std::uint64_t shards = 0;
    claimed.requested_shard_count =
        get_u64(*job, "requested_shard_count", shards) && shards > 0
            ? static_cast<std::uint32_t>(shards)
            : 1;
    std::uint64_t attempt = 0;
    claimed.attempt =
        get_u64(*job, "attempt", attempt) ? static_cast<std::uint32_t>(attempt) : 0;
    get_i64(*job, "lease_expires_at_ms", claimed.lease_expires_at_ms);
    out.job = claimed;
}

void parse_heartbeat_response(const std::string& body, HeartbeatResponse& out) {
    out = HeartbeatResponse{};
    std::string error;
    vortyx::platform::JsonValue holder;
    if (!parse_response_object(body, holder, error)) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = error;
        return;
    }
    const vortyx::platform::JsonValue& parsed = holder;
    std::string code;
    std::string message;
    if (extract_error_body(parsed, code, message)) {
        out.ok = false;
        out.error_code = code;
        out.error_message = message;
        return;
    }
    if (!only_known_fields(parsed, {"accepted", "cancel_requested", "lease_expires_at_ms"})) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "heartbeat response has unknown fields";
        return;
    }
    if (!get_bool(parsed, "accepted", out.accepted)) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "heartbeat response is missing the 'accepted' flag";
        return;
    }
    get_bool(parsed, "cancel_requested", out.cancel_requested);
    get_i64(parsed, "lease_expires_at_ms", out.lease_expires_at_ms);
}

void parse_complete_response(const std::string& body, CompleteResponse& out) {
    out = CompleteResponse{};
    std::string error;
    vortyx::platform::JsonValue holder;
    if (!parse_response_object(body, holder, error)) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = error;
        return;
    }
    const vortyx::platform::JsonValue& parsed = holder;
    std::string code;
    std::string message;
    if (extract_error_body(parsed, code, message)) {
        out.ok = false;
        out.error_code = code;
        out.error_message = message;
        return;
    }
    if (!only_known_fields(parsed, {"recorded", "status"})) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "complete response has unknown fields";
        return;
    }
    if (!get_bool(parsed, "recorded", out.recorded)) {
        out.ok = false;
        out.error_code = "invalid_response";
        out.error_message = "complete response is missing the 'recorded' flag";
        return;
    }
    get_string(parsed, "status", out.status);
}

}  // namespace vortyx::worker
