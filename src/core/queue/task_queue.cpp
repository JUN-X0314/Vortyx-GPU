// Task Queue implementation (Phase 6).
//
// Design notes (kept next to the code they explain):
//
// Lock discipline — the whole queue is guarded by ONE mutex. The worker pops
// a task under the lock, releases it, executes, then re-acquires to record
// the result. Nothing else is ever locked while the (arbitrarily long)
// Virtual GPU execution runs, so enqueue()/task_state()/wait() stay responsive
// and no lock ordering can be violated: there is only one lock.
//
// Shutdown policy A — shutdown() flips the state to ShuttingDown (refusing
// further enqueue()), wakes the worker, and joins it OUTSIDE the mutex (the
// worker needs the mutex to drain). The worker's exit condition is
// "ShuttingDown && pending empty", so every accepted task is finished FIFO
// before the thread goes away. A concurrent second shutdown() waits on
// cv_done_ for the first one to finish instead of joining the same thread
// twice (double join would be undefined behavior).
//
// Failure philosophy — the queue never hides a failure: task execution
// errors (including a thrown exception from custom work, and a Virtual GPU
// that became unavailable) are recorded on the task as Failed with the
// honest status and message, and logged as warnings.

#include "core/queue/task_queue.hpp"

#include <exception>
#include <utility>

#include "core/logger.hpp"
#include "core/vgpu/virtual_gpu.hpp"

