#pragma once

// Task Queue & Async Execution (Phase 6).
//
// The layer between the application and the Virtual GPU that lets several
// compute tasks be SUBMITTED first and EXECUTED later, asynchronously, on
// one dedicated worker execution context:
//
//   Application -> Task Queue -> Virtual GPU -> Compute Runtime
//                                          -> Resource Manager -> Backend
//
// Responsibilities (exactly these, nothing more):
//   - Queue tasks submitted through enqueue() and hand them to the ONE
//     Virtual GPU the queue was initialized with, strictly FIFO.
//   - Give every accepted task a unique, never-reused TaskId.
//   - Track per-task state (Queued -> Running -> Completed/Failed) and keep
//     the execution result queryable until the queue itself is destroyed.
//   - Run the single worker thread: wait on a condition variable (no
//     busy-waiting), execute, record, repeat.
//   - Shutdown policy A: stop accepting new tasks, let the worker finish
//     every already-queued task in FIFO order, then join the worker.
//
// The Task Queue is NOT a Scheduler. It never inspects load, never compares
// backends, never chooses between CPU and GPU, never reorders, reprioritizes
// or steals work. Whatever Virtual GPU it was given is the only execution
// target it will ever use. Scheduler logic (device selection, priorities,
// load balancing) is Phase 7 and lives somewhere else entirely.
//
// Task model:
//   QueuedTask is the minimal abstraction of "work the bound Virtual GPU can
//   execute": one execute() call, returning the project-wide VectorAddResult.
//   Phase 6 ships exactly one implementation, VectorAddQueuedTask, wrapping
//   the Phase 3 VectorAddTask. Future task kinds (matrix multiply, image
//   processing, ...) derive from QueuedTask without changing the queue.
//
// Task identity:
//   TaskId follows the ResourceId pattern from Phase 4: a 64-bit monotonic
//   counter, 0 reserved as invalid, ids never reused — a stale id can never
//   alias a newer task, even after shutdown()/re-initialization.
//
// Threading:
//   One std::mutex guards all queue bookkeeping. The worker pops a task and
//   releases the mutex BEFORE executing it, so a long-running Virtual GPU
//   execution never blocks enqueue()/task_state()/wait() (and vice versa:
//   nothing in the Virtual GPU can ever block on a mutex the worker holds).
//   With a single mutex and no nested locking, deadlock is structurally
//   impossible. The worker waits on a condition variable — it consumes no
//   CPU while the queue is empty.
//
// Ownership and lifetime:
//   - The queue REFERENCES one Virtual GPU (non-owning pointer). The caller
//     owns it and must shut the queue down BEFORE the Virtual GPU
//     (queue.shutdown() -> worker joined -> gpu.shutdown()). Executing a task
//     on a Virtual GPU that was shut down early does not crash: the task
//     fails honestly with Status::NotInitialized.
//   - The queue owns its queued tasks (unique_ptr) and every TaskRecord
//     (state + result). Records are kept until the queue object is
//     destroyed, so results remain fetchable after shutdown().
//   - Copying and moving are BOTH deleted: the object owns a std::thread
//     and a std::mutex, and a moved queue would break the worker's `this`
//     and every waiter's reference. One queue, one address, for life.
//   - ~TaskQueue() calls shutdown(): the worker is always joined before any
//     member is destroyed. A queue can never leak a thread or destroy a
//     thread that is still running.
//
// Error handling follows the project-wide result style (Phase 3): no
// exceptions thrown by the queue itself, explicit Status values with
// human-readable 'error' strings. Failures are never hidden — a task whose
// execution fails is reported as Failed with the honest reason.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "core/compute/task.hpp"  // Status / VectorAddTask / VectorAddResult

namespace vortyx::vgpu {
class VirtualGpu;  // the queue's execution target; complete type in the .cpp
}

