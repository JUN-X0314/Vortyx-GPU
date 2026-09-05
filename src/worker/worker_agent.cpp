// The native worker agent (Phase 15) — implementation.

#include "worker/worker_agent.hpp"

#include <chrono>
#include <thread>

#include "platform/status.hpp"

namespace vortyx::worker {

using vortyx::platform::Status;

namespace {

vortyx::compute::ComputeOp parse_operation(const std::string& label, std::string& error) {
    if (label == "vector_add") return vortyx::compute::ComputeOp::VectorAdd;
    if (label == "vector_multiply") return vortyx::compute::ComputeOp::VectorMultiply;
    if (label == "vector_scale") return vortyx::compute::ComputeOp::VectorScale;
    error = "unknown operation label: " + label;
    return vortyx::compute::ComputeOp::VectorAdd;
}

// Maps a terminal DistributedJobStatus to the wire terminal status.
std::string wire_terminal_status(vortyx::distributed::DistributedJobStatus status) {
    switch (status) {
        case vortyx::distributed::DistributedJobStatus::Completed:
            return kTerminalCompleted;
        case vortyx::distributed::DistributedJobStatus::Failed:
            return kTerminalFailed;
        case vortyx::distributed::DistributedJobStatus::Cancelled:
            return kTerminalCancelled;
        default:
            return "";  // never reported (the agent only reports terminal records)
    }
}

}  // namespace

Status WorkerAgent::create(IWorkerApiTransport* transport, INativeExecutor* executor,
                           const WorkerAgentConfig& config, std::unique_ptr<WorkerAgent>& out,
                           std::string& error) {
    if (transport == nullptr || executor == nullptr) {
        error = "the agent requires a transport and an executor";
        return Status::InvalidInput;
    }
    if (config.worker_id.empty()) {
        error = "worker_id is required (lease ownership is keyed by it)";
        return Status::InvalidInput;
    }
    if (config.poll_interval_ms <= 0 || config.heartbeat_interval_ms <= 0 ||
        config.lease_ms <= 0) {
        error = "poll/heartbeat/lease intervals must be positive";
        return Status::InvalidInput;
    }
    // A lease shorter than two heartbeat intervals expires while the agent
    // still runs — a misconfiguration, refused up front.
    if (config.lease_ms < 2 * config.heartbeat_interval_ms) {
        error = "lease_ms must be at least twice heartbeat_interval_ms";
        return Status::InvalidInput;
    }

    std::unique_ptr<WorkerAgent> agent(new WorkerAgent());
    agent->transport_ = transport;
    agent->executor_ = executor;
    agent->config_ = config;
    out = std::move(agent);
    return Status::Ok;
}

WorkerAgent::~WorkerAgent() { stop_heartbeat(); }

void WorkerAgent::stop_heartbeat() {
    heartbeat_stop_.store(true, std::memory_order_relaxed);
}

void WorkerAgent::heartbeat_loop(const std::string& job_id) {
    // First beat fires immediately: a long execution must not wait one full
    // interval before its lease is first renewed/observed.
    std::int64_t interval_ms = 0;
    while (!heartbeat_stop_.load(std::memory_order_relaxed)) {
        if (interval_ms > 0) {
            // Sleep in small slices so a stop request is honored promptly.
            std::int64_t slept = 0;
            while (slept < interval_ms &&
                   !heartbeat_stop_.load(std::memory_order_relaxed)) {
                const std::int64_t slice =
                    interval_ms - slept > 20 ? 20 : interval_ms - slept;
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                slept += slice;
            }
            if (heartbeat_stop_.load(std::memory_order_relaxed)) break;
        }

        const std::string body = encode_heartbeat_request(config_.worker_id);
        const IWorkerApiTransport::Response response =
            transport_->post("/api/worker/jobs/" + job_id + "/heartbeat", body);
        if (response.ok && response.status == 200) {
            HeartbeatResponse heartbeat;
            parse_heartbeat_response(response.body, heartbeat);
            if (heartbeat.ok && heartbeat.accepted && heartbeat.cancel_requested) {
                // Relay the control plane's cancellation into the
                // executing record (the orchestrator's flag — the same
                // mechanism a local owner cancel uses).
                cancel_observed_.store(true, std::memory_order_relaxed);
                (void)executor_->request_cancel(job_id);
            }
            interval_ms = config_.heartbeat_interval_ms;
        } else if (response.ok && (response.status == 404 || response.status == 409)) {
            // The lease is gone (expired/reclaimed) or the job is terminal:
            // the orchestrator's own record decides the outcome; keep
            // executing and let the report step learn the truth.
            interval_ms = config_.heartbeat_interval_ms;
        } else {
            // Transient fault: keep the cadence, never crash the execution.
            interval_ms = config_.heartbeat_interval_ms;
        }
    }
}

WorkerAgent::CycleResult WorkerAgent::run_cycle(std::string& detail) {
    detail.clear();

    // ---- 1. claim ----------------------------------------------------------
    ClaimRequest claim;
    claim.worker_id = config_.worker_id;
    claim.lease_ms = config_.lease_ms;
    const IWorkerApiTransport::Response claim_response =
        transport_->post("/api/worker/claim", encode_claim_request(claim));
    if (!claim_response.ok) {
        detail = "claim transport failure: " + claim_response.error;
        return CycleResult::Error;
    }

    ClaimResponse claim_parsed;
    parse_claim_response(claim_response.body, claim_parsed);
    if (!claim_parsed.ok) {
        detail = "claim refused (" + claim_parsed.error_code + "): " +
                 claim_parsed.error_message;
        return CycleResult::Error;
    }
    if (!claim_parsed.claimed) {
        detail = "no queued work";
        return CycleResult::NoWork;
    }
    const ClaimedJob job = claim_parsed.job;

    // ---- 2. execute on the real stack --------------------------------------
    vortyx::distributed::DistributedJobRequest request;
    request.envelope.job_id = job.job_id;
    std::string op_error;
    request.envelope.operation = parse_operation(job.operation, op_error);
    if (!op_error.empty()) {
        // The control plane sent an operation this worker cannot name.
        // Honest failure report — never a silent drop, never a fake success.
        CompletionReport report;
        report.terminal_status = kTerminalFailed;
        report.error = "unsupported operation: " + job.operation;
        std::string encode_error;
        const std::string body =
            encode_complete_request(config_.worker_id, report, encode_error);
        if (!body.empty()) {
            (void)transport_->post("/api/worker/jobs/" + job.job_id + "/fail", body);
            last_reported_status_ = kTerminalFailed;
        }
        detail = "claimed job refused (unsupported operation) and reported failed";
        return CycleResult::Claimed;
    }
    request.envelope.element_count = job.element_count;
    request.envelope.requested_backend = job.requested_backend;
    request.envelope.priority = 0;  // reserved field, no semantics (Phase 11 rule)
    request.envelope.protocol_version = vortyx::platform::kProtocolVersion;
    request.envelope.created_at_ms = std::nullopt;
    request.requested_shard_count = job.requested_shard_count;

    heartbeat_stop_.store(false, std::memory_order_relaxed);
    cancel_observed_.store(false, std::memory_order_relaxed);
    std::thread heartbeater([this, job_id = job.job_id]() { heartbeat_loop(job_id); });

    vortyx::distributed::DistributedJobRecord record;
    const Status execute_status = executor_->execute(request, record);

    stop_heartbeat();
    heartbeater.join();

    // ---- 3. report the terminal outcome ------------------------------------
    const std::string wire_status = wire_terminal_status(record.status);
    if (execute_status == Status::Ok && !wire_status.empty()) {
        CompletionReport report;
        report.terminal_status = wire_status;
        // A cancelled report always carries its reason (the protocol rule):
        // the orchestrator's own Cancelled record may legitimately carry an
        // empty error (cancellation is not a failure), so the wire reason
        // falls back to the status word — never an invented story.
        report.error = record.error.empty() && wire_status == kTerminalCancelled
                           ? std::string("cancelled")
                           : record.error;
        report.backend = record.result.backends_used.empty()
                             ? std::string()
                             : record.result.backends_used.front();
        const std::uint64_t elements = record.result.data.size();
        if (elements > 0) {
            report.has_result_element_count = true;
            report.result_element_count = elements;
        }
        // The real shard summary (the orchestrator record's counts).
        report.has_shard_summary = true;
        report.shards_total = static_cast<std::uint32_t>(record.shards.size());
        report.shards_succeeded = record.result.succeeded;
        report.shards_failed = record.result.failed;
        std::string encode_error;
        const std::string body =
            encode_complete_request(config_.worker_id, report, encode_error);
        if (body.empty()) {
            detail = "terminal record could not be encoded: " + encode_error +
                     " (the job stays in the control plane's reconciliation)";
            return CycleResult::Error;
        }
        const IWorkerApiTransport::Response report_response =
            transport_->post("/api/worker/jobs/" + job.job_id + "/complete", body);
        if (!report_response.ok) {
            detail = "report transport failure: " + report_response.error +
                     " (the job stays in the control plane's reconciliation)";
            return CycleResult::Error;
        }
        CompleteResponse report_parsed;
        parse_complete_response(report_response.body, report_parsed);
        if (!report_parsed.ok) {
            detail = "report refused (" + report_parsed.error_code + "): " +
                     report_parsed.error_message +
                     " (the job stays in the control plane's reconciliation)";
            return CycleResult::Error;
        }
        last_reported_status_ = wire_status;
        detail = "job " + job.job_id + " -> " + wire_status +
                 (cancel_observed_.load(std::memory_order_relaxed) ? " (cancel observed)"
                                                                   : "") +
                 ", recorded=" + (report_parsed.recorded ? "true" : "false");
        return CycleResult::Claimed;
    }

    // The executor refused BEFORE executing (validation/conflict). Report
    // the honest failure; the record was never terminal so the wire status
    // is 'failed' with the reason.
    CompletionReport report;
    report.terminal_status = kTerminalFailed;
    report.error = "executor refused the claimed job: " + record.error;
    std::string encode_error;
    const std::string body = encode_complete_request(config_.worker_id, report, encode_error);
    if (!body.empty()) {
        (void)transport_->post("/api/worker/jobs/" + job.job_id + "/fail", body);
        last_reported_status_ = kTerminalFailed;
        detail = "claimed job failed before execution and was reported";
        return CycleResult::Claimed;
    }
    detail = "failure report could not be encoded: " + encode_error;
    return CycleResult::Error;
}

}  // namespace vortyx::worker