namespace vortyx::queue {

const char* to_string(TaskState state) {
    switch (state) {
        case TaskState::Invalid: return "Invalid";
        case TaskState::Queued: return "Queued";
        case TaskState::Running: return "Running";
        case TaskState::Completed: return "Completed";
        case TaskState::Failed: return "Failed";
    }
    return "Unknown";
}

bool task_state_is_terminal(TaskState state) {
    return state == TaskState::Completed || state == TaskState::Failed;
}

const char* to_string(QueueState state) {
    switch (state) {
        case QueueState::Uninitialized: return "Uninitialized";
        case QueueState::Ready: return "Ready";
        case QueueState::ShuttingDown: return "ShuttingDown";
        case QueueState::ShutDown: return "ShutDown";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// VectorAddQueuedTask
// ---------------------------------------------------------------------------

VectorAddQueuedTask::VectorAddQueuedTask(VectorAddTask task) : task_(std::move(task)) {}

VectorAddResult VectorAddQueuedTask::execute(vortyx::vgpu::VirtualGpu& gpu) {
    return gpu.execute(task_);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TaskQueue::~TaskQueue() {
    shutdown();
}

Status TaskQueue::initialize(vortyx::vgpu::VirtualGpu& gpu) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ == QueueState::Ready) {
        vortyx::log(vortyx::LogLevel::Error,
                    "Task Queue: initialize() refused - already initialized "
                    "(call shutdown() before re-initializing).");
        return Status::InvalidInput;
    }
    if (gpu.state() != vortyx::vgpu::State::Ready) {
        vortyx::log(vortyx::LogLevel::Error,
                    "Task Queue: initialize() refused - the bound Virtual GPU is not Ready "
                    "(state: " + std::string(vortyx::vgpu::to_string(gpu.state())) + ").");
        return Status::InvalidInput;
    }

    // Fresh session: bind the execution target and reset the bookkeeping.
    // next_id_ is deliberately NOT reset - ids are never reused, so a stale
    // id from a previous session can never alias a new task.
    gpu_ = &gpu;
    pending_.clear();
    tasks_.clear();
    state_ = QueueState::Ready;

    // Worker thread creation is the one operation that can fail (resource
    // exhaustion). Report it honestly and leave no half-initialized queue.
    try {
        worker_ = std::thread(&TaskQueue::worker_loop, this);
    } catch (const std::exception& e) {
        gpu_ = nullptr;
        state_ = QueueState::Uninitialized;
        pending_.clear();
        tasks_.clear();
        vortyx::log(vortyx::LogLevel::Error,
                    std::string("Task Queue: worker thread could not be started: ") + e.what());
        return Status::BackendError;
    }

    // Honest environment reporting: a queue bound to a known-but-unavailable
    // backend is accepted (the same policy as Virtual GPU initialize()), but
    // every task will fail with the real reason until the environment changes.
    if (!gpu.backend_available()) {
        vortyx::log(vortyx::LogLevel::Warning,
                    "Task Queue: the bound Virtual GPU's backend '" + gpu.backend_name() +
                        "' is unavailable on this system: " + gpu.backend_unavailable_reason() +
                        " - tasks will fail until a usable backend is configured.");
    }

    vortyx::log(vortyx::LogLevel::Info,
                "Task Queue initialized (worker: 1 thread, execution target: '" +
                    gpu.backend_name() + "', FIFO).");
    return Status::Ok;
}

void TaskQueue::shutdown() {
    std::unique_lock<std::mutex> lock(mutex_);

    if (state_ == QueueState::Uninitialized || state_ == QueueState::ShutDown) {
        // Nothing was ever started (or already torn down): a safe no-op that
        // still lands on ShutDown, mirroring VirtualGpu::shutdown().
        state_ = QueueState::ShutDown;
        return;
    }
    if (state_ == QueueState::ShuttingDown) {
        // A concurrent shutdown() is already draining and joining. Waiting
        // here (instead of joining the same thread again) keeps the
        // "returns only after the worker is gone" contract for every caller.
        cv_done_.wait(lock, [this] { return state_ == QueueState::ShutDown; });
        return;
    }

    // Ready -> ShuttingDown: refuse further enqueue(), wake the worker so it
    // can drain the remaining FIFO tasks and exit.
    state_ = QueueState::ShuttingDown;
    cv_work_.notify_all();

    // Join OUTSIDE the mutex: the worker needs it to finish the drain.
    std::thread worker = std::move(worker_);
    lock.unlock();
    if (worker.joinable()) {
        worker.join();
    }
    lock.lock();

    gpu_ = nullptr;
    state_ = QueueState::ShutDown;
    cv_done_.notify_all();

    vortyx::log(vortyx::LogLevel::Info,
                "Task Queue shut down (worker joined, all accepted tasks processed).");
}

QueueState TaskQueue::state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

// ---------------------------------------------------------------------------
// Enqueue
// ---------------------------------------------------------------------------

EnqueueResult TaskQueue::enqueue(VectorAddTask task) {
    // Fail fast: the Runtime would reject an invalid task at execution time
    // anyway; refusing it here keeps the queue free of dead entries. The
    // Runtime's own validation rules are used unchanged.
    const Status validation = vortyx::compute::validate_vector_add(task);
    if (validation != Status::Ok) {
        return EnqueueResult{
            kInvalidTaskId, validation,
            "invalid VectorAddTask refused at enqueue() (" + std::string(to_string(validation)) +
                ": size mismatch or empty input)"};
    }
    return enqueue(std::unique_ptr<QueuedTask>(
        std::make_unique<VectorAddQueuedTask>(std::move(task))));
}

EnqueueResult TaskQueue::enqueue(std::unique_ptr<QueuedTask> work) {
    if (!work) {
        return EnqueueResult{kInvalidTaskId, Status::InvalidInput,
                             "enqueue() called without a task (null)"};
    }

    std::lock_guard<std::mutex> lock(mutex_);

    switch (state_) {
        case QueueState::Uninitialized:
            return EnqueueResult{kInvalidTaskId, Status::NotInitialized,
                                 "Task Queue is not initialized (call initialize() before enqueue())"};
        case QueueState::ShuttingDown:
            return EnqueueResult{kInvalidTaskId, Status::NotInitialized,
                                 "Task Queue is shutting down; no new tasks are accepted"};
        case QueueState::ShutDown:
            return EnqueueResult{kInvalidTaskId, Status::NotInitialized,
                                 "Task Queue is shut down (call initialize() again before enqueue())"};
        case QueueState::Ready:
            break;
    }

    // Ids are never reused; the wrap check makes the (theoretical) overflow
    // of the 64-bit counter an explicit refusal instead of a collision.
    if (next_id_ == kInvalidTaskId) {
        return EnqueueResult{kInvalidTaskId, Status::BackendError,
                             "task id space exhausted (ids are never reused)"};
    }

    const TaskId id = next_id_++;
    pending_.push_back(PendingItem{id, std::move(work)});
    tasks_.emplace(id, TaskRecord{TaskState::Queued, VectorAddResult{}});
    cv_work_.notify_one();

    return EnqueueResult{id, Status::Ok, std::string{}};
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

TaskState TaskQueue::task_state(TaskId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(id);
    return it == tasks_.end() ? TaskState::Invalid : it->second.state;
}

TaskSnapshot TaskQueue::task_snapshot(TaskId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return TaskSnapshot{TaskState::Invalid, VectorAddResult{}};
    }
    // By-value copy: the caller's snapshot never aliases queue internals.
    return TaskSnapshot{it->second.state, it->second.result};
}

TaskState TaskQueue::wait(TaskId id) const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_done_.wait(lock, [this, id] {
        const auto it = tasks_.find(id);
        return it == tasks_.end() || task_state_is_terminal(it->second.state);
    });
    const auto it = tasks_.find(id);
    return it == tasks_.end() ? TaskState::Invalid : it->second.state;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void TaskQueue::worker_loop() {
    for (;;) {
        PendingItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // No busy-waiting: sleep until a task is pending or shutdown was
            // requested. Spurious wakeups re-evaluate the predicate.
            cv_work_.wait(lock, [this] {
                return state_ == QueueState::ShuttingDown || !pending_.empty();
            });
            if (pending_.empty()) {
                // ShuttingDown and nothing left: policy A drain is complete.
                return;
            }
            item = std::move(pending_.front());
            pending_.pop_front();
            const auto it = tasks_.find(item.id);
            if (it != tasks_.end()) {
                it->second.state = TaskState::Running;
            }
        }
        // The mutex is NOT held during execution: a long compute run must
        // never block enqueue()/state queries, and nothing below this layer
        // can deadlock against the queue's lock.

        VectorAddResult result{Status::BackendError, "task was not executed", {}};
        if (gpu_ == nullptr) {
            // Unreachable by construction (gpu_ is only cleared after the
            // worker is joined) - kept defensive so the worker can never
            // dereference null even if future changes break that invariant.
            result = VectorAddResult{Status::NotInitialized,
                                     "Task Queue has no bound Virtual GPU", {}};
        } else {
            try {
                result = item.work->execute(*gpu_);
            } catch (const std::exception& e) {
                // Custom work items may throw; the worker survives and the
                // task fails honestly instead of killing the process.
                result = VectorAddResult{
                    Status::BackendError,
                    std::string("queued task terminated by an exception: ") + e.what(), {}};
                vortyx::log(vortyx::LogLevel::Warning,
                            "Task Queue: task " + std::to_string(item.id) +
                                " threw an exception.");
            } catch (...) {
                result = VectorAddResult{Status::BackendError,
                                         "queued task terminated by an unknown exception", {}};
                vortyx::log(vortyx::LogLevel::Warning,
                            "Task Queue: task " + std::to_string(item.id) +
                                " threw an unknown exception.");
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = tasks_.find(item.id);
            if (it != tasks_.end()) {
                it->second.result = std::move(result);
                it->second.state = (it->second.result.status == Status::Ok)
                                       ? TaskState::Completed
                                       : TaskState::Failed;
                if (it->second.state == TaskState::Failed) {
                    vortyx::log(vortyx::LogLevel::Warning,
                                "Task Queue: task " + std::to_string(item.id) + " failed: " +
                                    to_string(it->second.result.status) + " - " +
                                    it->second.result.error);
                }
            }
        }
        cv_done_.notify_all();
    }
}

}  // namespace vortyx::queue
