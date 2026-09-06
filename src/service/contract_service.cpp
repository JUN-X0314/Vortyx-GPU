// Service contract serialization (Phase 14) — implementation.

#include "service/contract_service.hpp"

#include "distributed/job.hpp"   // the reused job-status vocabulary
#include "platform/json.hpp"     // the strict JSON writer

namespace vortyx::service {

using vortyx::platform::JsonValue;

std::string serialize_project(const ProjectRecord& project) {
    JsonValue root = JsonValue::make_object();
    root.add("project_id", JsonValue::make_string(project.project_id));
    root.add("owner_user_id", JsonValue::make_string(project.owner_user_id));
    root.add("name", JsonValue::make_string(project.name));
    root.add("status", JsonValue::make_string(to_string(project.status)));
    root.add("created_at_ms", JsonValue::make_number(static_cast<double>(project.created_at_ms)));
    root.add("updated_at_ms", JsonValue::make_number(static_cast<double>(project.updated_at_ms)));
    return root.serialize();
}

std::string serialize_service_job(const ServiceJobView& job) {
    JsonValue root = JsonValue::make_object();
    root.add("job_id", JsonValue::make_string(job.job_id));
    root.add("project_id", JsonValue::make_string(job.project_id));
    root.add("submitted_by", JsonValue::make_string(job.submitted_by));
    root.add("operation", JsonValue::make_string(
                              vortyx::compute::workload_label(job.envelope.operation)));
    root.add("element_count",
             JsonValue::make_number(static_cast<double>(job.envelope.element_count)));
    root.add("requested_shard_count",
             JsonValue::make_number(static_cast<double>(job.requested_shard_count)));
    root.add("requested_backend", JsonValue::make_string(job.envelope.requested_backend));
    root.add("status",
             JsonValue::make_string(vortyx::distributed::to_string(job.status)));
    root.add("error", JsonValue::make_string(job.error));
    root.add("submitted_at_ms",
             JsonValue::make_number(static_cast<double>(job.submitted_at_ms)));
    // Optional fields are null when not set (never a fake 0).
    if (job.terminal_at_ms > 0) {
        root.add("terminal_at_ms", JsonValue::make_number(static_cast<double>(job.terminal_at_ms)));
    } else {
        root.add("terminal_at_ms", JsonValue::make_null());
    }
    if (job.total_shards >= 0) {
        root.add("total_shards", JsonValue::make_number(static_cast<double>(job.total_shards)));
        root.add("succeeded_shards",
                 JsonValue::make_number(static_cast<double>(job.succeeded_shards)));
        root.add("failed_shards",
                 JsonValue::make_number(static_cast<double>(job.failed_shards)));
    } else {
        root.add("total_shards", JsonValue::make_null());
        root.add("succeeded_shards", JsonValue::make_null());
        root.add("failed_shards", JsonValue::make_null());
    }
    // Phase 16 (optional, additive): the fabric plan summary — a nullable
    // object. Absent (null) when the job was never fabric-planned; the
    // web console renders that honestly as "not available".
    if (job.plan_available) {
        JsonValue plan = JsonValue::make_object();
        plan.add("plan_version",
                 JsonValue::make_number(static_cast<double>(job.plan.plan_version)));
        plan.add("planner", JsonValue::make_string(job.plan.planner_name));
        plan.add("planner_version", JsonValue::make_string(job.plan.planner_version));
        plan.add("cluster_revision",
                 JsonValue::make_number(static_cast<double>(job.plan.cluster_revision)));
        JsonValue devices = JsonValue::make_array();
        for (const std::string& device : job.plan.devices) {
            devices.push(JsonValue::make_string(device));
        }
        plan.add("devices", std::move(devices));
        plan.add("reason", JsonValue::make_string(job.plan.reason_summary));
        root.add("plan", std::move(plan));
    } else {
        root.add("plan", JsonValue::make_null());
    }
    return root.serialize();
}

std::string serialize_service_error(ServiceStatus status, const std::string& message) {
    JsonValue error = JsonValue::make_object();
    error.add("code", JsonValue::make_string(service_status_code(status)));
    error.add("message", JsonValue::make_string(message));
    JsonValue root = JsonValue::make_object();
    root.add("error", std::move(error));
    return root.serialize();
}

std::string serialize_metrics(const ServiceMetricsSnapshot& metrics) {
    JsonValue root = JsonValue::make_object();
    root.add("submit_attempts",
             JsonValue::make_number(static_cast<double>(metrics.submit_attempts)));
    root.add("jobs_submitted", JsonValue::make_number(static_cast<double>(metrics.jobs_submitted)));
    root.add("jobs_replayed", JsonValue::make_number(static_cast<double>(metrics.jobs_replayed)));
    root.add("jobs_completed", JsonValue::make_number(static_cast<double>(metrics.jobs_completed)));
    root.add("jobs_failed", JsonValue::make_number(static_cast<double>(metrics.jobs_failed)));
    root.add("jobs_cancelled", JsonValue::make_number(static_cast<double>(metrics.jobs_cancelled)));
    root.add("quota_rejections",
             JsonValue::make_number(static_cast<double>(metrics.quota_rejections)));
    root.add("rate_limit_rejections",
             JsonValue::make_number(static_cast<double>(metrics.rate_limit_rejections)));
    root.add("jobs_queued", JsonValue::make_number(static_cast<double>(metrics.jobs_queued)));
    root.add("jobs_running", JsonValue::make_number(static_cast<double>(metrics.jobs_running)));
    return root.serialize();
}

}  // namespace vortyx::service
