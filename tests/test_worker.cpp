// Phase 15 worker tests — the native execution boundary.
//
// Sections:
//   1. Payload synthesis: determinism (same job -> same bytes, different
//      job -> different bytes), strict operand policy per op, honest
//      refusals (zero/oversized element counts).
//   2. The worker protocol codec: strict parsing (unknown fields, missing
//      fields, wrong types are refusals), the unified error body, complete
//      request validation.
//   3. SimulatorNativeExecutor: a REAL end-to-end distributed execution
//      (2 devices, 2 shards) verified bit-exact against the host
//      reference; post-terminal cancellation refused honestly.
//   4. The agent loop over a scripted transport: claim -> execute ->
//      report, the cancel relay, no-work, refused claims, honest failure
//      reporting (unsupported operation), and terminal-record-driven
//      reporting (never a fabricated success).
//   5. HTTP transport configuration: honest endpoint refusals (no network).

#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "distributed/distributed.hpp"
#include "platform/platform.hpp"
#include "worker/http_transport.hpp"
#include "worker/native_executor.hpp"
#include "worker/worker_agent.hpp"
#include "worker/worker_protocol.hpp"

using namespace vortyx::worker;
using vortyx::platform::Status;
using Op = vortyx::compute::ComputeOp;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// ---------------------------------------------------------------------------
// Scripted transport: canned responses per path prefix, every request
// recorded (the deterministic stand-in for HTTP).
// ---------------------------------------------------------------------------

class ScriptedTransport final : public IWorkerApiTransport {
public:
    struct ScriptedResponse {
        int status = 200;
        std::string body;
    };

    IWorkerApiTransport::Response post(const std::string& path,
                                       const std::string& body) override {
        std::lock_guard<std::mutex> lock(mutex_);
        posts_.emplace_back(path, body);
        const auto it = post_scripts_.find(path);
        if (it == post_scripts_.end()) {
            IWorkerApiTransport::Response response;
            response.ok = false;
            response.error = "no script for " + path;
            return response;
        }
        const ScriptedResponse scripted = it->second();
        IWorkerApiTransport::Response response;
        response.ok = true;
        response.status = scripted.status;
        response.body = scripted.body;
        return response;
    }

    IWorkerApiTransport::Response get(const std::string& path) override {
        (void)path;
        IWorkerApiTransport::Response response;
        response.ok = true;
        response.status = 200;
        response.body = "{}";
        return response;
    }

    // Scripts the NEXT response for a path prefix.
    void script(const std::string& path_prefix,
                std::function<ScriptedResponse()> factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        post_scripts_[path_prefix] = std::move(factory);
    }

    std::vector<std::pair<std::string, std::string>> posts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return posts_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::function<ScriptedResponse()>> post_scripts_;
    std::vector<std::pair<std::string, std::string>> posts_;
};

// A scripted executor whose execute() blocks until request_cancel is
// invoked (the deterministic cancel-relay probe) and then reports a
// Cancelled record — exactly the record shape the real executor produces
// when the orchestrator's cancel flag wins.
class CancelRelayExecutor final : public INativeExecutor {
public:
    Status execute(vortyx::distributed::DistributedJobRequest& request,
                   vortyx::distributed::DistributedJobRecord& out) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            executed_job_ = request.envelope.job_id;
            started_ = true;
        }
        started_cv_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        cancelled_cv_.wait(lock, [this] { return cancel_requested_; });
        out = vortyx::distributed::DistributedJobRecord();
        out.job_id = request.envelope.job_id;
        out.owner_user_id = "vortyx-worker";
        out.status = vortyx::distributed::DistributedJobStatus::Cancelled;
        return Status::Ok;
    }

    Status request_cancel(const vortyx::platform::JobId& job_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)job_id;
        cancel_requested_ = true;
        cancelled_cv_.notify_all();
        return Status::Ok;
    }

    void wait_started() {
        std::unique_lock<std::mutex> lock(mutex_);
        started_cv_.wait(lock, [this] { return started_; });
    }

    std::string executed_job() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return executed_job_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable started_cv_;
    std::condition_variable cancelled_cv_;
    bool started_ = false;
    bool cancel_requested_ = false;
    std::string executed_job_;
};

vortyx::distributed::DistributedJobRequest make_request(const std::string& id,
                                                        std::uint64_t n,
                                                        std::uint32_t shards) {
    vortyx::distributed::DistributedJobRequest request;
    request.envelope.job_id = id;
    request.envelope.operation = Op::VectorAdd;
    request.envelope.element_count = n;
    request.envelope.requested_backend = "cpu";
    request.requested_shard_count = shards;
    return request;
}