namespace vortyx::queue {

// The queue layer speaks the project-wide result vocabulary from Phase 3
// (one unified error model, no second status system).
using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
// Phase 10 (Compute Engine): the generic task vocabulary for the new
// built-in queued work item.
using vortyx::compute::ComputeTask;

// ---------------------------------------------------------------------------
// Task identity
// ---------------------------------------------------------------------------

// Opaque handle to a task accepted by a TaskQueue. 0 is reserved as "no
// task". Ids are never reused (monotonic counter), so a stale id can never
// accidentally alias a newly enqueued task.
using TaskId = std::uint64_t;

inline constexpr TaskId kInvalidTaskId = 0;
inline constexpr TaskId kFirstTaskId = 1;

// ---------------------------------------------------------------------------
// Task state
// ---------------------------------------------------------------------------

// Lifecycle of ONE task inside the queue.
//  - Invalid:   the id is unknown to this queue (never issued, or issued by
//               a previous queue session before re-initialization).
//  - Queued:    accepted, waiting in FIFO order.
//  - Running:   the worker has popped it and is executing it right now.
//  - Completed: execution finished with Status::Ok; the result data is valid.
//  - Failed:    execution finished with a status != Ok; the recorded result
//               carries the honest error.
// There is deliberately no Cancelled value: cancelling queued work is not
// implemented in Phase 6, and claiming an unimplemented state would be a lie.
enum class TaskState {
    Invalid,
    Queued,
    Running,
    Completed,
    Failed,
};

const char* to_string(TaskState state);

// True when the state can no longer change (the queue reached a final
// verdict for the task). Used by wait(); exposed as a free function because
// callers checking task_state() need the same predicate.
bool task_state_is_terminal(TaskState state);

// ---------------------------------------------------------------------------
// Queue lifecycle state
// ---------------------------------------------------------------------------

// Lifecycle of the QUEUE itself.
//  - Uninitialized: constructed (or re-initializable); initialize() has not
//    succeeded yet. enqueue() is refused.
//  - Ready: worker running, tasks accepted.
//  - ShuttingDown: shutdown() was called; new tasks are refused while the
//    worker drains the remaining FIFO queue.
//  - ShutDown: worker joined, every submitted task reached a terminal state.
//    enqueue() is refused until initialize() is called again.
enum class QueueState {
    Uninitialized,
    Ready,
    ShuttingDown,
    ShutDown,
};

const char* to_string(QueueState state);

// ---------------------------------------------------------------------------
// Queued work abstraction
// ---------------------------------------------------------------------------

// One unit of queued work. The queue calls execute() exactly once, on its
// worker context, passing the ONE Virtual GPU it was initialized with.
// Implementations must not throw (a throw is caught by the worker and turns
// the task into an honest Failed result) and must not shut down or reconfigure
// the Virtual GPU they receive.
class QueuedTask {
public:
    QueuedTask() = default;
    virtual ~QueuedTask() = default;

    // Held by the queue through unique_ptr; copying a live unit of work has
    // no defined owner, so it is forbidden exactly like every other Vortyx
    // execution/resource handle.
    QueuedTask(const QueuedTask&) = delete;
    QueuedTask& operator=(const QueuedTask&) = delete;

    virtual VectorAddResult execute(vortyx::vgpu::VirtualGpu& gpu) = 0;
};

// The Phase 6 built-in work item: vector addition (C = A + B) executed
// through the bound Virtual GPU. Wraps the Phase 3 VectorAddTask unchanged.
class VectorAddQueuedTask final : public QueuedTask {
public:
    // Stores the task by value: the queue owns its input completely, callers
    // may destroy their copy as soon as enqueue() returns.
    explicit VectorAddQueuedTask(VectorAddTask task);

    VectorAddResult execute(vortyx::vgpu::VirtualGpu& gpu) override;

private:
    VectorAddTask task_;
};

// Phase 10 built-in work item: any generic ComputeTask (VectorAdd /
// VectorMultiply / VectorScale) executed through the bound Virtual GPU's
// Compute Engine path. Runs on the queue's single worker exactly like
// VectorAddQueuedTask; the only difference is the workload description.
//
// Result note: the queue's result vocabulary is VectorAddResult (Phase 6
// API, unchanged); the engine's ComputeTaskResult has the identical shape
// today (status + error + int32 data), and this wrapper records it as such.
// A future task kind whose result payload genuinely differs will need the
// queue's result type to evolve — that is an explicit future seam, not
// something this phase fakes.
class ComputeTaskQueuedTask final : public QueuedTask {
public:
    explicit ComputeTaskQueuedTask(ComputeTask task);

    VectorAddResult execute(vortyx::vgpu::VirtualGpu& gpu) override;

private:
    ComputeTask task_;
};

// ---------------------------------------------------------------------------
// Queue results
// ---------------------------------------------------------------------------

// Result of an enqueue() request. 'id' is a usable task id only when
// status == Status::Ok; 'error' explains any refusal. (Same shape as the
// resource layer's BufferResult: handle + status + error.)
struct EnqueueResult {
    TaskId id = kInvalidTaskId;
    Status status = Status::Ok;
    std::string error;  // empty when status == Ok
};

// Point-in-time view of one task: its current state plus the recorded
// execution result. 'result' is meaningful only when state is Completed or
// Failed (for Queued/Running it is a default-constructed value); callers
// must consult 'state' first. Returned by value — no references into the
// queue's internals, no dangling risk.
struct TaskSnapshot {
    TaskState state = TaskState::Invalid;
    VectorAddResult result{};
};

// ---------------------------------------------------------------------------
// Task Queue
// ---------------------------------------------------------------------------

class TaskQueue final {
public:
    // Creates an Uninitialized queue. initialize() before any use.
    TaskQueue() = default;

    // Joins the worker (if running). Always safe; see shutdown().
    ~TaskQueue();

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    // Move is deleted as well: the worker thread and every waiting caller
    // hold `this`. A movable queue would be a use-after-free factory.
    TaskQueue(TaskQueue&&) = delete;
    TaskQueue& operator=(TaskQueue&&) = delete;

