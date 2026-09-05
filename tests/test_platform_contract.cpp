// Platform API contract tests (Phase 11) — CPU path, must pass on every
// system.
//
// These tests pin the CONTROL-PLANE WIRE CONTRACT that the Vercel-hosted
// TypeScript layer (platform/api) implements too:
//
//   strict JSON module (parse + deterministic serialize + malformed
//   rejection) -> the unified error schema {"error":{code,message}} -> the
//   documented HTTP status mapping (400/401/403/404/409/422/500) -> request
//   parsers (register device / create job: valid, missing fields, invalid
//   enums, invalid ids, unknown fields, malformed JSON) -> response
//   serializers (byte-deterministic, documented field order) -> a full
//   local round trip: parse -> store -> serialize -> parse.
//
// A failure here means the two layers (C++ and TypeScript) could drift —
// the contract is deliberately tested on BOTH sides.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "core/version.hpp"
#include "platform/platform.hpp"

using vortyx::platform::AuthContext;
using vortyx::platform::DeviceId;
using vortyx::platform::DeviceMetadata;
using vortyx::platform::DeviceRecord;
using vortyx::platform::InMemoryPlatformStore;
using vortyx::platform::JobEnvelope;
using vortyx::platform::JobRecord;
using vortyx::platform::JsonValue;
using vortyx::platform::Status;
using vortyx::platform::contract::ParseOutcome;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// Minimal helper: parse text; a rejection here is a FATAL test defect (the
// remaining checks would dereference nothing valid), so the process stops
// with a failure status instead of continuing on a null value.
JsonValue must_parse(const std::string& text, const std::string& context) {
    JsonValue value;
    std::string error;
    if (!vortyx::platform::parse_json(text, value, error)) {
        std::cerr << "FAIL: " << context << " did not parse: " << error << "\n";
        std::exit(1);
    }
    return value;
}

std::string register_body(const std::string& protocol = "1") {
    return std::string(R"json({
        "device_id": "dev-1",
        "protocol_version": ")json" + protocol + R"json(",
        "software_version": "0.11.0",
        "operating_system": "linux",
        "architecture": "x86_64",
        "display_name": "workstation",
        "backends": ["cpu", "vulkan"],
        "operations": ["vector_add", "vector_scale"]
    })json");
}

std::string create_job_body() {
    return R"({
        "job_id": "job-1",
        "operation": "vector_add",
        "element_count": 1024,
        "requested_backend": "cpu",
        "priority": 3,
        "protocol_version": "1",
        "created_at_ms": 1700000000000
    })";
}

}  // namespace