ClaimedJob make_claimed(const std::string& id, const char* op, std::uint64_t n) {
    ClaimedJob job;
    job.job_id = id;
    job.project_id = "proj";
    job.operation = op;
    job.element_count = n;
    job.requested_backend = "cpu";
    job.requested_shard_count = 1;
    job.attempt = 0;
    return job;
}

std::string claim_body(const ClaimedJob& job) {
    vortyx::platform::JsonValue response = vortyx::platform::JsonValue::make_object();
    response.add("claimed", vortyx::platform::JsonValue::make_bool(true));
    vortyx::platform::JsonValue job_json = vortyx::platform::JsonValue::make_object();
    job_json.add("job_id", vortyx::platform::JsonValue::make_string(job.job_id));
    job_json.add("project_id", vortyx::platform::JsonValue::make_string(job.project_id));
    job_json.add("operation", vortyx::platform::JsonValue::make_string(job.operation));
    job_json.add("element_count", vortyx::platform::JsonValue::make_number(
                                       static_cast<double>(job.element_count)));
    job_json.add("requested_backend",
                 vortyx::platform::JsonValue::make_string(job.requested_backend));
    job_json.add("requested_shard_count",
                 vortyx::platform::JsonValue::make_number(job.requested_shard_count));
    job_json.add("attempt", vortyx::platform::JsonValue::make_number(job.attempt));
    job_json.add("lease_expires_at_ms", vortyx::platform::JsonValue::make_number(1000));
    response.add("job", job_json);
    return response.serialize();
}

}  // namespace