    // Binds the queue to one Virtual GPU and starts the single worker thread.
    // The Virtual GPU must be Ready (Status::InvalidInput otherwise) and must
    // outlive the queue until shutdown(). A known-but-unavailable backend on
    // the Virtual GPU is accepted (matching Virtual GPU semantics): the queue
    // logs a warning and every task will honestly fail with
    // Status::BackendUnavailable — the queue never reroutes work elsewhere.
    //
    // Re-initializing while Ready is refused with Status::InvalidInput (call
    // shutdown() first). After a shutdown() a fresh initialize() is allowed;
    // task ids continue monotonically, so ids from the previous session are
    // never handed out again.
    // Returns Status::Ok, or Status::BackendError if the worker thread cannot
    // be created (the queue then stays unusable, no half state is kept).
    Status initialize(vortyx::vgpu::VirtualGpu& gpu);

    // Shutdown policy A: refuse new tasks, let the worker finish every
    // already-accepted task in FIFO order, then join it. When shutdown()
    // returns, the worker thread is gone and every submitted task has a
    // terminal state. Safe to call multiple times, on Uninitialized queues,
    // and concurrently (a second caller waits until the first one finished).
    void shutdown();

    // Current queue lifecycle state (never fabricated).
    QueueState state() const noexcept;

    // Enqueues a VectorAddTask (the built-in work item). The task is
    // validated first: an invalid task never enters the queue and is refused
    // with Status::InvalidInput. On success the call returns immediately with
    // a unique TaskId — it never waits for execution.
    EnqueueResult enqueue(VectorAddTask task);

    // Enqueues any QueuedTask implementation (future task kinds; tests).
    // The queue takes ownership. Refused exactly like the typed overload
    // when the queue is not Ready; a null pointer is Status::InvalidInput.
    // Content validation happens at execution time here (the queue cannot
    // see inside a custom task), so a rejected-at-runtime task becomes
    // Failed instead of refused at enqueue.
    EnqueueResult enqueue(std::unique_ptr<QueuedTask> work);

    // Current state of one task; TaskState::Invalid for unknown ids. Never
    // blocks. Callable at any time, also after shutdown().
    TaskState task_state(TaskId id) const;

    // State + recorded result of one task (by value, lifetime-safe). For an
    // unknown id the snapshot's state is TaskState::Invalid.
    TaskSnapshot task_snapshot(TaskId id) const;

    // Blocks until the task reaches a terminal state (Completed or Failed),
    // then returns that state. Already-terminal tasks return immediately.
    // Unknown ids return TaskState::Invalid without blocking. The caller
    // must keep the queue alive while waiting (destroying a queue that
    // someone is waiting on is a caller error, like any blocking primitive).
    TaskState wait(TaskId id) const;

    // Bounded wait: returns the task state after at most 'timeout' has
    // elapsed (a state that is not terminal means "not done yet"). Unknown
    // ids return TaskState::Invalid immediately.
    template <typename Rep, typename Period>
    TaskState wait_for(TaskId id, const std::chrono::duration<Rep, Period>& timeout) const;

private:
    // Everything the queue knows about one accepted task.
    struct TaskRecord {
        TaskState state = TaskState::Queued;
        VectorAddResult result{};
    };

    // A task waiting in FIFO order: its id and the owned work item.
    struct PendingItem {
        TaskId id = kInvalidTaskId;
        std::unique_ptr<QueuedTask> work;
    };

    // Worker context. Waits on cv_work_ until a task is pending or shutdown
    // was requested, pops strictly from the front (FIFO), executes WITHOUT
    // holding the mutex, records the outcome, wakes waiters.
    void worker_loop();

    vortyx::vgpu::VirtualGpu* gpu_ = nullptr;  // non-owning; the caller's Virtual GPU

    std::thread worker_;  // joinable exactly between initialize() and shutdown()

    mutable std::mutex mutex_;         // guards everything below (const queries lock it too)
    std::condition_variable cv_work_;  // wakes the worker (new task / shutdown)
    mutable std::condition_variable cv_done_;  // wakes wait() / wait_for() / concurrent shutdown

    std::deque<PendingItem> pending_;                 // FIFO of not-yet-started tasks
    std::unordered_map<TaskId, TaskRecord> tasks_;    // every task ever accepted (this session)
    TaskId next_id_ = kFirstTaskId;                   // monotonic; never reset, never reused
    QueueState state_ = QueueState::Uninitialized;
};

template <typename Rep, typename Period>
TaskState TaskQueue::wait_for(TaskId id, const std::chrono::duration<Rep, Period>& timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_done_.wait_for(lock, timeout, [this, id] {
        const auto it = tasks_.find(id);
        return it == tasks_.end() || task_state_is_terminal(it->second.state);
    });
    const auto it = tasks_.find(id);
    return it == tasks_.end() ? TaskState::Invalid : it->second.state;
}

}  // namespace vortyx::queue
