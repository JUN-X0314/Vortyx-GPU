// Platform layer tests (Phase 11) — CPU path, must pass on every system.
//
// These tests verify the Phase 11 platform foundation INVARIANTS:
//
//   identity rules (id syntax, UUID generation) -> metadata validation
//   (protocol, capability vocabulary, duplicates) -> the job model (status
//   vocabulary, the documented transition table, envelope validation) ->
//   the auth boundary (AuthN vs AuthZ, the single ownership rule) ->
//   the InMemoryPlatformStore as the executable specification of the
//   control-plane contract: registration, heartbeat, ownership, job
//   idempotency, lifecycle transitions, result recording — and thread
//   safety of the local store.
//
// No network, no cloud account, no secrets: the store under test is the
// local/mock implementation, and the rules it exercises are the same rules
// the Supabase RLS policies enforce for real (see
// platform/supabase/migrations). Every refusal is asserted WITH its exact
// Status — failures are never blurred into one generic error.

#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "platform/platform.hpp"

using vortyx::platform::anonymous;
using vortyx::platform::AuthContext;
using vortyx::platform::DeviceId;
using vortyx::platform::DeviceMetadata;
using vortyx::platform::DeviceRecord;
using vortyx::platform::DeviceStatus;
using vortyx::platform::InMemoryPlatformStore;
using vortyx::platform::JobEnvelope;
using vortyx::platform::JobId;
using vortyx::platform::JobRecord;
using vortyx::platform::JobStatus;
using vortyx::platform::ResultEnvelope;
using vortyx::platform::Status;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// A minimal, valid registration payload.
DeviceMetadata valid_metadata() {
    DeviceMetadata metadata;
    metadata.software_version = "0.11.0";
    metadata.operating_system = "linux";
    metadata.architecture = "x86_64";
    metadata.backends = {"cpu"};
    metadata.operations = {"vector_add"};
    metadata.display_name = "workstation";
    return metadata;
}

JobEnvelope valid_envelope(const JobId& job_id) {
    JobEnvelope envelope;
    envelope.job_id = job_id;
    envelope.operation = vortyx::compute::ComputeOp::VectorAdd;
    envelope.element_count = 1024;
    envelope.requested_backend = "cpu";
    envelope.protocol_version = vortyx::platform::kProtocolVersion;
    return envelope;
}

}  // namespace

