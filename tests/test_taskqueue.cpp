// Task Queue & Async Execution tests (Phase 6) — CPU path.
//
// These tests MUST pass on every system, including machines without any GPU
// and CPU-only builds. They verify the Task Queue against the always-
// available CPU Virtual GPU:
//   lifecycle -> enqueue -> FIFO execution -> asynchronous execution ->
//   state transitions -> results -> wait -> shutdown (incl. drain policy) ->
//   move/destruction safety -> honest failure reporting.
//
// Design rules honored here:
//   - No timing-based flakiness: async behavior is proven with
//     synchronization primitives (a gated work item that blocks the worker
//     until the test releases it), never with "must be faster than X ms".
//   - FIFO order is proven with an order-recording work item (a minimal test
//     QueuedTask), not with sleeps or probabilities.
//   - Failures are never faked: failed tasks must carry the honest status.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/compute/task.hpp"
#include "core/queue/task_queue.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
using vortyx::queue::EnqueueResult;
using vortyx::queue::kInvalidTaskId;
using vortyx::queue::QueuedTask;
using vortyx::queue::QueueState;
using vortyx::queue::TaskId;
using vortyx::queue::TaskQueue;
using vortyx::queue::TaskSnapshot;
using vortyx::queue::TaskState;
using vortyx::queue::VectorAddQueuedTask;
using vortyx::vgpu::VirtualGpu;
using vortyx::vgpu::VirtualGpuDesc;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// The queue owns a std::thread + std::mutex + std::condition_variable. It can
// be neither copied nor moved (a moved queue would break the worker's `this`
// and every waiter's reference). Verified at compile time.
static_assert(!std::is_copy_constructible<TaskQueue>::value,
              "TaskQueue must not be copy-constructible");
static_assert(!std::is_copy_assignable<TaskQueue>::value,
              "TaskQueue must not be copy-assignable");
static_assert(!std::is_move_constructible<TaskQueue>::value,
              "TaskQueue must not be move-constructible (worker holds `this`)");
static_assert(!std::is_move_assignable<TaskQueue>::value,
              "TaskQueue must not be move-assignable (worker holds `this`)");
static_assert(!std::is_copy_constructible<QueuedTask>::value,
              "QueuedTask must not be copy-constructible");

VectorAddTask make_task(std::size_t count) {
    VectorAddTask task;
    task.a.resize(count);
    task.b.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Deterministic values well inside int32 range (no overflow).
        task.a[i] = static_cast<std::int32_t>(i % 1000) - 300;
        task.b[i] = static_cast<std::int32_t>((i * 7) % 500) + 11;
    }
    return task;
}

bool result_matches(const VectorAddResult& result, const VectorAddTask& task) {
    if (result.status != Status::Ok) return false;
    if (result.data.size() != task.a.size()) return false;
    for (std::size_t i = 0; i < task.a.size(); ++i) {
        if (result.data[i] != task.a[i] + task.b[i]) return false;
    }
    return true;
}

// A work item that blocks the worker inside execute() until the test
// releases it. This makes the worker's position observable without any
// timing: while the gate is closed, the queue is provably busy in THIS task.
// The test keeps the release promise; the task waits on the matching future.
class GatedTask final : public QueuedTask {
public:
    GatedTask(VectorAddTask task, std::future<void> release_gate)
        : task_(std::move(task)), release_gate_(std::move(release_gate)) {}

    VectorAddResult execute(VirtualGpu& gpu) override {
        started_.set_value();
        release_gate_.wait();
        return gpu.execute(task_);
    }

    std::future<void> started_future() { return started_.get_future(); }

private:
    VectorAddTask task_;
    std::promise<void> started_;
    std::future<void> release_gate_;
};

// A work item that records the order in which tasks are EXECUTED (not
// enqueued). With a single worker thread, execution is strictly serial, so
// the recorded order must equal the enqueue order if the queue is FIFO.
class OrderRecordingTask final : public QueuedTask {
public:
    OrderRecordingTask(int key, std::mutex& order_mutex, std::vector<int>& order)
        : key_(key), order_mutex_(order_mutex), order_(order) {}

    VectorAddResult execute(VirtualGpu& /*gpu*/) override {
        {
            std::lock_guard<std::mutex> lock(order_mutex_);
            order_.push_back(key_);
        }
        return VectorAddResult{Status::Ok, std::string{}, {}};
    }

private:
    int key_;
    std::mutex& order_mutex_;
    std::vector<int>& order_;
};

