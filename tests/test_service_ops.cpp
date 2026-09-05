// Service observability tests (Phase 14) — audit, metrics, health,
// artifacts and the JSON contract.
//
// Covered: audit event structure + bounds + scope, honest health values
// (a NotConfigured provider is never "healthy", device health is
// caller-scoped), artifact metadata (no payload storage exists), the
// metrics counters, and the serialization schemas (deterministic JSON in
// the strict platform subset).

#include <iostream>
#include <string>

#include "platform/json.hpp"
#include "service/service.hpp"

using namespace vortyx::service;
using vortyx::distributed::FakeClock;
using vortyx::platform::AuthContext;
using vortyx::platform::make_authenticated;
using vortyx::platform::parse_json;
using vortyx::platform::JsonValue;
using SS = ServiceStatus;

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
    // =====================================================================
    // 1. Audit trail: structure, uniqueness, bounds, scope
    // =====================================================================
    {
        auto store = std::make_shared<InMemoryAuditStore>(8);  // tiny ring
        auto clock = std::make_shared<FakeClock>(5000);
        AuditTrail trail(store, clock);

        trail.record("user-a", "proj-1", "job-1", AuditAction::JobSubmit, AuditOutcome::Ok, "");
        clock->advance(10);
        trail.record("user-a", "proj-1", "job-1", AuditAction::JobTerminal, AuditOutcome::Ok, "");
        trail.record("user-b", "proj-1", "job-2", AuditAction::JobSubmit, AuditOutcome::Denied,
                     "quota_exceeded");

        std::vector<AuditEvent> tail = store->tail(10);
        check(tail.size() == 3, "three events recorded");
        check(tail[0].timestamp_ms == 5000 && tail[1].timestamp_ms == 5010,
              "timestamps from the injected clock");
        check(tail[0].event_id != tail[1].event_id && tail[1].event_id != tail[2].event_id,
              "event ids are unique");
        check(std::string(to_string(tail[2].action)) == "job_submit" &&
                  std::string(to_string(tail[2].outcome)) == "denied" &&
                  tail[2].reason_code == "quota_exceeded",
              "the refusal is recorded with its code");

        // Bounded: appending past the cap drops the OLDEST and counts drops.
        for (int i = 0; i < 10; ++i) {
            trail.record("user-a", "", "", AuditAction::AdminAction, AuditOutcome::Ok, "");
        }
        check(store->size() == 8, "the ring stays bounded");
        check(store->dropped_total() > 0, "drops are counted honestly");
        tail = store->tail(1);
        check(std::string(to_string(tail[0].action)) == "admin_action",
              "the newest event survives");

        // Secret-free by construction: the event structure has no field a
        // secret could ride in — verify the shape stays small.
        check(tail[0].actor_user_id.size() <= 128 && tail[0].reason_code.size() <= 128,
              "no unbounded payload fields in audit events");
    }

    // =====================================================================
    // 2. Metrics: real counters only
    // =====================================================================
    {
        ServiceMetrics metrics;
        metrics.inc_submit_attempts();
        metrics.inc_submit_attempts();
        metrics.inc_jobs_submitted();
        metrics.inc_jobs_replayed();
        metrics.inc_jobs_completed();
        metrics.inc_quota_rejections();
        metrics.inc_rate_limit_rejections();
        metrics.inc_jobs_running();
        metrics.inc_jobs_running();
        metrics.set_jobs_queued(3);
        const ServiceMetricsSnapshot snapshot = metrics.snapshot();
        check(snapshot.submit_attempts == 2 && snapshot.jobs_submitted == 1 &&
                  snapshot.jobs_replayed == 1 && snapshot.jobs_completed == 1 &&
                  snapshot.quota_rejections == 1 && snapshot.rate_limit_rejections == 1,
              "counters reflect the calls");
        check(snapshot.jobs_running == 2 && snapshot.jobs_queued == 3, "gauges readable");
        metrics.dec_jobs_running();
        check(metrics.snapshot().jobs_running == 1, "running gauge decrements");
    }

    // =====================================================================
    // 3. Health: honest values, caller-scoped device aggregates
    // =====================================================================
    {
        PlatformServiceConfig config;
        config.rate_limit_enabled = false;
        PlatformService::Deps deps;
        auto clock = std::make_shared<FakeClock>(1000);
        vortyx::distributed::LocalDeviceRegistry registry(clock);
        vortyx::distributed::LocalInProcessTransport transport;
        deps.registry = &registry;
        deps.transport = &transport;
        deps.clock = clock;
        // NOTE: no platform_store attached on purpose (the NotConfigured case).

        std::unique_ptr<PlatformService> service;
        std::string error;
        check(PlatformService::create(deps, config, service, error) == SS::Ok,
              "health fixture service created");

        const AuthContext auth = make_authenticated("user-h");
        HealthReport report = service->health_check(auth);
        check(report.checked_at_ms == 1000, "health stamped by the injected clock");
        bool has_service = false;
        bool has_queue = false;
        bool has_scheduler = false;
        bool has_store = false;
        for (const ComponentHealth& component : report.components) {
            if (component.component == "service") has_service = true;
            if (component.component == "queue") has_queue = true;
            if (component.component == "scheduler") has_scheduler = true;
            if (component.component == "platform_store") {
                has_store = true;
                check(component.status == HealthValue::NotConfigured,
                      "an unattached store is NOT healthy (not_configured)");
            }
        }
        check(has_service && has_queue && has_scheduler && has_store,
              "every component reports");
        check(report.devices.total == 0 && report.devices.healthy == 0,
              "no devices claimed for a user with none");
        check(report.overall() != HealthValue::Healthy,
              "overall is not healthy while a component is not configured");

        // Register + activate + heartbeat a device through the service:
        // the caller's aggregate must reflect it.
        vortyx::distributed::DeviceCapabilities caps;
        caps.metadata.protocol_version = vortyx::platform::kProtocolVersion;
        caps.metadata.software_version = "0.15.0";
        caps.metadata.backends = {"cpu"};
        caps.metadata.operations = {"vector_add"};
        caps.metadata.display_name = "h";
        caps.capacity.memory_bytes = 1024 * 1024;
        caps.capacity.concurrent_jobs = 1;
        caps.max_concurrent_shards = 1;
        bool created = false;
        check(service->register_device(auth, "dev-h", caps, created) == SS::Ok, "device added");
        check(service->set_device_state(auth, "dev-h", vortyx::distributed::DeviceState::Ready) ==
                  SS::Ok,
              "device activated");
        check(service->heartbeat_device(auth, "dev-h") == SS::Ok, "device heartbeaten");
        report = service->health_check(auth);
        check(report.devices.total == 1 && report.devices.healthy == 1,
              "the caller's own device counts");
        // A different user sees none of it (ownership-scoped health).
        HealthReport other_report =
            service->health_check(make_authenticated("user-other"));
        check(other_report.devices.total == 0, "device health is caller-scoped");

        // The report serializes to strict JSON and parses back.
        const std::string text = report.serialize();
        JsonValue parsed;
        std::string parse_error;
        check(parse_json(text, parsed, parse_error), "the health report is valid strict JSON");
        check(parsed.is_object() && parsed.find("overall") != nullptr &&
                  std::string(parsed.find("overall")->as_string()) ==
                      to_string(report.overall()),
              "the overall value round-trips");
    }

    // =====================================================================
    // 4. Artifacts: metadata only, no payload storage exists
    // =====================================================================
    {
        InMemoryArtifactStore store;
        ArtifactMetadata artifact;
        artifact.project_id = "proj-a";
        artifact.name = "weights";
        artifact.created_by = "user-a";
        artifact.declared_byte_size = 1234;
        ArtifactMetadata stored;
        check(store.register_artifact(artifact, stored) == SS::Ok, "artifact registered");
        check(!stored.artifact_id.empty(), "the store generates the id");
        check(stored.created_at_ms == 0, "no clock injected -> epoch stamp (honest default)");

        ArtifactMetadata fetched;
        check(store.artifact(stored.artifact_id, fetched) == SS::Ok, "artifact fetched");
        check(fetched.name == "weights" && fetched.declared_byte_size == 1234,
              "metadata round-trips");
        check(store.artifact("nope", fetched) == SS::NotFound, "unknown artifact NotFound");

        std::vector<ArtifactMetadata> list;
        check(store.artifacts("proj-a", list) == SS::Ok && list.size() == 1, "listed per project");

        // Validation: names and sizes.
        ArtifactMetadata bad;
        bad.name = "";
        check(store.register_artifact(bad, stored) == SS::InvalidInput, "empty name refused");
        bad.name = std::string(kMaxProjectNameLength + 1, 'x');
        check(store.register_artifact(bad, stored) == SS::InvalidInput, "overlong name refused");
        bad.name = "ok";
        bad.declared_byte_size = -1;
        check(store.register_artifact(bad, stored) == SS::InvalidInput,
              "negative declared size refused");
    }

    // =====================================================================
    // 5. Contract serialization: deterministic, strict-JSON, error codes
    // =====================================================================
    {
        // Status codes: stable + parseable round trip.
        bool codes_round_trip = true;
        for (const SS status : {SS::Ok, SS::InvalidInput, SS::Unauthenticated, SS::Forbidden,
                                SS::NotFound, SS::Conflict, SS::QuotaExceeded,
                                SS::RateLimitExceeded, SS::UnsupportedOperation, SS::Unavailable,
                                SS::Internal}) {
            ServiceStatus parsed_status;
            codes_round_trip = codes_round_trip &&
                               service_status_from_code(service_status_code(status),
                                                        parsed_status) &&
                               parsed_status == status;
        }
        check(codes_round_trip, "every status code round-trips");
        check(service_status_http(SS::QuotaExceeded) == 429 &&
                  service_status_http(SS::RateLimitExceeded) == 429 &&
                  service_status_http(SS::Unauthenticated) == 401 &&
                  service_status_http(SS::Forbidden) == 403 &&
                  service_status_http(SS::NotFound) == 404 &&
                  service_status_http(SS::Conflict) == 409,
              "HTTP mapping pinned");
        check(service_status_from_platform(vortyx::platform::Status::NotFound) == SS::NotFound &&
                  service_status_from_platform(vortyx::platform::Status::Ok) == SS::Ok,
              "platform mapping pinned");

        // Project JSON.
        ProjectRecord project;
        project.project_id = "proj-1";
        project.owner_user_id = "user-1";
        project.name = "alpha";
        project.status = ProjectStatus::Active;
        project.created_at_ms = 10;
        project.updated_at_ms = 20;
        const std::string project_json = serialize_project(project);
        JsonValue parsed_project;
        std::string error;
        check(parse_json(project_json, parsed_project, error), "project JSON valid");
        check(project_json == serialize_project(project), "project JSON deterministic");
        check(parsed_project.find("owner_user_id") != nullptr &&
                  std::string(parsed_project.find("owner_user_id")->as_string()) == "user-1",
              "project fields present");

        // Job JSON (optional fields are null, never fake zeros).
        ServiceJobView job;
        job.job_id = "job-1";
        job.project_id = "proj-1";
        job.submitted_by = "user-1";
        job.envelope.operation = vortyx::compute::ComputeOp::VectorAdd;
        job.envelope.element_count = 100;
        job.requested_shard_count = 2;
        job.status = vortyx::distributed::DistributedJobStatus::Queued;
        job.submitted_at_ms = 30;
        const std::string running_json = serialize_service_job(job);
        check(running_json.find("\"terminal_at_ms\":null") != std::string::npos,
              "not-terminal job carries null (not a fake 0)");
        job.status = vortyx::distributed::DistributedJobStatus::Completed;
        job.terminal_at_ms = 40;
        job.total_shards = 2;
        job.succeeded_shards = 2;
        job.failed_shards = 0;
        const std::string done_json = serialize_service_job(job);
        check(done_json.find("\"terminal_at_ms\":40") != std::string::npos &&
                  done_json.find("\"succeeded_shards\":2") != std::string::npos,
              "terminal counts serialized");
        JsonValue parsed_job;
        check(parse_json(done_json, parsed_job, error), "job JSON valid");

        // Error body.
        const std::string error_json = serialize_service_error(SS::QuotaExceeded, "too many jobs");
        check(error_json.find("\"code\":\"quota_exceeded\"") != std::string::npos,
              "the error code is the stable snake_case one");
        JsonValue parsed_error;
        check(parse_json(error_json, parsed_error, error), "error JSON valid");

        // Metrics JSON.
        ServiceMetricsSnapshot snapshot;
        snapshot.jobs_completed = 3;
        const std::string metrics_json = serialize_metrics(snapshot);
        JsonValue parsed_metrics;
        check(parse_json(metrics_json, parsed_metrics, error), "metrics JSON valid");
        check(metrics_json.find("\"jobs_completed\":3") != std::string::npos,
              "metrics fields serialized");
    }

    if (failures == 0) {
        std::cout << "Service observability tests passed.\n";
        return 0;
    }
    std::cerr << failures << " service ops test(s) failed.\n";
    return 1;
}
