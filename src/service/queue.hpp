#pragma once

// Provider-neutral service job queue (Phase 14).
//
// The seam between job submission and scheduler handoff. The facade inserts
// accepted submissions; dispatcher threads drain it and hand each job to the
// EXISTING Phase 12 orchestrator (the queue never executes anything and
// never talks to devices — scheduling stays where Phase 12 put it).
//
// ORDERING: FIFO by enqueue order. The JobEnvelope's 'priority' field stays
// UNINTERPRETED (the Phase 11 rule: it is a reserved transport field; giving
// it scheduling semantics now would fake a capability Phase 11 explicitly
// deferred). A future phase may add priority by extending enqueue — the
// interface leaves room without changing callers.
//
// IDEMPOTENCY: enqueueing a job id already waiting is a replay (Ok, created
// == false) — a duplicate submission can never occupy two queue slots.
// remove() is the cancel-in-queue path: it succeeds exactly once per queued
// entry.
//
// Thread-safe (dispatcher threads and submitters share it).

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "platform/identity.hpp"
#include "service/service_status.hpp"

namespace vortyx::service {

struct QueuedJob {
    vortyx::platform::JobId job_id;
    std::int64_t enqueued_at_ms = 0;  // the facade's clock (observability)
};

class IJobQueue {
public:
    virtual ~IJobQueue() = default;

    // Enqueues one job. Replay of an already-queued id: Ok with
    // created == false. Errors: InvalidInput (empty id) | Conflict (never —
    // replays are Ok; reserved for future keyed semantics) | Internal.
    virtual ServiceStatus enqueue(const QueuedJob& job, bool& created) = 0;

    // Dequeues the next job in FIFO order. False when the queue is empty
    // (a non-blocking probe — the dispatcher waits on its own condition
    // variable, never by spinning here).
    virtual bool try_dequeue(QueuedJob& out) = 0;

    // Removes a queued job (cancel-in-queue). True when the job was queued
    // and is now removed; false when it was not queued (already dequeued /
    // unknown). Exactly-once per entry.
    virtual bool remove(const vortyx::platform::JobId& job_id) = 0;

    // True while the id is waiting in the queue.
    virtual bool contains(const vortyx::platform::JobId& job_id) const = 0;

    // Current depth (observability; a consistent instant value).
    virtual std::size_t depth() const = 0;

    // Snapshot in dequeue order (observability / tests).
    virtual std::vector<QueuedJob> snapshot() const = 0;
};

// The local reference implementation (bounded by the facade's capacity
// limit — enqueue past capacity refuses with ServiceStatus::Unavailable and
// names the limit; a service-level resource-exhaustion guard).
class InMemoryJobQueue final : public IJobQueue {
public:
    explicit InMemoryJobQueue(std::size_t capacity);

    ServiceStatus enqueue(const QueuedJob& job, bool& created) override;
    bool try_dequeue(QueuedJob& out) override;
    bool remove(const vortyx::platform::JobId& job_id) override;
    bool contains(const vortyx::platform::JobId& job_id) const override;
    std::size_t depth() const override;
    std::vector<QueuedJob> snapshot() const override;

    std::size_t capacity() const { return capacity_; }

private:
    std::size_t capacity_;
    std::vector<QueuedJob> items_;  // FIFO (front = next out)
    mutable std::mutex mutex_;
};

}  // namespace vortyx::service
