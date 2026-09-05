// Distributed contract tests (Phase 12) — the wire codec for the
// distributed API surface: request parsing (schema violations carry their
// stable error codes and HTTP statuses) and deterministic serialization
// (byte-for-byte, documented field order).
//
// The conventions are the Phase 11 contract's: the unified error body, the
// same status mapping (400 invalid_json / 422 semantic / 404 / 409...),
// the same strict JSON module. No payload field exists anywhere — the
// control plane carries metadata only.

#include <iostream>
#include <string>

#include "distributed/contract_distributed.hpp"
#include "distributed/distributed.hpp"
#include "platform/json.hpp"

using namespace vortyx::distributed;
using vortyx::platform::JsonValue;
using vortyx::platform::parse_json;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

}  // namespace

int main() {
    using vortyx::distributed::contract::parse_create_distributed_job;

    // =====================================================================
    // 1. parse_create_distributed_job: the valid request
    // =====================================================================
    {
        const std::string body =
            "{\"job_id\":\"job-1\",\"operation\":\"vector_add\",\"element_count\":4096,"
            "\"requested_shard_count\":4,\"requested_backend\":\"cpu\",\"priority\":5,"
            "\"protocol_version\":\"1\",\"created_at_ms\":123456789}";

        vortyx::platform::JobEnvelope envelope;
        std::uint32_t shards = 0;
        const vortyx::platform::contract::ParseOutcome outcome =
            parse_create_distributed_job(body, envelope, shards);
        check(outcome.ok(), "the valid request parses");
        check(envelope.job_id == "job-1", "job_id parsed");
        check(envelope.operation == vortyx::compute::ComputeOp::VectorAdd, "operation parsed");
        check(envelope.element_count == 4096, "element_count parsed");
        check(shards == 4, "requested_shard_count parsed");
        check(envelope.requested_backend == "cpu", "requested_backend parsed");
        check(envelope.priority == 5, "priority parsed");
        check(envelope.protocol_version == "1", "protocol_version parsed");
        check(envelope.created_at_ms.has_value() && *envelope.created_at_ms == 123456789,
              "created_at_ms parsed");

        // Backend is optional; shard count is NOT (the multi-device choice
        // is explicit on the distributed surface).
        const std::string minimal =
            "{\"job_id\":\"job-2\",\"operation\":\"vector_scale\",\"element_count\":10,"
            "\"requested_shard_count\":1,\"protocol_version\":\"1\"}";
        vortyx::platform::JobEnvelope minimal_envelope;
        std::uint32_t minimal_shards = 0;
        check(parse_create_distributed_job(minimal, minimal_envelope, minimal_shards).ok(),
              "the minimal request parses");
        check(minimal_envelope.requested_backend.empty(), "backend defaults to \"\"");
        check(minimal_envelope.operation == vortyx::compute::ComputeOp::VectorScale,
              "vector_scale parses");
    }

    // =====================================================================
    // 2. Parse refusals: every violation carries its stable code
    // =====================================================================
    {
        vortyx::platform::JobEnvelope envelope;
        std::uint32_t shards = 0;

        const auto parse = [&](const std::string& body) {
            return parse_create_distributed_job(body, envelope, shards);
        };

        vortyx::platform::contract::ParseOutcome outcome =
            parse("not json at all");
        check(!outcome.ok() && outcome.error_code == "invalid_json" &&
                  outcome.http_status_code == 400,
              "unparseable JSON -> 400 invalid_json");

        outcome = parse("[]");
        check(!outcome.ok() && outcome.error_code == "invalid_type",
              "a non-object body -> invalid_type");

        outcome = parse("{}");
        check(!outcome.ok() && outcome.error_code == "missing_field",
              "an empty object -> missing_field");

        outcome = parse(
            "{\"job_id\":\"job\",\"element_count\":10,\"requested_shard_count\":1,"
            "\"protocol_version\":\"1\"}");
        check(!outcome.ok() && outcome.error_code == "missing_field" &&
                  outcome.message.find("operation") != std::string::npos,
              "a missing operation is named in the error");

        outcome = parse(
            "{\"job_id\":\"job\",\"operation\":\"matmul\",\"element_count\":10,"
            "\"requested_shard_count\":1,\"protocol_version\":\"1\"}");
        check(!outcome.ok() && outcome.error_code == "invalid_enum" &&
                  outcome.http_status_code == 422,
              "an unknown operation -> 422 invalid_enum");

        outcome = parse(
            "{\"job_id\":\"job\",\"operation\":\"vector_add\",\"element_count\":-5,"
            "\"requested_shard_count\":1,\"protocol_version\":\"1\"}");
        check(!outcome.ok() && outcome.error_code == "invalid_value",
              "a negative element_count -> invalid_value");

        // requested_shard_count is required and must be a positive integer.
        outcome = parse(
            "{\"job_id\":\"job\",\"operation\":\"vector_add\",\"element_count\":10,"
            "\"protocol_version\":\"1\"}");
        check(!outcome.ok() && outcome.error_code == "missing_field" &&
                  outcome.message.find("requested_shard_count") != std::string::npos,
              "a missing requested_shard_count is required-field");
        outcome = parse(
            "{\"job_id\":\"job\",\"operation\":\"vector_add\",\"element_count\":10,"
            "\"requested_shard_count\":0,\"protocol_version\":\"1\"}");
        check(!outcome.ok() && outcome.error_code == "invalid_value",
              "a zero shard count -> invalid_value");
        outcome = parse(
            "{\"job_id\":\"job\",\"operation\":\"vector_add\",\"element_count\":10,"
            "\"requested_shard_count\":1.5,\"protocol_version\":\"1\"}");
        check(!outcome.ok() && outcome.error_code == "invalid_value",
              "a fractional shard count -> invalid_value");

        outcome = parse(
            "{\"job_id\":\"job\",\"operation\":\"vector_add\",\"element_count\":10,"
            "\"requested_shard_count\":1,\"protocol_version\":\"9\"}");
        check(!outcome.ok() &&
                  outcome.error_code == "unsupported_protocol_version" &&
                  outcome.http_status_code == 422,
              "a wrong protocol version -> 422 unsupported_protocol_version");

        outcome = parse(
            "{\"job_id\":\"job with spaces\",\"operation\":\"vector_add\","
            "\"element_count\":10,\"requested_shard_count\":1,\"protocol_version\":\"1\"}");
        check(!outcome.ok(), "an invalid job_id is refused (charset rules)");

        outcome = parse(
            "{\"job_id\":\"job\",\"operation\":\"vector_add\",\"element_count\":10,"
            "\"requested_shard_count\":1,\"protocol_version\":\"1\",\"payload\":{\"a\":1}}");
        check(!outcome.ok(), "an unknown payload field is refused (metadata-only contract)");
    }

    // =====================================================================
    // 3. serialize_cluster_view: deterministic, parseable, honest
    // =====================================================================
    {
        DeviceSnapshot device;
        device.device_id = "device-0";
        device.owner_user_id = "user";
        device.capabilities.metadata.protocol_version = vortyx::platform::kProtocolVersion;
        device.capabilities.metadata.software_version = "0.13.0";
        device.capabilities.metadata.backends = {"cpu"};
        device.capabilities.metadata.operations = {"vector_add"};
        device.capabilities.capacity.memory_bytes = 8 * 1024 * 1024;
        device.capabilities.capacity.concurrent_jobs = 1;
        device.capabilities.max_concurrent_shards = 1;
        device.state = DeviceState::Ready;
        device.health = DeviceHealth::Healthy;
        device.allocated.concurrent_jobs = 1;
        device.last_heartbeat_ms = 5000;

        ClusterSnapshot snapshot;
        snapshot.revision = 42;
        snapshot.devices = {device};

        const std::string first = contract::serialize_cluster_view(snapshot);
        const std::string again = contract::serialize_cluster_view(snapshot);
        check(first == again, "the cluster view serializes byte-for-byte deterministically");

        JsonValue parsed;
        std::string error;
        check(parse_json(first, parsed, error), "the output is valid JSON");
        check(parsed.find("revision") != nullptr && parsed.find("revision")->as_number() == 42,
              "the revision is present");
        const JsonValue* devices = parsed.find("devices");
        check(devices != nullptr && devices->is_array() && devices->items().size() == 1,
              "the device list is present");
        const JsonValue& entry = devices->items()[0];
        check(entry.find("device_id") != nullptr &&
                  entry.find("device_id")->as_string() == "device-0",
              "the device id is present");
        check(entry.find("state") != nullptr && entry.find("state")->as_string() == "ready",
              "the device state uses the distributed vocabulary");
        check(entry.find("health") != nullptr &&
                  entry.find("health")->as_string() == "healthy",
              "the health uses the distributed vocabulary");
        check(entry.find("capacity") != nullptr &&
                  entry.find("capacity")->find("memory_bytes")->as_number() == 8 * 1024 * 1024,
              "the capacity object is present");
        check(entry.find("allocated") != nullptr &&
                  entry.find("allocated")->find("concurrent_jobs")->as_number() == 1,
              "the allocation object is present");
    }

    // =====================================================================
    // 4. serialize_distributed_job / serialize_shards
    // =====================================================================
    {
        DistributedJobRecord job;
        job.job_id = "job-9";
        job.owner_user_id = "user";
        job.operation = vortyx::compute::ComputeOp::VectorAdd;
        job.element_count = 100;
        job.requested_backend = "cpu";
        job.requested_shard_count = 2;
        job.status = DistributedJobStatus::Failed;
        job.error = "1 of 2 shards failed";
        job.created_at_ms = 1000;
        job.completed_at_ms = 2000;

        JobShard shard;
        shard.shard_id = "job-9-s0";
        shard.parent_job_id = "job-9";
        shard.index = 0;
        shard.work.kind = PartitionKind::ElementRange;
        shard.work.element_range.begin = 0;
        shard.work.element_range.end = 50;
        shard.state = ShardState::Completed;
        shard.attempt = 1;
        job.shards.push_back(shard);

        JobShard failed = shard;
        failed.shard_id = "job-9-s1";
        failed.index = 1;
        failed.work.element_range.begin = 50;
        failed.work.element_range.end = 100;
        failed.state = ShardState::Failed;
        failed.attempt = 4;
        failed.retry_count = 3;
        failed.last_failure_code = "device_lost";
        failed.last_error = "device lost mid-execution";
        job.shards.push_back(failed);

        job.result.job_id = "job-9";
        job.result.shard_count = 2;
        job.result.failed = 1;

        const std::string first = contract::serialize_distributed_job(job);
        check(first == contract::serialize_distributed_job(job),
              "the job record serializes deterministically");

        JsonValue parsed;
        std::string error;
        check(parse_json(first, parsed, error), "the job JSON parses");
        check(parsed.find("job_id")->as_string() == "job-9", "the job id is present");
        check(parsed.find("status")->as_string() == "failed",
              "the distributed status vocabulary is on the wire");
        check(parsed.find("operation")->as_string() == "vector_add",
              "the operation uses the shared Phase 10 label");
        const JsonValue* shards = parsed.find("shards");
        check(shards != nullptr && shards->items().size() == 2, "both shards are present");
        const JsonValue& failed_entry = shards->items()[1];
        check(failed_entry.find("state")->as_string() == "failed" &&
                  failed_entry.find("attempt")->as_number() == 4 &&
                  failed_entry.find("retry_count")->as_number() == 3 &&
                  failed_entry.find("failure_code")->as_string() == "device_lost",
              "the failed shard's record is on the wire");
        check(parsed.find("element_begin") == nullptr,
              "no payload fields exist on the job object");

        const std::string shard_json = contract::serialize_shards(job);
        check(shard_json == contract::serialize_shards(job), "the shard table is deterministic");
        JsonValue shard_parsed;
        check(parse_json(shard_json, shard_parsed, error), "the shard JSON parses");
        check(shard_parsed.find("shards")->items().size() == 2, "the shard table size matches");
        check(shard_parsed.find("job_id")->as_string() == "job-9", "the parent id is present");
    }

    // =====================================================================
    // 5. The status-code mapping is the Phase 11 mapping (shared source)
    // =====================================================================
    {
        check(vortyx::platform::contract::http_status(vortyx::platform::Status::Ok, "") == 200,
              "Ok -> 200");
        check(vortyx::platform::contract::http_status(vortyx::platform::Status::InvalidInput,
                                                      "invalid_json") == 400,
              "invalid_json -> 400");
        check(vortyx::platform::contract::http_status(vortyx::platform::Status::InvalidInput,
                                                      "invalid_value") == 422,
              "semantic invalid_input -> 422");
        check(vortyx::platform::contract::http_status(vortyx::platform::Status::NotFound, "") ==
                  404,
              "NotFound -> 404");
    }

    if (failures == 0) {
        std::cout << "Distributed contract tests passed.\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