int main() {
    // =====================================================================
    // 1. Identity: id syntax + UUID generation
    // =====================================================================
    {
        check(vortyx::platform::is_valid_id("node-1"), "plain id must be valid");
        check(vortyx::platform::is_valid_id("aB3._-"), "all charset chars must be valid");
        check(vortyx::platform::is_valid_id(std::string(128, 'a')),
              "a 128-char id must be valid");
        check(!vortyx::platform::is_valid_id(""), "empty id must be invalid");
        check(!vortyx::platform::is_valid_id(std::string(129, 'a')),
              "a 129-char id must be invalid");
        check(!vortyx::platform::is_valid_id("has space"), "space must be invalid");
        check(!vortyx::platform::is_valid_id("semi;colon"), "punctuation outside the charset must be invalid");
        check(!vortyx::platform::is_valid_id("sl/ash"), "slash must be invalid");

        const DeviceId generated = vortyx::platform::generate_device_id();
        check(vortyx::platform::is_valid_id(generated),
              "generated device id must satisfy the id rule");
        check(generated.size() == 36 && generated[8] == '-' && generated[13] == '-' &&
                  generated[18] == '-' && generated[23] == '-',
              "generated id must be a canonical UUID shape");
        check(generated[14] == '4', "generated id must be a version-4 UUID");
        const JobId other = vortyx::platform::generate_job_id();
        check(other != generated, "two generated ids must differ");
        check(vortyx::platform::generate_device_id() != generated,
              "generated device ids must be unique across calls");

        std::string error;
        check(vortyx::platform::validate_id("device_id", "ok-id", error) == Status::Ok,
              "validate_id must accept a valid id");
        check(vortyx::platform::validate_id("device_id", "", error) == Status::InvalidInput &&
                  error.find("must not be empty") != std::string::npos,
              "validate_id must explain an empty id");
        check(vortyx::platform::validate_id("job_id", "bad id", error) == Status::InvalidInput &&
                  error.find("invalid character") != std::string::npos,
              "validate_id must explain a bad character");
    }

    // =====================================================================
    // 2. Metadata: protocol, capability vocabulary, duplicates, caps
    // =====================================================================
    {
        std::string error;
        DeviceMetadata metadata = valid_metadata();
        check(vortyx::platform::validate_device_metadata(metadata, error) == Status::Ok,
              "valid metadata must pass");

        DeviceMetadata minimal;
        minimal.software_version = "0.11.0";
        minimal.display_name = "headless-node";
        check(vortyx::platform::validate_device_metadata(minimal, error) == Status::Ok,
              "minimal metadata (no capabilities) must pass");

        metadata.protocol_version = "999";
        check(vortyx::platform::validate_device_metadata(metadata, error) == Status::InvalidInput &&
                  error.find("unsupported protocol version") != std::string::npos,
              "a wrong protocol version must be refused explicitly");
        metadata = valid_metadata();

        metadata.software_version = "";
        check(vortyx::platform::validate_device_metadata(metadata, error) == Status::InvalidInput,
              "empty software_version must be refused");
        metadata = valid_metadata();

        metadata.backends = {"cuda"};
        check(vortyx::platform::validate_device_metadata(metadata, error) == Status::InvalidInput &&
                  error.find("unknown backend") != std::string::npos,
              "an unknown backend must not become a fake capability");
        metadata.backends = {"cpu", "cpu"};
        check(vortyx::platform::validate_device_metadata(metadata, error) == Status::InvalidInput &&
                  error.find("duplicate") != std::string::npos,
              "duplicate backend entries must be refused");
        metadata = valid_metadata();

        metadata.operations = {"matrix_multiply"};
        check(vortyx::platform::validate_device_metadata(metadata, error) == Status::InvalidInput &&
                  error.find("unknown operation") != std::string::npos,
              "an unknown operation must be refused");
        metadata = valid_metadata();

        // The vocabulary comes from the Phase 10 workload labels — one
        // source of truth.
        check(vortyx::platform::is_known_operation(
                  vortyx::compute::workload_label(vortyx::compute::ComputeOp::VectorScale)),
              "known_operations must contain the ComputeOp labels");
        check(vortyx::platform::is_known_backend("vulkan") &&
                  vortyx::platform::is_known_backend("cpu") &&
                  !vortyx::platform::is_known_backend("cuda"),
              "known_backends must be exactly the contract names");
    }

    // =====================================================================
    // 3. Job lifecycle: status vocabulary + the documented transition table
    // =====================================================================
    {
        using vortyx::platform::job_status_is_terminal;
        using vortyx::platform::job_status_transition_valid;
        using vortyx::platform::to_string;

        check(std::string(to_string(JobStatus::Queued)) == "queued" &&
                  std::string(to_string(JobStatus::Running)) == "running" &&
                  std::string(to_string(JobStatus::Completed)) == "completed" &&
                  std::string(to_string(JobStatus::Failed)) == "failed" &&
                  std::string(to_string(JobStatus::Cancelled)) == "cancelled",
              "job status strings are the wire vocabulary");

        check(!job_status_is_terminal(JobStatus::Queued) &&
                  !job_status_is_terminal(JobStatus::Running) &&
                  job_status_is_terminal(JobStatus::Completed) &&
                  job_status_is_terminal(JobStatus::Failed) &&
                  job_status_is_terminal(JobStatus::Cancelled),
              "terminal classification must match the documented model");

        check(job_status_transition_valid(JobStatus::Queued, JobStatus::Running),
              "Queued -> Running must be valid");
        check(job_status_transition_valid(JobStatus::Queued, JobStatus::Cancelled),
              "Queued -> Cancelled must be valid");
        check(job_status_transition_valid(JobStatus::Running, JobStatus::Completed),
              "Running -> Completed must be valid");
        check(job_status_transition_valid(JobStatus::Running, JobStatus::Failed),
              "Running -> Failed must be valid");
        check(job_status_transition_valid(JobStatus::Running, JobStatus::Cancelled),
              "Running -> Cancelled must be valid");

        check(!job_status_transition_valid(JobStatus::Queued, JobStatus::Completed),
              "Queued -> Completed must be invalid (never ran)");
        check(!job_status_transition_valid(JobStatus::Queued, JobStatus::Failed),
              "Queued -> Failed must be invalid (never ran)");
        check(!job_status_transition_valid(JobStatus::Running, JobStatus::Queued),
              "Running -> Queued must be invalid");
        check(!job_status_transition_valid(JobStatus::Completed, JobStatus::Running),
              "terminal states allow no transitions");
        check(!job_status_transition_valid(JobStatus::Failed, JobStatus::Running),
              "terminal states allow no transitions");
        check(!job_status_transition_valid(JobStatus::Cancelled, JobStatus::Queued),
              "terminal states allow no transitions");
    }

    // =====================================================================
    // 4. Job envelope + result envelope validation
    // =====================================================================
    {
        std::string error;
        JobEnvelope envelope = valid_envelope("job-1");
        check(vortyx::platform::validate_job_envelope(envelope, error) == Status::Ok,
              "a valid envelope must pass");

        envelope.element_count = 0;
        check(vortyx::platform::validate_job_envelope(envelope, error) == Status::InvalidInput,
              "zero-element jobs must be refused");
        envelope.element_count = vortyx::platform::kMaxJobElementCount + 1;
        check(vortyx::platform::validate_job_envelope(envelope, error) == Status::InvalidInput,
              "element_count above the contract cap must be refused");
        envelope = valid_envelope("job-1");

        envelope.job_id = "";
        check(vortyx::platform::validate_job_envelope(envelope, error) == Status::InvalidInput,
              "an empty job_id must be refused");
        envelope = valid_envelope("job-1");

        envelope.requested_backend = "cuda";
        check(vortyx::platform::validate_job_envelope(envelope, error) == Status::InvalidInput &&
                  error.find("requested_backend") != std::string::npos,
              "an unknown requested_backend must be refused");
        envelope = valid_envelope("job-1");

        envelope.protocol_version = "2";
        check(vortyx::platform::validate_job_envelope(envelope, error) == Status::InvalidInput,
              "a wrong protocol version must be refused");
        envelope = valid_envelope("job-1");

        ResultEnvelope result;
        result.job_id = "job-1";
        result.status = JobStatus::Completed;
        check(vortyx::platform::validate_result_envelope(result, error) == Status::Ok,
              "a completed result without error must pass");

        result.status = JobStatus::Failed;
        result.error = "device lost";
        check(vortyx::platform::validate_result_envelope(result, error) == Status::Ok,
              "a failed result with a reason must pass");

        result.error = "";
        check(vortyx::platform::validate_result_envelope(result, error) == Status::InvalidInput &&
                  error.find("requires an error reason") != std::string::npos,
              "a failed result without a reason must be refused (failures are never hidden)");

        result.status = JobStatus::Completed;
        result.error = "residual error";
        check(vortyx::platform::validate_result_envelope(result, error) == Status::InvalidInput,
              "a completed result must not carry an error string");

        result.status = JobStatus::Cancelled;
        result.error = "";
        check(vortyx::platform::validate_result_envelope(result, error) == Status::InvalidInput,
              "cancellation is not a result outcome");
    }

    // =====================================================================
    // 5. Auth boundary: AuthN vs AuthZ
    // =====================================================================
    {
        std::string error;
        const AuthContext alice = vortyx::platform::make_authenticated("user-alice");
        const AuthContext bob = vortyx::platform::make_authenticated("user-bob");

        check(vortyx::platform::validate_auth(alice, error) == Status::Ok,
              "an authenticated context must validate");
        check(vortyx::platform::validate_auth(anonymous(), error) == Status::Unauthenticated,
              "an anonymous context must be unauthenticated");
        AuthContext broken;
        broken.authenticated = true;
        broken.user_id = "";
        check(vortyx::platform::validate_auth(broken, error) == Status::Unauthenticated,
              "a 'verified' context naming nobody must be refused");

        check(vortyx::platform::is_owner(alice, "user-alice"),
              "the subject must own its own records");
        check(!vortyx::platform::is_owner(alice, "user-bob"),
              "a subject must not own another user's records");
        check(!vortyx::platform::is_owner(anonymous(), "user-alice"),
              "an anonymous subject owns nothing");

        check(vortyx::platform::authorize_record_access(alice, "user-alice") == Status::Ok,
              "owner access must be Ok");
        check(vortyx::platform::authorize_record_access(alice, "user-bob") == Status::Forbidden,
              "foreign access must be Forbidden");
        check(vortyx::platform::authorize_record_access(anonymous(), "user-alice") ==
                  Status::Unauthenticated,
              "missing credentials must be Unauthenticated");
        (void)bob;
    }

    // =====================================================================
    // 6. Store — devices: registration, duplicates, ownership, heartbeat
    // =====================================================================
    {
        InMemoryPlatformStore store;
        const AuthContext alice = vortyx::platform::make_authenticated("user-alice");
        const AuthContext bob = vortyx::platform::make_authenticated("user-bob");

        DeviceRecord record;
        check(store.register_device(alice, "dev-1", valid_metadata(), record) == Status::Ok,
              "registration must succeed");
        check(record.owner_user_id == "user-alice",
              "owner must come from the authenticated subject, never from the client");
        check(record.status == DeviceStatus::Online && record.last_seen_ms.has_value() &&
                  record.created_at_ms.has_value(),
              "a fresh registration must be Online with server timestamps");
        check(*record.last_seen_ms == *record.created_at_ms,
              "registration stamps last_seen with the creation moment");

        DeviceMetadata bad = valid_metadata();
        bad.protocol_version = "9";
        check(store.register_device(alice, "dev-bad", bad, record) == Status::InvalidInput,
              "an unsupported protocol version must be refused at the boundary");

        check(store.register_device(anonymous(), "dev-2", valid_metadata(), record) ==
                  Status::Unauthenticated,
              "anonymous registration must be refused");

        check(store.register_device(alice, "dev-1", valid_metadata(), record) == Status::Conflict,
              "a duplicate device id must conflict");
        check(store.register_device(bob, "dev-1", valid_metadata(), record) == Status::Conflict,
              "a duplicate device id must conflict for another owner too (no info leak)");

        check(store.device(alice, "dev-1", record) == Status::Ok && record.device_id == "dev-1",
              "the owner must read its device");
        check(store.device(bob, "dev-1", record) == Status::NotFound,
              "a foreign device must be invisible (NotFound — RLS equivalence, no id leak)");
        check(store.device(alice, "dev-missing", record) == Status::NotFound,
              "a missing device must be NotFound");

        std::vector<DeviceRecord> devices;
        DeviceRecord bob_device;
        check(store.register_device(bob, "dev-bob", valid_metadata(), bob_device) == Status::Ok,
              "second user registration must succeed");
        check(store.devices(alice, devices) == Status::Ok && devices.size() == 1 &&
                  devices[0].device_id == "dev-1",
              "the device list must contain exactly the caller's devices in insertion order");

        DeviceRecord beaten;
        check(store.heartbeat_device(alice, "dev-1", beaten) == Status::Ok &&
                  beaten.status == DeviceStatus::Online &&
                  beaten.last_seen_ms.has_value() &&
                  *beaten.last_seen_ms >= *record.created_at_ms,
              "a heartbeat must mark the device Online with a fresh server timestamp");
        check(store.heartbeat_device(bob, "dev-1", beaten) == Status::NotFound,
              "a foreign heartbeat must be invisible (NotFound)");
        check(store.heartbeat_device(alice, "dev-missing", beaten) == Status::NotFound,
              "a heartbeat for a missing device must be NotFound");
    }

    // =====================================================================
    // 7. Store — jobs: submission, idempotency, device linkage, ownership
    // =====================================================================
    {
        InMemoryPlatformStore store;
        const AuthContext alice = vortyx::platform::make_authenticated("user-alice");
        const AuthContext bob = vortyx::platform::make_authenticated("user-bob");
        DeviceRecord device;
        check(store.register_device(alice, "dev-1", valid_metadata(), device) == Status::Ok,
              "device setup");

        JobRecord record;
        bool created = false;
        check(store.create_job(alice, valid_envelope("job-1"), "dev-1", record, created) ==
                      Status::Ok &&
                  created && record.status == JobStatus::Queued &&
                  record.owner_user_id == "user-alice" &&
                  record.submitted_by_device_id.has_value() &&
                  *record.submitted_by_device_id == "dev-1" &&
                  record.created_at_ms.has_value() && !record.started_at_ms.has_value(),
              "a fresh job must be Queued with owner + server timestamp + device link");

        // Idempotent replay: same id + same payload -> the existing record.
        JobRecord replay;
        bool replayed_created = true;
        check(store.create_job(alice, valid_envelope("job-1"), "dev-1", replay,
                               replayed_created) == Status::Ok &&
                  !replayed_created && replay.job.job_id == "job-1" &&
                  replay.status == JobStatus::Queued,
              "an identical resubmission must return the existing job (idempotency)");

        // Same id, different payload -> conflict.
        JobEnvelope changed = valid_envelope("job-1");
        changed.element_count = 2048;
        check(store.create_job(alice, changed, "dev-1", record, created) == Status::Conflict,
              "the same id with a different payload must conflict");
        // Same id, different owner -> conflict (no info leak about payloads).
        check(store.create_job(bob, valid_envelope("job-1"), std::nullopt, record, created) ==
                  Status::Conflict,
              "the same id from another owner must conflict");

        check(store.create_job(alice, valid_envelope("job-2"), "dev-bob", record, created) ==
                  Status::Forbidden,
              "submitting through a foreign device must be Forbidden");
        check(store.create_job(alice, valid_envelope("job-2"), "dev-unknown", record, created) ==
                  Status::Forbidden,
              "submitting through an unknown device must be Forbidden (no existence leak)");
        JobEnvelope zero = valid_envelope("job-3");
        zero.element_count = 0;
        check(store.create_job(alice, zero, std::nullopt, record, created) == Status::InvalidInput,
              "a zero-element job must be refused");

        check(store.job(alice, "job-1", record) == Status::Ok,
              "the owner must read its job");
        check(store.job(bob, "job-1", record) == Status::NotFound,
              "a foreign job must be invisible (NotFound — RLS equivalence)");
        check(store.job(alice, "job-missing", record) == Status::NotFound,
              "a missing job must be NotFound");

        JobRecord bob_job;
        check(store.create_job(bob, valid_envelope("job-bob"), std::nullopt, bob_job,
                               created) == Status::Ok,
              "second user job setup");
        std::vector<JobRecord> jobs;
        check(store.jobs(alice, jobs) == Status::Ok && jobs.size() == 1 &&
                  jobs[0].job.job_id == "job-1",
              "the job list must contain exactly the caller's jobs in submission order");
        check(store.jobs(anonymous(), jobs) == Status::Unauthenticated,
              "an anonymous job list must be refused");
    }

    // =====================================================================
    // 8. Store — job lifecycle transitions + cancellation + failure honesty
    // =====================================================================
    {
        InMemoryPlatformStore store;
        const AuthContext alice = vortyx::platform::make_authenticated("user-alice");
        DeviceRecord device;
        check(store.register_device(alice, "dev-1", valid_metadata(), device) == Status::Ok,
              "device setup");
        JobRecord record;
        bool created = false;
        check(store.create_job(alice, valid_envelope("job-1"), std::nullopt, record, created) ==
                  Status::Ok,
              "job setup");

        // Illegal: never-ran -> Completed.
        check(store.update_job(alice, "job-1", JobStatus::Completed, "", record) ==
                  Status::InvalidInput,
              "Queued -> Completed must be refused as an illegal transition");
        // Illegal: Queued -> Failed (never ran, cannot fail).
        check(store.update_job(alice, "job-1", JobStatus::Failed, "boom", record) ==
                  Status::InvalidInput,
              "Queued -> Failed must be refused as an illegal transition");
        // A Failed transition without a reason is refused (failures are
        // never hidden) — reachable only from Running, checked below.
        check(store.update_job(alice, "job-1", JobStatus::Running, "", record) == Status::Ok &&
                  record.status == JobStatus::Running && record.started_at_ms.has_value(),
              "Queued -> Running stamps started_at");
        check(store.update_job(alice, "job-1", JobStatus::Failed, "", record) ==
                  Status::InvalidInput,
              "Running -> Failed without an error reason must be refused");
        check(store.update_job(alice, "job-1", JobStatus::Failed, "executor crashed", record) ==
                      Status::Ok &&
                  record.status == JobStatus::Failed && record.error == "executor crashed" &&
                  record.completed_at_ms.has_value() && record.started_at_ms.has_value(),
              "Running -> Failed records the honest reason and stamps completed_at");
        check(vortyx::platform::job_status_is_terminal(record.status),
              "sanity: Failed is terminal");
        // Terminal states accept nothing, including cancellation.
        check(store.cancel_job(alice, "job-1", record) == Status::InvalidInput,
              "cancelling a terminal job must be refused (illegal transition)");
        check(store.update_job(alice, "job-1", JobStatus::Completed, "", record) ==
                  Status::InvalidInput,
              "reviving a terminal job must be refused");

        // Cancellation path on a fresh job.
        check(store.create_job(alice, valid_envelope("job-2"), std::nullopt, record, created) ==
                  Status::Ok,
              "second job setup");
        check(store.cancel_job(alice, "job-2", record) == Status::Ok &&
                  record.status == JobStatus::Cancelled && record.completed_at_ms.has_value() &&
                  record.error == "cancelled",
              "owner cancellation must record the cancelled state + reason");
        check(store.job(alice, "job-2", record) == Status::Ok &&
                  record.status == JobStatus::Cancelled,
              "the cancelled state must persist");

        // Ownership on updates.
        const AuthContext bob = vortyx::platform::make_authenticated("user-bob");
        check(store.create_job(alice, valid_envelope("job-3"), std::nullopt, record, created) ==
                  Status::Ok,
              "third job setup");
        check(store.update_job(bob, "job-3", JobStatus::Running, "", record) == Status::NotFound,
              "a foreign update must be invisible (NotFound)");
        check(store.cancel_job(bob, "job-3", record) == Status::NotFound,
              "a foreign cancellation must be invisible (NotFound)");
        check(store.update_job(anonymous(), "job-3", JobStatus::Running, "", record) ==
                  Status::Unauthenticated,
              "an anonymous update must be Unauthenticated");
        check(store.update_job(alice, "job-missing", JobStatus::Running, "", record) ==
                  Status::NotFound,
              "updating a missing job must be NotFound");
    }

    // =====================================================================
    // 9. Store — results: recording, honesty rules, single result
    // =====================================================================
    {
        InMemoryPlatformStore store;
        const AuthContext alice = vortyx::platform::make_authenticated("user-alice");
        const AuthContext bob = vortyx::platform::make_authenticated("user-bob");
        JobRecord record;
        bool created = false;
        check(store.create_job(alice, valid_envelope("job-1"), std::nullopt, record, created) ==
                  Status::Ok,
              "job setup");

        ResultEnvelope result;
        result.job_id = "job-1";
        result.status = JobStatus::Completed;
        result.result_element_count = 1024;
        ResultEnvelope stored;
        check(store.put_result(alice, result, stored) == Status::InvalidInput,
              "a result for a never-started job must be refused");
        check(store.update_job(alice, "job-1", JobStatus::Running, "", record) == Status::Ok,
              "start the job");

        result.backend = "cpu";
        check(store.put_result(alice, result, stored) == Status::Ok &&
                  stored.status == JobStatus::Completed && stored.backend == "cpu" &&
                  stored.result_element_count.has_value() &&
                  *stored.result_element_count == 1024,
              "a running job's completion must be recordable");
        check(store.job(alice, "job-1", record) == Status::Ok &&
                  record.status == JobStatus::Completed &&
                  record.completed_at_ms.has_value() && record.error.empty(),
              "recording the result must complete the job honestly");

        ResultEnvelope again = result;
        check(store.put_result(alice, again, stored) == Status::Conflict,
              "a second result must conflict (single outcome per job)");

        ResultEnvelope fetched;
        check(store.result(alice, "job-1", fetched) == Status::Ok &&
                  fetched.job_id == "job-1" && fetched.status == JobStatus::Completed,
              "the owner must fetch the stored result");
        check(store.result(alice, "job-no-result", fetched) == Status::NotFound,
              "a missing job's result must be NotFound");

        // Failed path with a mandatory reason.
        check(store.create_job(alice, valid_envelope("job-2"), std::nullopt, record, created) ==
                  Status::Ok,
              "second job setup");
        check(store.update_job(alice, "job-2", JobStatus::Running, "", record) == Status::Ok,
              "start the second job");
        ResultEnvelope failure;
        failure.job_id = "job-2";
        failure.status = JobStatus::Failed;
        failure.backend = "vulkan";
        failure.error = "";  // no reason -> refused at the store boundary too
        check(store.put_result(alice, failure, stored) == Status::InvalidInput,
              "a failure result without a reason must be refused");
        failure.error = "vulkan device lost";
        check(store.put_result(alice, failure, stored) == Status::Ok &&
                  store.job(alice, "job-2", record) == Status::Ok &&
                  record.status == JobStatus::Failed && record.error == "vulkan device lost",
              "a failure result must carry its reason into the job record");

        // Ownership rules.
        check(store.result(bob, "job-1", fetched) == Status::NotFound,
              "a foreign result must be invisible (NotFound)");
        check(store.put_result(bob, result, stored) == Status::NotFound,
              "a foreign result recording must be invisible (NotFound)");

        // Cancellation is an owner action, not a result outcome.
        check(store.create_job(alice, valid_envelope("job-3"), std::nullopt, record, created) ==
                  Status::Ok,
              "third job setup");
        check(store.update_job(alice, "job-3", JobStatus::Running, "", record) == Status::Ok,
              "start the third job");
        ResultEnvelope cancel_as_result;
        cancel_as_result.job_id = "job-3";
        cancel_as_result.status = JobStatus::Cancelled;
        check(store.put_result(alice, cancel_as_result, stored) == Status::InvalidInput,
              "cancellation must not be recordable as a result");
    }

    // =====================================================================
    // 10. Store — concurrent use is safe (the local/mock store's thread
    //     contract; the real backend gets the same guarantees from its DB)
    // =====================================================================
    {
        InMemoryPlatformStore store;
        const AuthContext alice = vortyx::platform::make_authenticated("user-alice");
        constexpr int kThreads = 4;
        constexpr int kPerThread = 25;

        {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&store, &alice, t, kPerThread] {
                    for (int i = 0; i < kPerThread; ++i) {
                        DeviceRecord device;
                        const DeviceId id = "dev-" + std::to_string(t) + "-" + std::to_string(i);
                        const Status status = store.register_device(alice, id, valid_metadata(),
                                                                    device);
                        if (status != Status::Ok) {
                            std::cerr << "FAIL: concurrent registration failed\n";
                            ++failures;
                        }
                    }
                });
            }
            for (std::thread& thread : threads) thread.join();
        }
        std::vector<DeviceRecord> devices;
        check(store.devices(alice, devices) == Status::Ok &&
                  devices.size() == kThreads * kPerThread,
              "all concurrent registrations must land exactly once");

        // Racing registrations of the SAME id: exactly one winner, the rest
        // must observe a conflict — never a duplicate record.
        {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&store, &alice] {
                    DeviceRecord device;
                    const Status status =
                        store.register_device(alice, "dev-race", valid_metadata(), device);
                    if (status != Status::Ok && status != Status::Conflict) {
                        std::cerr << "FAIL: concurrent same-id registration produced " +
                                         std::string(vortyx::platform::to_string(status)) + "\n";
                        ++failures;
                    }
                });
            }
            for (std::thread& thread : threads) thread.join();
        }
        check(store.devices(alice, devices) == Status::Ok &&
                  devices.size() == kThreads * kPerThread + 1,
              "same-id races must produce exactly one registration");
    }

    if (failures == 0) {
        std::cout << "Platform layer tests passed.\n";
        return 0;
    }
    std::cerr << failures << " platform check(s) FAILED.\n";
    return 1;
}