int main() {
    // =====================================================================
    // 1. Payload synthesis
    // =====================================================================
    {
        vortyx::compute::ComputeTask first;
        std::string error;
        check(synthesize_task("job-one", Op::VectorAdd, 64, first, error) == Status::Ok,
              "synthesis succeeds");
        check(first.a.size() == 64 && first.b.size() == 64, "synthesis fills both operands");

        vortyx::compute::ComputeTask again;
        check(synthesize_task("job-one", Op::VectorAdd, 64, again, error) == Status::Ok,
              "re-synthesis succeeds");
        check(first.a == again.a && first.b == again.b,
              "the same job id synthesizes the same bytes");

        vortyx::compute::ComputeTask other;
        check(synthesize_task("job-two", Op::VectorAdd, 64, other, error) == Status::Ok,
              "the other job synthesizes");
        check(first.a != other.a, "a different job id synthesizes different bytes");

        // The VectorAdd policy: sums stay inside int32 (bit-exact by design).
        for (std::size_t i = 0; i < 64; ++i) {
            const std::int64_t sum = static_cast<std::int64_t>(first.a[i]) + first.b[i];
            if (sum > 2147483647 || sum < -2147483648LL) {
                check(false, "synthesized VectorAdd sum overflows int32");
                break;
            }
        }

        vortyx::compute::ComputeTask scale;
        check(synthesize_task("job-scale", Op::VectorScale, 32, scale, error) == Status::Ok,
              "VectorScale synthesis succeeds");
        check(scale.b.empty() && scale.scalar != 0, "VectorScale: scalar operand, no b");

        vortyx::compute::ComputeTask refused;
        check(synthesize_task("job-zero", Op::VectorAdd, 0, refused, error) == Status::InvalidInput,
              "a zero element count is refused");
        check(synthesize_task("job-big", Op::VectorAdd, 4294967296ULL, refused, error) ==
                  Status::InvalidInput,
              "an oversized element count is refused");
        check(synthesize_task("", Op::VectorAdd, 16, refused, error) == Status::InvalidInput,
              "an empty job id is refused");
    }

    // =====================================================================
    // 2. The worker protocol codec
    // =====================================================================
    {
        // Round trip.
        const std::string claim_request = encode_claim_request({"worker-a", 30000});
        check(claim_request.find("\"worker_id\":\"worker-a\"") != std::string::npos,
              "claim request encodes the worker id");

        ClaimResponse parsed;
        parse_claim_response("{\"claimed\":false}", parsed);
        check(parsed.ok && !parsed.claimed, "an honest no-work response parses");

        parse_claim_response(claim_body(make_claimed("job-x", "vector_add", 10)), parsed);
        check(parsed.ok && parsed.claimed && parsed.job.job_id == "job-x" &&
                  parsed.job.element_count == 10 && parsed.job.operation == "vector_add",
              "a claimed job parses field by field");

        parse_claim_response(
            "{\"error\":{\"code\":\"unauthenticated\",\"message\":\"bad token\"}}", parsed);
        check(!parsed.ok && parsed.error_code == "unauthenticated",
              "the unified error body parses");

        // Strictness.
        parse_claim_response("{\"claimed\":true,\"surprise\":1}", parsed);
        check(!parsed.ok, "unknown top-level fields are refused");
        parse_claim_response("{\"claimed\":true,\"job\":{\"job_id\":\"j\"}}", parsed);
        check(!parsed.ok, "an incomplete claimed job is refused");
        parse_claim_response("not json", parsed);
        check(!parsed.ok, "malformed JSON is refused");
        parse_claim_response("[1,2]", parsed);
        check(!parsed.ok, "a non-object response is refused");

        HeartbeatResponse heartbeat;
        parse_heartbeat_response(
            "{\"accepted\":true,\"cancel_requested\":true,\"lease_expires_at_ms\":500}",
            heartbeat);
        check(heartbeat.ok && heartbeat.accepted && heartbeat.cancel_requested,
              "a cancel-flagged heartbeat parses");
        parse_heartbeat_response("{\"accepted\":true,\"extra\":false}", heartbeat);
        check(!heartbeat.ok, "a heartbeat with unknown fields is refused");

        CompletionReport report;
        report.terminal_status = kTerminalCompleted;
        report.backend = "cpu";
        report.has_result_element_count = true;
        report.result_element_count = 42;
        std::string encode_error;
        const std::string body = encode_complete_request("worker-a", report, encode_error);
        check(!body.empty() && body.find("\"status\":\"completed\"") != std::string::npos &&
                  body.find("\"result_element_count\":42") != std::string::npos,
              "a complete request encodes");

        CompletionReport bad;
        bad.terminal_status = "sort-of-done";
        check(encode_complete_request("worker-a", bad, encode_error).empty(),
              "a bogus terminal status is refused");
        CompletionReport reasonless;
        reasonless.terminal_status = kTerminalFailed;
        check(encode_complete_request("worker-a", reasonless, encode_error).empty(),
              "a failure without its reason is refused");
    }

    // =====================================================================
    // 3. SimulatorNativeExecutor — REAL distributed execution
    // =====================================================================
    {
        NativeExecutorConfig config;
        config.device_count = 2;
        std::unique_ptr<INativeExecutor> executor;
        std::string error;
        check(SimulatorNativeExecutor::create(config, executor, error) == Status::Ok,
              "the executor builds: " + error);

        vortyx::distributed::DistributedJobRequest request = make_request("job-e2e", 200, 2);
        vortyx::distributed::DistributedJobRecord record;
        check(executor->execute(request, record) == Status::Ok, "the job executes");
        check(record.status == vortyx::distributed::DistributedJobStatus::Completed,
              "the real execution completes");
        check(record.shards.size() == 2, "two shards ran");
        check(record.result.data.size() == 200, "the aggregate result is the full domain");

        // Bit-exact against the host reference (the same synthesis).
        vortyx::compute::ComputeTask reference;
        check(synthesize_task("job-e2e", Op::VectorAdd, 200, reference, error) == Status::Ok,
              "the reference synthesizes");
        bool mismatch = false;
        for (std::size_t i = 0; i < 200; ++i) {
            if (record.result.data[i] != reference.a[i] + reference.b[i]) mismatch = true;
        }
        check(!mismatch, "the distributed result is bit-exact vs the host reference");
        check(!record.result.backends_used.empty(), "backends are honestly reported");

        // Post-terminal cancellation: refused honestly (InvalidInput).
        check(executor->request_cancel("job-e2e") == vortyx::platform::Status::InvalidInput,
              "cancelling a terminal job is refused");

        // A bad envelope is refused BEFORE execution (never executed, never
        // reported complete).
        vortyx::distributed::DistributedJobRequest bad = make_request("job-bad", 0, 1);
        vortyx::distributed::DistributedJobRecord bad_record;
        check(executor->execute(bad, bad_record) == Status::InvalidInput,
              "a zero-element claim is refused");
    }

    // =====================================================================
    // 4. The agent loop over the scripted transport
    // =====================================================================
    {
        // 4a: no work -> NoWork, exactly one claim posted.
        {
            ScriptedTransport transport;
            transport.script("/api/worker/claim", [] {
                return ScriptedTransport::ScriptedResponse{200, "{\"claimed\":false}"};
            });
            NativeExecutorConfig executor_config;
            std::unique_ptr<INativeExecutor> executor;
            std::string error;
            check(SimulatorNativeExecutor::create(executor_config, executor, error) ==
                      Status::Ok,
                  "4a: executor builds");
            WorkerAgentConfig agent_config;
            agent_config.worker_id = "worker-a";
            std::unique_ptr<WorkerAgent> agent;
            check(WorkerAgent::create(&transport, executor.get(), agent_config, agent,
                                      error) == Status::Ok,
                  "4a: agent builds");
            std::string detail;
            check(agent->run_cycle(detail) == WorkerAgent::CycleResult::NoWork,
                  "4a: no work is an honest outcome");
            check(transport.posts().size() == 1, "4a: only the claim was posted");
        }

        // 4b: claim -> real execute -> complete reported.
        {
            ScriptedTransport transport;
            transport.script("/api/worker/claim",
                             [job = make_claimed("job-run", "vector_add", 50)] {
                                 return ScriptedTransport::ScriptedResponse{
                                     200, claim_body(job)};
                             });
            transport.script("/api/worker/jobs/job-run/complete", [] {
                return ScriptedTransport::ScriptedResponse{
                    200, "{\"recorded\":true,\"status\":\"completed\"}"};
            });
            NativeExecutorConfig executor_config;
            std::unique_ptr<INativeExecutor> executor;
            std::string error;
            check(SimulatorNativeExecutor::create(executor_config, executor, error) ==
                      Status::Ok,
                  "4b: executor builds");
            WorkerAgentConfig agent_config;
            agent_config.worker_id = "worker-a";
            agent_config.heartbeat_interval_ms = 60000;  // one beat, immediately
            agent_config.lease_ms = 120000;
            std::unique_ptr<WorkerAgent> agent;
            check(WorkerAgent::create(&transport, executor.get(), agent_config, agent,
                                      error) == Status::Ok,
                  "4b: agent builds");
            std::string detail;
            check(agent->run_cycle(detail) == WorkerAgent::CycleResult::Claimed,
                  "4b: the cycle claims and completes");
            check(agent->last_reported_status() == "completed", "4b: completed reported");
            bool complete_posted = false;
            bool complete_body_ok = false;
            for (const auto& post : transport.posts()) {
                if (post.first.find("/complete") != std::string::npos) {
                    complete_posted = true;
                    complete_body_ok = post.second.find("\"status\":\"completed\"") !=
                                           std::string::npos &&
                                       post.second.find("\"result_element_count\":50") !=
                                           std::string::npos;
                }
            }
            check(complete_posted, "4b: the complete report was posted");
            check(complete_body_ok,
                  "4b: the report carries the real terminal status and result metadata");
        }

        // 4c: the cancel relay — the control plane's cancel reaches the
        // executing record through the heartbeat.
        {
            ScriptedTransport transport;
            transport.script("/api/worker/claim",
                             [job = make_claimed("job-cancel", "vector_add", 50)] {
                                 return ScriptedTransport::ScriptedResponse{
                                     200, claim_body(job)};
                             });
            // The FIRST heartbeat (immediate) reports the control plane's
            // cancel flag; the complete endpoint accepts the cancelled
            // record.
            transport.script("/api/worker/jobs/job-cancel/heartbeat", [] {
                return ScriptedTransport::ScriptedResponse{
                    200,
                    "{\"accepted\":true,\"cancel_requested\":true,"
                    "\"lease_expires_at_ms\":500}"};
            });
            transport.script("/api/worker/jobs/job-cancel/complete", [] {
                return ScriptedTransport::ScriptedResponse{
                    200, "{\"recorded\":true,\"status\":\"cancelled\"}"};
            });
            auto executor = std::make_unique<CancelRelayExecutor>();
            CancelRelayExecutor* executor_ptr = executor.get();
            WorkerAgentConfig agent_config;
            agent_config.worker_id = "worker-a";
            agent_config.heartbeat_interval_ms = 60000;
            agent_config.lease_ms = 120000;
            std::unique_ptr<WorkerAgent> agent;
            std::string error;
            check(WorkerAgent::create(&transport, executor.get(), agent_config, agent,
                                      error) == Status::Ok,
                  "4c: agent builds");

            // Drive the cycle on another thread and wait for the executor
            // to start (condvar — no sleeps).
            std::string cycle_detail;
            std::thread cycle([&agent, &cycle_detail]() {
                std::string local;
                (void)agent->run_cycle(local);
            });
            executor_ptr->wait_started();
            cycle.join();
            check(executor_ptr->executed_job() == "job-cancel", "4c: the job executed");
            check(agent->last_reported_status() == "cancelled",
                  "4c: the cancelled record is what got reported");
        }

        // 4d: an unsupported operation is reported FAILED (never dropped,
        // never faked).
        {
            ScriptedTransport transport;
            transport.script("/api/worker/claim",
                             [job = make_claimed("job-unknown-op", "matrix_magic", 10)] {
                                 return ScriptedTransport::ScriptedResponse{
                                     200, claim_body(job)};
                             });
            transport.script("/api/worker/jobs/job-unknown-op/fail", [] {
                return ScriptedTransport::ScriptedResponse{
                    200, "{\"recorded\":true,\"status\":\"failed\"}"};
            });
            NativeExecutorConfig executor_config;
            std::unique_ptr<INativeExecutor> executor;
            std::string error;
            check(SimulatorNativeExecutor::create(executor_config, executor, error) ==
                      Status::Ok,
                  "4d: executor builds");
            WorkerAgentConfig agent_config;
            agent_config.worker_id = "worker-a";
            std::unique_ptr<WorkerAgent> agent;
            check(WorkerAgent::create(&transport, executor.get(), agent_config, agent,
                                      error) == Status::Ok,
                  "4d: agent builds");
            std::string detail;
            check(agent->run_cycle(detail) == WorkerAgent::CycleResult::Claimed,
                  "4d: the refusal is reported, not swallowed");
            check(agent->last_reported_status() == "failed",
                  "4d: the unsupported operation failed honestly");
            bool fail_posted = false;
            for (const auto& post : transport.posts()) {
                if (post.first.find("/fail") != std::string::npos &&
                    post.second.find("unsupported operation") != std::string::npos) {
                    fail_posted = true;
                }
            }
            check(fail_posted, "4d: the failure carries its reason");
        }

        // 4e: a refused claim (bad token) is an Error outcome — the agent
        // does not pretend anything happened.
        {
            ScriptedTransport transport;
            transport.script("/api/worker/claim", [] {
                return ScriptedTransport::ScriptedResponse{
                    401, "{\"error\":{\"code\":\"unauthenticated\","
                         "\"message\":\"worker token rejected\"}}"};
            });
            NativeExecutorConfig executor_config;
            std::unique_ptr<INativeExecutor> executor;
            std::string error;
            check(SimulatorNativeExecutor::create(executor_config, executor, error) ==
                      Status::Ok,
                  "4e: executor builds");
            WorkerAgentConfig agent_config;
            agent_config.worker_id = "worker-a";
            std::unique_ptr<WorkerAgent> agent;
            check(WorkerAgent::create(&transport, executor.get(), agent_config, agent,
                                      error) == Status::Ok,
                  "4e: agent builds");
            std::string detail;
            check(agent->run_cycle(detail) == WorkerAgent::CycleResult::Error,
                  "4e: a refused claim is an honest error");
        }
    }

    // =====================================================================
    // 5. HTTP transport configuration refusals (no network involved)
    // =====================================================================
    {
        std::unique_ptr<IWorkerApiTransport> transport;
        std::string error;
        HttpTransportConfig https;
        https.endpoint = "https://api.example.com";
        check(HttpWorkerTransport::create(https, transport, error) ==
                  vortyx::platform::Status::InvalidInput,
              "https is refused up front (TLS termination is the deployment's job)");

        HttpTransportConfig garbage;
        garbage.endpoint = "not-a-url";
        check(HttpWorkerTransport::create(garbage, transport, error) ==
                  vortyx::platform::Status::InvalidInput,
              "a malformed endpoint is refused");

        HttpTransportConfig unresolvable;
        unresolvable.endpoint = "http://vortyx-does-not-exist.invalid:8080";
        check(HttpWorkerTransport::create(unresolvable, transport, error) ==
                  vortyx::platform::Status::InvalidInput,
              "an unresolvable host is refused at configuration time");
    }

    if (failures == 0) {
        std::cout << "All Phase 15 worker tests passed\n";
        return 0;
    }
    std::cerr << failures << " Phase 15 worker test(s) failed\n";
    return 1;
}