int main() {
    // =====================================================================
    // 1. JSON module: strict parsing
    // =====================================================================
    {
        JsonValue value = must_parse(
            R"({"name": "vortyx", "n": -12, "f": 1.5, "e": 2e3, "ok": true, "nothing": null,
                "list": [1, "two", false], "esc": "a\"b\\c\nd\u0041\u00e9",
                "emoji": "\ud83d\ude00", "empty": {}, "earr": []})",
            "feature-rich JSON");
        check(value.is_object(), "root must be an object");
        check(value.find("name")->as_string() == "vortyx", "string member");
        check(value.find("n")->as_number() == -12, "negative integer");
        check(value.find("f")->as_number() == 1.5, "fraction");
        check(value.find("e")->as_number() == 2000, "exponent form");
        check(value.find("ok")->as_bool() == true, "true literal");
        check(value.find("nothing")->is_null(), "null literal");
        check(value.find("list")->items().size() == 3, "array members");
        check(value.find("esc")->as_string() == std::string("a\"b\\c\nd") + "Aé",
              "escape sequences decode correctly");
        check(value.find("emoji")->as_string() == std::string("\xF0\x9F\x98\x80"),
              "surrogate pairs decode to UTF-8");
        check(value.find("empty")->members().empty(), "empty object");
        check(value.find("earr")->items().empty(), "empty array");

        const std::string malformed[] = {
            "", " ", "{", "}", "[1,]", "{\"a\":}", "{\"a\" 1}", "{a:1}",
            "01", "+1", ".5", "1.", "1e", "1e+", "NaN", "Infinity", "-Infinity",
            "\"unterminated", "\"bad\\escape\"", "\"\\u12\"", "\"\\ud800\"",
            "\"\\udc00\"", "\"tab\tinside\"", "{\"a\":1} trailing", "tru", "nul",
        };
        for (const std::string& text : malformed) {
            JsonValue out;
            std::string error;
            check(!vortyx::platform::parse_json(text, out, error),
                  "malformed input must be rejected: '" + text + "'");
            check(!error.empty(), "every rejection must explain itself");
        }

        // Nesting beyond the documented depth cap is rejected (the
        // stack-exhaustion guard from the threat model).
        std::string deep(100, '[');
        deep.append(100, ']');
        JsonValue out;
        std::string error;
        check(!vortyx::platform::parse_json(deep, out, error) &&
                  error.find("maximum") != std::string::npos,
              "nesting beyond kMaxJsonDepth must be rejected");
    }

    // =====================================================================
    // 2. JSON module: deterministic serialization + round trip
    // =====================================================================
    {
        JsonValue object = JsonValue::make_object();
        object.add("zeta", JsonValue::make_number(1));
        object.add("alpha", JsonValue::make_string("first"));
        object.add("nested", [] {
            JsonValue array = JsonValue::make_array();
            array.push(JsonValue::make_bool(true));
            array.push(JsonValue::make_null());
            return array;
        }());
        object.add("big", JsonValue::make_number(1700000000000.0));
        object.add("ctrl", JsonValue::make_string(std::string("a") + char(0x01) + "b"));

        const std::string first = object.serialize();
        const std::string second = object.serialize();
        check(first == second, "serialization must be deterministic");
        check(first.find("\"zeta\":1,") == 1, "insertion order is preserved (first member)");
        check(first.find("\"big\":1700000000000") != std::string::npos,
              "epoch-scale numbers serialize exactly");
        check(first.find("\"ctrl\":\"a\\u0001b\"") != std::string::npos,
              "control characters are escaped");

        JsonValue reparsed;
        std::string error;
        check(vortyx::platform::parse_json(first, reparsed, error),
              "serialized output must parse again");
        check(reparsed.serialize() == first, "round trip must be byte-stable");

        // Duplicate key: last value wins at the original position.
        JsonValue dup = JsonValue::make_object();
        dup.add("k", JsonValue::make_number(1));
        dup.add("k", JsonValue::make_number(2));
        check(dup.members().size() == 1 && dup.find("k")->as_number() == 2,
              "duplicate keys must replace (last wins)");
    }

    // =====================================================================
    // 3. Error schema + HTTP status mapping
    // =====================================================================
    {
        using vortyx::platform::contract::error_body;
        using vortyx::platform::contract::http_status;
        using vortyx::platform::contract::kErrConflict;
        using vortyx::platform::contract::kErrInvalidJson;
        using vortyx::platform::contract::kErrNotFound;
        using vortyx::platform::contract::store_error_code;

        check(error_body("not_found", "no such job") ==
                  R"({"error":{"code":"not_found","message":"no such job"}})",
              "the unified error body shape is fixed");

        check(http_status(Status::Ok, "") == 200, "Ok maps to 200");
        check(http_status(Status::InvalidInput, kErrInvalidJson) == 400,
              "invalid_json maps to 400");
        check(http_status(Status::InvalidInput, "missing_field") == 422,
              "semantic validation failures map to 422");
        check(http_status(Status::Unauthenticated, "unauthenticated") == 401,
              "Unauthenticated maps to 401");
        check(http_status(Status::Forbidden, "forbidden") == 403,
              "Forbidden maps to 403");
        check(http_status(Status::NotFound, kErrNotFound) == 404,
              "NotFound maps to 404");
        check(http_status(Status::Conflict, kErrConflict) == 409,
              "Conflict maps to 409");
        check(http_status(Status::Internal, "internal_error") == 500,
              "Internal maps to 500");

        check(std::string(store_error_code(Status::NotFound)) == "not_found" &&
                  std::string(store_error_code(Status::Conflict)) == "conflict" &&
                  std::string(store_error_code(Status::Unauthenticated)) == "unauthenticated" &&
                  std::string(store_error_code(Status::Forbidden)) == "forbidden" &&
                  std::string(store_error_code(Status::InvalidInput)) == "invalid_request" &&
                  std::string(store_error_code(Status::Internal)) == "internal_error",
              "store outcomes map to stable error codes");
    }

    // =====================================================================
    // 4. Request parsing — register device
    // =====================================================================
    {
        DeviceId device_id;
        DeviceMetadata metadata;
        ParseOutcome outcome = vortyx::platform::contract::parse_register_device(
            register_body(), device_id, metadata);
        check(outcome.ok() && outcome.http_status_code == 200, "a valid body must parse");
        check(device_id == "dev-1", "device_id parsed");
        check(metadata.software_version == "0.11.0" &&
                  metadata.operating_system == "linux" &&
                  metadata.architecture == "x86_64" &&
                  metadata.display_name == "workstation" &&
                  metadata.backends.size() == 2 && metadata.operations.size() == 2 &&
                  metadata.protocol_version == "1",
              "metadata fields parsed");

        struct Case {
            std::string body;
            std::string code;
            int status;
            std::string label;
        };
        const std::vector<Case> cases = {
            {"{", vortyx::platform::contract::kErrInvalidJson, 400, "malformed JSON -> 400"},
            {R"([1,2])", vortyx::platform::contract::kErrInvalidType, 422,
             "non-object body -> 422"},
            {R"({"protocol_version":"1","software_version":"v"})",
             vortyx::platform::contract::kErrMissingField, 422, "missing device_id"},
            {R"({"device_id":"dev-1","software_version":"v"})",
             vortyx::platform::contract::kErrMissingField, 422, "missing protocol_version"},
            {R"({"device_id":"dev-1","protocol_version":"1"})",
             vortyx::platform::contract::kErrMissingField, 422, "missing software_version"},
            {register_body("9"), vortyx::platform::contract::kErrUnsupportedProtocol, 422,
             "unsupported protocol version"},
            {R"({"device_id":"bad id","protocol_version":"1","software_version":"v"})",
             vortyx::platform::contract::kErrInvalidId, 422, "invalid id characters"},
            {R"({"device_id":5,"protocol_version":"1","software_version":"v"})",
             vortyx::platform::contract::kErrInvalidType, 422, "wrong field type"},
            {R"({"device_id":"dev-1","protocol_version":"1","software_version":"v",
                 "backends":["cuda"]})",
             vortyx::platform::contract::kErrInvalidEnum, 422, "unknown backend"},
            {R"({"device_id":"dev-1","protocol_version":"1","software_version":"v",
                 "operations":["vector_add","vector_add"]})",
             vortyx::platform::contract::kErrInvalidValue, 422, "duplicate operation"},
            {R"({"device_id":"dev-1","protocol_version":"1","software_version":"v",
                 "ghost_field":1})",
             vortyx::platform::contract::kErrInvalidValue, 422, "unknown field rejected"},
            {R"({"device_id":"dev-1","protocol_version":"1","software_version":"v",
                 "backends":"cpu"})",
             vortyx::platform::contract::kErrInvalidType, 422, "backends must be an array"},
        };
        for (const Case& test_case : cases) {
            DeviceId parsed_id;
            DeviceMetadata parsed_metadata;
            const ParseOutcome parsed = vortyx::platform::contract::parse_register_device(
                test_case.body, parsed_id, parsed_metadata);
            check(!parsed.ok() && parsed.error_code == test_case.code &&
                      parsed.http_status_code == test_case.status,
                  test_case.label + " (got " + parsed.error_code + "/" +
                      std::to_string(parsed.http_status_code) + ")");
            check(!parsed.message.empty(),
                  test_case.label + " must carry a human-readable message");
        }
    }

    // =====================================================================
    // 5. Request parsing — create job
    // =====================================================================
    {
        JobEnvelope envelope;
        std::optional<DeviceId> submitted_by;
        ParseOutcome outcome =
            vortyx::platform::contract::parse_create_job(create_job_body(), envelope, submitted_by);
        check(outcome.ok(), "a valid job body must parse");
        check(envelope.job_id == "job-1" &&
                  envelope.operation == vortyx::compute::ComputeOp::VectorAdd &&
                  envelope.element_count == 1024 && envelope.requested_backend == "cpu" &&
                  envelope.priority == 3 && envelope.protocol_version == "1" &&
                  envelope.created_at_ms.has_value() &&
                  *envelope.created_at_ms == 1700000000000LL,
              "envelope fields parsed exactly");
        check(!submitted_by.has_value(), "an absent device reference parses as absent");

        JobEnvelope with_device;
        std::optional<DeviceId> device_ref;
        outcome = vortyx::platform::contract::parse_create_job(
            R"json({"job_id":"j2","operation":"vector_add","element_count":8,
                     "protocol_version":"1","submitted_by_device_id":"dev-1"})json",
            with_device, device_ref);
        check(outcome.ok() && device_ref.has_value() && *device_ref == "dev-1",
              "an optional device reference parses when present");

        // Minimal body: optional fields absent.
        JobEnvelope minimal;
        std::optional<DeviceId> minimal_submitted;
        outcome = vortyx::platform::contract::parse_create_job(
            R"json({"job_id":"j","operation":"vector_scale","element_count":8,
                "protocol_version":"1"})json",
            minimal, minimal_submitted);
        check(outcome.ok() && minimal.operation == vortyx::compute::ComputeOp::VectorScale &&
                  minimal.requested_backend.empty() && minimal.priority == 0 &&
                  !minimal.created_at_ms.has_value(),
              "optional fields default honestly");

        const struct {
            const char* body;
            std::string code;
            int status;
            std::string label;
        } cases[] = {
            {"{", vortyx::platform::contract::kErrInvalidJson, 400, "malformed JSON -> 400"},
            {R"({"operation":"vector_add","element_count":4,"protocol_version":"1"})",
             vortyx::platform::contract::kErrMissingField, 422, "missing job_id"},
            {R"({"job_id":"j","element_count":4,"protocol_version":"1"})",
             vortyx::platform::contract::kErrMissingField, 422, "missing operation"},
            {R"({"job_id":"j","operation":"matrix_multiply","element_count":4,
                 "protocol_version":"1"})",
             vortyx::platform::contract::kErrInvalidEnum, 422, "unknown operation"},
            {R"({"job_id":"j","operation":7,"element_count":4,"protocol_version":"1"})",
             vortyx::platform::contract::kErrInvalidType, 422, "operation must be a string"},
            {R"({"job_id":"j","operation":"vector_add","protocol_version":"1"})",
             vortyx::platform::contract::kErrMissingField, 422, "missing element_count"},
            {R"({"job_id":"j","operation":"vector_add","element_count":0,
                 "protocol_version":"1"})",
             vortyx::platform::contract::kErrInvalidValue, 422, "zero element_count"},
            {R"({"job_id":"j","operation":"vector_add","element_count":-5,
                 "protocol_version":"1"})",
             vortyx::platform::contract::kErrInvalidValue, 422, "negative element_count"},
            {R"({"job_id":"j","operation":"vector_add","element_count":1.5,
                 "protocol_version":"1"})",
             vortyx::platform::contract::kErrInvalidValue, 422, "non-integral element_count"},
            {R"({"job_id":"j","operation":"vector_add","element_count":"4",
                 "protocol_version":"1"})",
             vortyx::platform::contract::kErrInvalidType, 422, "element_count must be a number"},
            {R"({"job_id":"j","operation":"vector_add","element_count":4,
                 "requested_backend":"cuda","protocol_version":"1"})",
             vortyx::platform::contract::kErrInvalidEnum, 422, "unknown requested_backend"},
            {R"({"job_id":"j","operation":"vector_add","element_count":4,
                 "priority":99999999999,"protocol_version":"1"})",
             vortyx::platform::contract::kErrInvalidValue, 422, "priority outside int32"},
            {R"({"job_id":"j","operation":"vector_add","element_count":4,
                 "protocol_version":"2"})",
             vortyx::platform::contract::kErrUnsupportedProtocol, 422,
             "unsupported protocol version"},
            {R"({"job_id":"j","operation":"vector_add","element_count":4,
                 "protocol_version":"1","surprise":true})",
             vortyx::platform::contract::kErrInvalidValue, 422, "unknown field rejected"},
        };
        for (const auto& test_case : cases) {
            JobEnvelope parsed;
            std::optional<DeviceId> parsed_submitted;
            const ParseOutcome parsed_outcome = vortyx::platform::contract::parse_create_job(
                test_case.body, parsed, parsed_submitted);
            check(!parsed_outcome.ok() && parsed_outcome.error_code == test_case.code &&
                      parsed_outcome.http_status_code == test_case.status,
                  test_case.label + " (got " + parsed_outcome.error_code + "/" +
                      std::to_string(parsed_outcome.http_status_code) + ")");
        }
    }

    // =====================================================================
    // 6. Response serializers: documented field order, exact values
    // =====================================================================
    {
        using vortyx::platform::contract::serialize_device;
        using vortyx::platform::contract::serialize_job;
        using vortyx::platform::contract::serialize_platform_info;
        using vortyx::platform::contract::serialize_result;

        InMemoryPlatformStore store;
        const AuthContext alice = vortyx::platform::make_authenticated("user-alice");
        DeviceMetadata metadata;
        metadata.software_version = "0.11.0";
        metadata.backends = {"cpu"};
        metadata.operations = {"vector_add"};
        DeviceRecord device;
        check(store.register_device(alice, "dev-1", metadata, device) == Status::Ok,
              "store setup for serialization");

        const std::string device_json = serialize_device(device);
        check(device_json.find("\"device_id\":\"dev-1\"") == 1,
              "serialize_device starts with device_id in documented order");
        check(device_json.find("\"owner_user_id\":\"user-alice\"") != std::string::npos &&
                  device_json.find("\"status\":\"online\"") != std::string::npos &&
                  device_json.find("\"last_seen_ms\":") != std::string::npos,
              "serialize_device carries owner + status + server timestamps");
        check(device_json == serialize_device(device),
              "device serialization is deterministic");
        JsonValue parsed_device = must_parse(device_json, "serialized device");
        check(parsed_device.members().at(0).first == "device_id",
              "field order: device_id first");
        check(parsed_device.members().back().first == "created_at_ms",
              "field order: created_at_ms last");

        JobRecord job;
        bool created = false;
        check(store.create_job(alice, vortyx::platform::JobEnvelope{
                                        "job-1", vortyx::compute::ComputeOp::VectorScale,
                                        64, "vulkan", -2, "1", std::int64_t{1700000000000LL}},
                               "dev-1", job, created) == Status::Ok,
              "job setup for serialization");
        const std::string job_json = serialize_job(job);
        JsonValue parsed_job = must_parse(job_json, "serialized job");
        check(parsed_job.find("operation")->as_string() == "vector_scale" &&
                  parsed_job.find("element_count")->as_number() == 64 &&
                  parsed_job.find("requested_backend")->as_string() == "vulkan" &&
                  parsed_job.find("priority")->as_number() == -2 &&
                  parsed_job.find("status")->as_string() == "queued" &&
                  parsed_job.find("error")->as_string() == "" &&
                  parsed_job.find("submitted_by_device_id")->as_string() == "dev-1" &&
                  parsed_job.find("started_at_ms")->is_null() &&
                  parsed_job.find("completed_at_ms")->is_null(),
              "serialize_job values are exact (unset timestamps are null, never 0)");

        vortyx::platform::ResultEnvelope result;
        result.job_id = "job-1";
        result.status = vortyx::platform::JobStatus::Completed;
        result.backend = "vulkan";
        result.result_element_count = 64;
        const std::string result_json = serialize_result(result);
        JsonValue parsed_result = must_parse(result_json, "serialized result");
        check(parsed_result.find("job_id")->as_string() == "job-1" &&
                  parsed_result.find("status")->as_string() == "completed" &&
                  parsed_result.find("backend")->as_string() == "vulkan" &&
                  parsed_result.find("error")->as_string() == "" &&
                  parsed_result.find("result_element_count")->as_number() == 64,
              "serialize_result values are exact");

        JsonValue info = must_parse(serialize_platform_info(), "platform info");
        check(info.find("protocol_version")->as_string() == "1" &&
                  info.find("software_version")->as_string() == VORTYX_VERSION_STRING &&
                  info.find("operations")->items().size() == 3 &&
                  info.find("backends")->items().size() == 2,
              "platform info exposes the contract vocabulary");
    }

    // =====================================================================
    // 7. Local round trip: parse -> store -> serialize -> parse
    //    (the exact flow the TypeScript layer implements over HTTP)
    // =====================================================================
    {
        InMemoryPlatformStore store;
        const AuthContext alice = vortyx::platform::make_authenticated("11111111-1111-4111-8111-111111111111");
        const AuthContext bob = vortyx::platform::make_authenticated("22222222-2222-4222-8222-222222222222");

        // Register through the contract.
        DeviceId device_id;
        DeviceMetadata metadata;
        ParseOutcome outcome = vortyx::platform::contract::parse_register_device(
            register_body(), device_id, metadata);
        check(outcome.ok(), "register request parses");
        DeviceRecord device;
        check(store.register_device(alice, device_id, metadata, device) == Status::Ok,
              "register executes");
        const std::string device_response =
            vortyx::platform::contract::serialize_device(device);
        JsonValue device_body = must_parse(device_response, "device response");
        check(device_body.find("owner_user_id")->as_string() == alice.user_id,
              "the response owner is the authenticated subject");

        // Foreign user cannot touch the device — invisible under the RLS
        // equivalence, mapped to 404 with the unified error body.
        const Status foreign = store.device(bob, device_id, device);
        check(foreign == Status::NotFound, "foreign read is invisible (NotFound)");
        const std::string forbidden_response = vortyx::platform::contract::error_body(
            vortyx::platform::contract::store_error_code(foreign),
            "no such device");
        check(forbidden_response.find(R"("code":"not_found")") != std::string::npos,
              "the not-found body carries the stable code");

        // Submit through the contract, replay idempotently, then serialize.
        JobEnvelope envelope;
        std::optional<DeviceId> submitted_by;
        outcome = vortyx::platform::contract::parse_create_job(create_job_body(), envelope, submitted_by);
        check(outcome.ok(), "job request parses");
        JobRecord job;
        bool created = false;
        check(store.create_job(alice, envelope, submitted_by, job, created) == Status::Ok &&
                  created,
              "job executes");
        JobRecord replay;
        bool replay_created = true;
        check(store.create_job(alice, envelope, submitted_by, replay, replay_created) ==
                      Status::Ok &&
                  !replay_created,
              "contract-level resubmission stays idempotent");
        const std::string job_response = vortyx::platform::contract::serialize_job(job);
        JsonValue job_body = must_parse(job_response, "job response");
        check(job_body.find("status")->as_string() == "queued",
              "a fresh contract job is queued on the wire");

        // Not-found maps to 404 with the unified body.
        const Status missing = store.job(bob, "job-1", job);
        check(missing == Status::NotFound, "another user's job is invisible (NotFound)");
        JobRecord nobody;
        check(store.job(bob, "totally-missing", nobody) == Status::NotFound,
              "a truly missing job is NotFound — indistinguishable from a foreign one");
    }

    if (failures == 0) {
        std::cout << "Platform contract tests passed.\n";
        return 0;
    }
    std::cerr << failures << " platform contract check(s) FAILED.\n";
    return 1;
}
