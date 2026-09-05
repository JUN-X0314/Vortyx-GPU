// Job queue tests (Phase 14) — the provider-neutral queue contract.
//
// FIFO ordering, idempotent enqueue (replay never occupies two slots),
// exactly-once removal (the cancel-in-queue path), capacity refusal (the
// service-level resource limit), depth/snapshot observability.

#include <iostream>
#include <string>

#include "service/queue.hpp"

using namespace vortyx::service;
using SS = ServiceStatus;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_status(SS actual, SS expected, const std::string& message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " (expected " << to_string(expected) << ", got "
                  << to_string(actual) << ")\n";
        ++failures;
    }
}

}  // namespace

int main() {
    // 1. FIFO order and idempotent enqueue.
    {
        InMemoryJobQueue queue(4);
        bool created = false;

        QueuedJob first;
        first.job_id = "job-1";
        first.enqueued_at_ms = 10;
        check_status(queue.enqueue(first, created), SS::Ok, "enqueue first");
        check(created, "first enqueue created");

        check_status(queue.enqueue(first, created), SS::Ok, "replay enqueue Ok");
        check(!created, "replay did not create");
        check(queue.depth() == 1, "still one entry (no duplicate slot)");

        QueuedJob second;
        second.job_id = "job-2";
        second.enqueued_at_ms = 20;
        check_status(queue.enqueue(second, created), SS::Ok, "enqueue second");

        check(queue.contains("job-1") && !queue.contains("job-9"),
              "contains reflects reality");

        QueuedJob out;
        check(queue.try_dequeue(out) && out.job_id == "job-1" && out.enqueued_at_ms == 10,
              "FIFO dequeue order");
        check(queue.try_dequeue(out) && out.job_id == "job-2", "second in order");
        check(!queue.try_dequeue(out), "empty queue refuses");
    }

    // 2. Exactly-once removal (the cancel-in-queue path).
    {
        InMemoryJobQueue queue(8);
        bool created = false;
        for (int i = 0; i < 3; ++i) {
            QueuedJob job;
            job.job_id = "job-" + std::to_string(i);
            queue.enqueue(job, created);
        }
        check(queue.remove("job-1"), "middle job removed");
        check(!queue.remove("job-1"), "second removal refused (exactly-once)");
        check(!queue.contains("job-1") && queue.depth() == 2, "queue consistent");

        // Order preserved after the middle removal.
        QueuedJob out;
        check(queue.try_dequeue(out) && out.job_id == "job-0", "order preserved");
        check(queue.try_dequeue(out) && out.job_id == "job-2", "order preserved (tail)");
    }

    // 3. Capacity refusal (the resource-exhaustion guard).
    {
        InMemoryJobQueue tiny(1);
        bool created = false;
        QueuedJob a;
        a.job_id = "a";
        QueuedJob b;
        b.job_id = "b";
        check_status(tiny.enqueue(a, created), SS::Ok, "capacity fit");
        check_status(tiny.enqueue(b, created), SS::Unavailable, "capacity exceeded refused");
        check(tiny.depth() == 1, "the refusal inserted nothing");
        check(tiny.capacity() == 1, "capacity readable");
    }

    // 4. Validation and snapshot.
    {
        InMemoryJobQueue queue(8);
        bool created = false;
        QueuedJob empty;
        check_status(queue.enqueue(empty, created), SS::InvalidInput, "empty id refused");

        InMemoryJobQueue snap(8);
        for (int i = 0; i < 3; ++i) {
            QueuedJob job;
            job.job_id = "job-" + std::to_string(i);
            bool c = false;
            snap.enqueue(job, c);
        }
        std::vector<QueuedJob> items = snap.snapshot();
        check(items.size() == 3 && items[0].job_id == "job-0" && items[2].job_id == "job-2",
              "snapshot is dequeue-ordered");
    }

    if (failures == 0) {
        std::cout << "Service queue tests passed.\n";
        return 0;
    }
    std::cerr << failures << " service queue test(s) failed.\n";
    return 1;
}