// A work item that fails with a fixed, honest error (queue-level failure
// propagation check; does not touch the Virtual GPU at all).
class FailingTask final : public QueuedTask {
public:
    VectorAddResult execute(VirtualGpu& /*gpu*/) override {
        return VectorAddResult{Status::BackendError, "intentional test failure", {}};
    }
};

}  // namespace

int main() {
    // --- 1. Fresh object: Uninitialized and honestly unusable ---------------
    {
        TaskQueue queue;
        check(queue.state() == QueueState::Uninitialized,
              "a fresh TaskQueue must be Uninitialized");
        check(std::string(vortyx::queue::to_string(queue.state())) == "Uninitialized",
              "to_string(QueueState) must report Uninitialized");
        check(std::string(vortyx::queue::to_string(TaskState::Running)) == "Running",
              "to_string(TaskState) must report Running");

        const EnqueueResult early = queue.enqueue(make_task(4));
        check(early.status == Status::NotInitialized,
              "enqueue() before initialize() must fail with NotInitialized");
        check(early.id == kInvalidTaskId,
              "a refused enqueue() must return kInvalidTaskId");
        check(!early.error.empty(), "a refused enqueue() must carry a reason");

        check(queue.task_state(7) == TaskState::Invalid,
              "task_state() for an unknown id must be Invalid");
        check(queue.task_snapshot(7).state == TaskState::Invalid,
              "task_snapshot() for an unknown id must be Invalid");
        check(queue.wait(7) == TaskState::Invalid,
              "wait() for an unknown id must return Invalid without blocking");
        check(vortyx::queue::task_state_is_terminal(TaskState::Completed) &&
                  vortyx::queue::task_state_is_terminal(TaskState::Failed) &&
                  !vortyx::queue::task_state_is_terminal(TaskState::Queued) &&
                  !vortyx::queue::task_state_is_terminal(TaskState::Running) &&
                  !vortyx::queue::task_state_is_terminal(TaskState::Invalid),
              "task_state_is_terminal() must classify states correctly");

        queue.shutdown();  // must be safe on a fresh object
        check(queue.state() == QueueState::ShutDown,
              "shutdown() on a fresh object must end in ShutDown");
    }

    // --- 2. initialize: refuses a non-Ready Virtual GPU, accepts a Ready one
    VirtualGpu cpu_gpu;
    TaskQueue queue;
    {
        const Status refused = queue.initialize(cpu_gpu);
        check(refused == Status::InvalidInput,
              "initialize() with an Uninitialized Virtual GPU must be refused");
        check(queue.state() == QueueState::Uninitialized,
              "a refused initialize() must leave the queue Uninitialized");

        check(cpu_gpu.initialize() == Status::Ok, "cpu VirtualGpu must initialize");
        const Status ok = queue.initialize(cpu_gpu);
        check(ok == Status::Ok, "initialize() with a Ready Virtual GPU must succeed");
        check(queue.state() == QueueState::Ready, "an initialized queue must be Ready");
        check(std::string(vortyx::queue::to_string(queue.state())) == "Ready",
              "to_string(QueueState) must report Ready");
    }

    // --- 3. Re-initialization while Ready is refused ------------------------
    {
        const Status again = queue.initialize(cpu_gpu);
        check(again == Status::InvalidInput,
              "initialize() while Ready must be refused with InvalidInput");
        check(queue.state() == QueueState::Ready,
              "a refused re-initialization must leave the queue Ready");
        const EnqueueResult still_works = queue.enqueue(make_task(4));
        check(still_works.status == Status::Ok,
              "the queue must keep working after a refused re-initialization");
        check(queue.wait(still_works.id) == TaskState::Completed,
              "the task enqueued after a refused re-initialization must complete");
    }

    // --- 4. enqueue + wait + result correctness ------------------------------
    {
        const VectorAddTask task = make_task(1024);
        const EnqueueResult r = queue.enqueue(task);
        check(r.status == Status::Ok, "a valid enqueue() must succeed");
        check(r.id != kInvalidTaskId, "a successful enqueue() must return a valid id");
        check(r.error.empty(), "a successful enqueue() must have an empty error");

        const TaskState final_state = queue.wait(r.id);
        check(final_state == TaskState::Completed, "wait() must report Completed");
        const TaskSnapshot snap = queue.task_snapshot(r.id);
        check(snap.state == TaskState::Completed,
              "task_snapshot() after wait() must agree with wait()");
        check(result_matches(snap.result, task),
              "the queued vector addition must compute A+B correctly");
    }

    // --- 5. Ids: unique, increasing, never invalid ---------------------------
    {
        std::vector<TaskId> ids;
        for (int i = 0; i < 10; ++i) {
            const EnqueueResult r = queue.enqueue(make_task(8));
            check(r.status == Status::Ok, "enqueue in id test must succeed");
            ids.push_back(r.id);
        }
        std::set<TaskId> unique(ids.begin(), ids.end());
        check(unique.size() == ids.size(), "task ids must be unique");
        check(*std::min_element(ids.begin(), ids.end()) > kInvalidTaskId,
              "no task id may equal kInvalidTaskId");
        bool increasing = true;
        for (std::size_t i = 1; i < ids.size(); ++i) {
            if (ids[i] <= ids[i - 1]) increasing = false;
        }
        check(increasing, "task ids must be monotonically increasing (never reused)");
        for (const TaskId id : ids) {
            check(queue.wait(id) == TaskState::Completed, "id-test task must complete");
        }
    }

    // --- 6. Asynchronous execution and state transitions (deterministic) -----
    {
        // The gated task blocks the worker inside execute() until released.
        // Everything observed while the gate is closed is therefore a stable
        // fact about the queue, not a race that happens to pass.
        std::promise<void> release_promise;
        auto gated = std::make_unique<GatedTask>(make_task(16), release_promise.get_future());
        const std::future<void> worker_inside = gated->started_future();
        const TaskId gated_id = [&] {
            const EnqueueResult r =
                queue.enqueue(std::unique_ptr<QueuedTask>(std::move(gated)));
            check(r.status == Status::Ok, "gated enqueue must succeed");
            return r.id;
        }();

        check(worker_inside.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
              "the worker must start executing the gated task");
        check(queue.task_state(gated_id) == TaskState::Running,
              "while the worker is inside execute(), the task must be Running");

        // enqueue() must NOT wait for the running task: it returns while the
        // worker is still blocked inside the gated task's execute(). (If
        // enqueue() blocked on completion, this call would deadlock — the
        // test finishing at all is the proof.)
        const VectorAddTask second = make_task(32);
        const EnqueueResult second_r = queue.enqueue(second);
        check(second_r.status == Status::Ok,
              "enqueue() while the worker is busy must return immediately (non-blocking)");
        check(queue.task_state(second_r.id) == TaskState::Queued,
              "a task behind a running task must stay Queued");

        // Bounded wait: not terminal yet (worker still blocked), must return
        // the current non-terminal state instead of blocking forever.
        const TaskState not_yet =
            queue.wait_for(second_r.id, std::chrono::milliseconds(50));
        check(not_yet == TaskState::Queued,
              "wait_for() on a still-queued task must return Queued (not done yet)");
        const TaskSnapshot running_snap = queue.task_snapshot(gated_id);
        check(running_snap.state == TaskState::Running,
              "snapshot of the running task must show Running");

        release_promise.set_value();  // let the worker finish the gated task

        check(queue.wait(gated_id) == TaskState::Completed,
              "the gated task must complete after release");
        check(queue.wait(second_r.id) == TaskState::Completed,
              "the second task must complete after the gated task");
        const TaskSnapshot gated_snap = queue.task_snapshot(gated_id);
        check(gated_snap.result.status == Status::Ok &&
                  result_matches(gated_snap.result, make_task(16)),
              "the gated task's result must be correct");
        check(result_matches(queue.task_snapshot(second_r.id).result, second),
              "the second task's result must be correct");
    }

    // --- 7. FIFO execution order ---------------------------------------------
    {
        std::mutex order_mutex;
        std::vector<int> order;
        std::vector<TaskId> ids;
        const int count = 10;
        for (int i = 0; i < count; ++i) {
            const EnqueueResult r = queue.enqueue(std::unique_ptr<QueuedTask>(
                std::make_unique<OrderRecordingTask>(i, order_mutex, order)));
            check(r.status == Status::Ok, "order-recording enqueue must succeed");
            ids.push_back(r.id);
        }
        for (const TaskId id : ids) {
            check(queue.wait(id) == TaskState::Completed, "order task must complete");
        }
        check(order.size() == static_cast<std::size_t>(count),
              "every order-recording task must have executed exactly once");
        bool fifo = true;
        for (int i = 0; i < count; ++i) {
            if (order[static_cast<std::size_t>(i)] != i) fifo = false;
        }
        check(fifo, "execution order must equal enqueue order (FIFO)");
    }

    // --- 8. Failed tasks are reported honestly --------------------------------
    {
        // 8a. A custom task that fails: the failure must propagate verbatim.
        const EnqueueResult fail_r = queue.enqueue(
            std::unique_ptr<QueuedTask>(std::make_unique<FailingTask>()));
        check(fail_r.status == Status::Ok, "enqueue of a failing task is accepted (it fails at execution)");
        check(queue.wait(fail_r.id) == TaskState::Failed,
              "wait() must report Failed for a failing task");
        const TaskSnapshot fail_snap = queue.task_snapshot(fail_r.id);
        check(fail_snap.state == TaskState::Failed, "snapshot must show Failed");
        check(fail_snap.result.status == Status::BackendError,
              "the recorded failure must carry the honest status");
        check(fail_snap.result.error == "intentional test failure",
              "the recorded failure must carry the honest error message");
        check(fail_snap.result.data.empty(),
              "a failed task must not carry result data");

        // 8b. A real Runtime rejection through the generic path (content
        // validation happens at execution time there): mismatched sizes must
        // fail the task with Status::InvalidInput from the Runtime itself.
        VectorAddTask mismatch;
        mismatch.a = {1, 2, 3};
        mismatch.b = {1, 2};
        const EnqueueResult mismatch_r = queue.enqueue(
            std::unique_ptr<QueuedTask>(std::make_unique<VectorAddQueuedTask>(std::move(mismatch))));
        check(mismatch_r.status == Status::Ok,
              "the generic enqueue path accepts the task (validation happens at execution)");
        check(queue.wait(mismatch_r.id) == TaskState::Failed,
              "a Runtime-rejected task must end Failed");
        check(queue.task_snapshot(mismatch_r.id).result.status == Status::InvalidInput,
              "the recorded failure must be the Runtime's InvalidInput");

        // 8c. The typed overload validates at enqueue time: an invalid task
        // never enters the queue at all.
        VectorAddTask invalid;
        invalid.a = {1};
        invalid.b = {1, 2};
        const EnqueueResult refused = queue.enqueue(invalid);
        check(refused.status == Status::InvalidInput,
              "an invalid VectorAddTask must be refused at enqueue()");
        check(refused.id == kInvalidTaskId,
              "a refused task must not consume an id");
        check(queue.task_state(refused.id) == TaskState::Invalid,
              "a refused task must not be trackable");

        // 8d. A null work item is refused.
        const EnqueueResult null_r = queue.enqueue(std::unique_ptr<QueuedTask>{});
        check(null_r.status == Status::InvalidInput,
              "enqueue(nullptr) must be refused with InvalidInput");
    }

    // --- 9. Queue bound to a known-but-unavailable backend (honest) ----------
    {
        // Environment-adaptive, always honest: on machines WITH a usable
        // Vulkan device the queued tasks really execute on it; without one
        // every task must FAIL with Status::BackendUnavailable (never fall
        // back to the CPU silently).
        VirtualGpu vulkan_gpu;
        VirtualGpuDesc desc;
        desc.backend = "vulkan";
        check(vulkan_gpu.initialize(desc) == Status::Ok,
              "initializing a vulkan Virtual GPU must succeed (known backend)");
        TaskQueue vulkan_queue;
        check(vulkan_queue.initialize(vulkan_gpu) == Status::Ok,
              "the queue must accept a Ready Virtual GPU with an unavailable backend");

        const EnqueueResult r = vulkan_queue.enqueue(make_task(64));
        check(r.status == Status::Ok, "enqueue on the vulkan-bound queue must be accepted");
        const TaskState state = vulkan_queue.wait(r.id);
        if (vulkan_gpu.backend_available()) {
            check(state == TaskState::Completed,
                  "with an available vulkan backend the queued task must complete");
            check(result_matches(vulkan_queue.task_snapshot(r.id).result, make_task(64)),
                  "the vulkan queue's result must be correct");
        } else {
            check(state == TaskState::Failed,
                  "without a usable vulkan backend the queued task must FAIL (no silent fallback)");
            check(vulkan_queue.task_snapshot(r.id).result.status == Status::BackendUnavailable,
                  "the honest failure must be Status::BackendUnavailable");
            std::cout << "Note: vulkan backend unavailable in this environment - "
                      << vulkan_gpu.backend_unavailable_reason() << "\n";
        }
        vulkan_queue.shutdown();
        vulkan_gpu.shutdown();
    }

    // --- 10. Shutdown policy A: drain pending tasks FIFO, then join ----------
    {
        // The gated task occupies the worker; the next three tasks wait in
        // the FIFO. shutdown() must finish ALL of them before returning.
        std::promise<void> drain_release;
        auto gated = std::make_unique<GatedTask>(make_task(8), drain_release.get_future());
        const std::future<void> worker_inside = gated->started_future();
        const TaskId gated_id = [&] {
            const EnqueueResult r =
                queue.enqueue(std::unique_ptr<QueuedTask>(std::move(gated)));
            check(r.status == Status::Ok, "drain test: gated enqueue");
            return r.id;
        }();
        check(worker_inside.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
              "drain test: worker must reach the gated task");

        std::vector<TaskId> pending_ids;
        for (int i = 0; i < 3; ++i) {
            const EnqueueResult r = queue.enqueue(make_task(16));
            check(r.status == Status::Ok, "drain test: pending enqueue");
            pending_ids.push_back(r.id);
        }
        check(queue.task_state(pending_ids.back()) == TaskState::Queued,
              "drain test: pending tasks must be Queued while the worker is busy");

        drain_release.set_value();  // unblock the worker
        queue.shutdown();           // must return only after everything drained

        check(queue.state() == QueueState::ShutDown, "shutdown() must end in ShutDown");
        check(queue.task_state(gated_id) == TaskState::Completed,
              "drain: the running task must be Completed after shutdown()");
        for (const TaskId id : pending_ids) {
            check(queue.task_state(id) == TaskState::Completed,
                  "drain: every pending task must be Completed after shutdown()");
        }

        // After shutdown: enqueue refused, but results stay queryable.
        const EnqueueResult after = queue.enqueue(make_task(4));
        check(after.status == Status::NotInitialized,
              "enqueue() after shutdown() must fail with NotInitialized");
        check(after.id == kInvalidTaskId, "post-shutdown enqueue must return no id");
        check(!after.error.empty(), "post-shutdown enqueue must carry a reason");
        check(queue.task_state(gated_id) == TaskState::Completed,
              "records must stay queryable after shutdown()");

        queue.shutdown();  // double shutdown: safe no-op
        check(queue.state() == QueueState::ShutDown,
              "double shutdown() must stay ShutDown");
    }

    // --- 11. Re-initialization: fresh session, ids never reused --------------
    {
        // (queue is ShutDown here after section 10.)
        const EnqueueResult probe = queue.enqueue(make_task(4));
        check(probe.status == Status::NotInitialized,
              "the queue must still refuse enqueue() before re-initialization");

        check(queue.initialize(cpu_gpu) == Status::Ok,
              "re-initialization after shutdown() must succeed");
        check(queue.state() == QueueState::Ready, "the re-initialized queue must be Ready");

        const EnqueueResult fresh = queue.enqueue(make_task(4));
        check(fresh.status == Status::Ok, "the re-initialized queue must accept tasks");
        check(fresh.id > 0 && fresh.id != kInvalidTaskId, "the fresh id must be valid");
        check(queue.wait(fresh.id) == TaskState::Completed,
              "the re-initialized queue must execute");
        // Old-session ids are unknown in the new session (records were reset).
        check(queue.task_state(fresh.id - 1) == TaskState::Invalid ||
                  fresh.id == vortyx::queue::kFirstTaskId,
              "a previous session's last id must not alias the new session's tasks");
    }

    // --- 12. The bound Virtual GPU shutting down first: honest, no crash -----
    {
        VirtualGpu short_gpu;
        check(short_gpu.initialize() == Status::Ok, "short-lived gpu must initialize");
        TaskQueue short_queue;
        check(short_queue.initialize(short_gpu) == Status::Ok,
              "the queue must initialize on the short-lived gpu");

        std::promise<void> gpu_first_release;
        auto gated =
            std::make_unique<GatedTask>(make_task(8), gpu_first_release.get_future());
        const std::future<void> worker_inside = gated->started_future();
        const TaskId id = [&] {
            const EnqueueResult r =
                short_queue.enqueue(std::unique_ptr<QueuedTask>(std::move(gated)));
            check(r.status == Status::Ok, "gpu-first shutdown: enqueue");
            return r.id;
        }();
        check(worker_inside.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
              "gpu-first shutdown: worker must reach the gated task");

        // Caller error (gpu before queue) must still be SAFE: the worker is
        // blocked in the gate, so shutting the gpu down here cannot race an
        // in-flight execute(). After release, the task fails honestly.
        short_gpu.shutdown();
        gpu_first_release.set_value();
        check(short_queue.wait(id) == TaskState::Failed,
              "a task whose Virtual GPU was shut down must fail (never fake success)");
        check(short_queue.task_snapshot(id).result.status == Status::NotInitialized,
              "the failure must report the Virtual GPU is not usable");

        short_queue.shutdown();  // must work normally even after the gpu is gone
        check(short_queue.state() == QueueState::ShutDown,
              "queue shutdown after gpu shutdown must end in ShutDown");
    }

    // --- 13. Destructor joins the worker (no leaked thread, no terminate) ----
    {
        {
            TaskQueue scoped;
            check(scoped.initialize(cpu_gpu) == Status::Ok, "scoped queue must initialize");
            std::vector<TaskId> ids;
            for (int i = 0; i < 5; ++i) {
                const EnqueueResult r = scoped.enqueue(make_task(64));
                check(r.status == Status::Ok, "scoped enqueue");
                ids.push_back(r.id);
            }
            for (const TaskId id : ids) {
                check(scoped.wait(id) == TaskState::Completed, "scoped task must complete");
            }
            // No shutdown() call: the destructor must join the worker cleanly.
        }  // scoped queue destroyed here
        std::cout << "Note: scoped TaskQueue destroyed without explicit shutdown (destructor path).\n";
    }

    // --- 14. Concurrent enqueue from several threads --------------------------
    {
        TaskQueue cq;
        check(cq.initialize(cpu_gpu) == Status::Ok, "concurrent queue must initialize");

        constexpr int kThreads = 3;
        constexpr int kPerThread = 20;
        std::vector<std::vector<TaskId>> ids(kThreads);
        std::vector<std::thread> producers;
        std::mutex ids_mutex;
        std::atomic<bool> all_accepted{true};

        for (int t = 0; t < kThreads; ++t) {
            producers.emplace_back([t, kPerThread, &cq, &ids, &ids_mutex, &all_accepted] {
                for (int i = 0; i < kPerThread; ++i) {
                    VectorAddTask task;
                    const std::int32_t base = static_cast<std::int32_t>(t * 1000 + i);
                    task.a = {base, base + 1, base + 2};
                    task.b = {1, 2, 3};
                    const EnqueueResult r =
                        cq.enqueue(std::unique_ptr<QueuedTask>(
                            std::make_unique<VectorAddQueuedTask>(std::move(task))));
                    if (r.status != Status::Ok) {
                        all_accepted = false;
                        return;
                    }
                    std::lock_guard<std::mutex> lock(ids_mutex);
                    ids[static_cast<std::size_t>(t)].push_back(r.id);
                }
            });
        }
        for (std::thread& t : producers) t.join();
        check(all_accepted.load(), "every concurrent enqueue must be accepted");

        std::set<TaskId> unique;
        for (const auto& vec : ids) {
            for (const TaskId id : vec) unique.insert(id);
        }
        check(unique.size() == static_cast<std::size_t>(kThreads * kPerThread),
              "concurrently produced ids must all be distinct");

        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kPerThread; ++i) {
                const TaskId id = ids[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)];
                check(cq.wait(id) == TaskState::Completed, "concurrent task must complete");
                const VectorAddResult result = cq.task_snapshot(id).result;
                const std::int32_t base = static_cast<std::int32_t>(t * 1000 + i);
                const bool correct = result.status == Status::Ok && result.data.size() == 3 &&
                                     result.data[0] == base + 1 &&
                                     result.data[1] == base + 3 &&
                                     result.data[2] == base + 5;
                check(correct, "concurrent task results must match their own inputs");
            }
        }
        cq.shutdown();
    }

    // --- 15. Repeated initialize/shutdown cycles stay safe --------------------
    {
        TaskQueue cycles;
        for (int cycle = 0; cycle < 3; ++cycle) {
            check(cycles.initialize(cpu_gpu) == Status::Ok,
                  "cycle " + std::to_string(cycle) + " initialize");
            const EnqueueResult r = cycles.enqueue(make_task(32));
            check(r.status == Status::Ok, "cycle " + std::to_string(cycle) + " enqueue");
            check(cycles.wait(r.id) == TaskState::Completed,
                  "cycle " + std::to_string(cycle) + " execute");
            cycles.shutdown();
            check(cycles.state() == QueueState::ShutDown,
                  "cycle " + std::to_string(cycle) + " shutdown");
        }
    }

    queue.shutdown();
    cpu_gpu.shutdown();

    if (failures == 0) {
        std::cout << "Task Queue CPU tests passed.\n";
        return 0;
    }
    return 1;
}
