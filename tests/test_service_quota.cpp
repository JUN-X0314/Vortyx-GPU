// Quota engine tests (Phase 14) — the ledger consistency rules.
//
// Covered: reserve/exceed per field, exactly-once release, replay without
// double charge, conflict on different dimensions, per-project isolation,
// concurrent reserve/release consistency (threads; the ledger must stay
// exact — no negative usage, no overcommit).

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "service/quota.hpp"

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
    // =====================================================================
    // 1. Basic reservation and the per-field refusals
    // =====================================================================
    {
        QuotaEngine engine;
        ProjectQuota quota;
        quota.max_concurrent_jobs = 2;
        quota.max_running_shards = 4;
        quota.max_memory_bytes = 1000;
        engine.set_quota("proj-a", quota);

        check_status(engine.reserve("proj-a", "job-1", 2, 400).status, SS::Ok, "first reserve");
        check_status(engine.reserve("proj-a", "job-2", 2, 400).status, SS::Ok, "second reserve");

        QuotaUsage usage = engine.usage("proj-a");
        check(usage.active_jobs == 2 && usage.running_shards == 4 &&
                  usage.reserved_memory_bytes == 800,
              "usage tracks the ledger");

        // Concurrent-job quota.
        QuotaEngine::Decision jobs = engine.reserve("proj-a", "job-3", 1, 100);
        check_status(jobs.status, SS::QuotaExceeded, "concurrent-job quota exceeded");
        check(jobs.error.find("concurrent-job") != std::string::npos,
              "the refusal names the field");

        // Shard quota (a fresh engine with a tighter shard bound).
        QuotaEngine shard_engine;
        ProjectQuota shard_quota;
        shard_quota.max_concurrent_jobs = 10;
        shard_quota.max_running_shards = 3;
        shard_quota.max_memory_bytes = 1000000;
        shard_engine.set_quota("proj-a", shard_quota);
        check_status(shard_engine.reserve("proj-a", "job-1", 3, 0).status, SS::Ok, "shard fit");
        QuotaEngine::Decision shards = shard_engine.reserve("proj-a", "job-2", 1, 0);
        check_status(shards.status, SS::QuotaExceeded, "shard quota exceeded");
        check(shards.error.find("shard") != std::string::npos, "the refusal names the field");

        // Memory quota.
        QuotaEngine mem_engine;
        ProjectQuota mem_quota;
        mem_quota.max_concurrent_jobs = 10;
        mem_quota.max_running_shards = 10;
        mem_quota.max_memory_bytes = 500;
        mem_engine.set_quota("proj-a", mem_quota);
        check_status(mem_engine.reserve("proj-a", "job-1", 1, 400).status, SS::Ok, "mem fit");
        QuotaEngine::Decision memory = mem_engine.reserve("proj-a", "job-2", 1, 200);
        check_status(memory.status, SS::QuotaExceeded, "memory quota exceeded");
        check(memory.error.find("memory") != std::string::npos,
              "the refusal names the field");

        // Malformed dimensions.
        QuotaEngine::Decision bad = engine.reserve("proj-a", "job-9", 0, 100);
        check_status(bad.status, SS::InvalidInput, "zero shards refused");
        bad = engine.reserve("proj-a", "job-9", 1, -1);
        check_status(bad.status, SS::InvalidInput, "negative bytes refused");
    }

    // =====================================================================
    // 2. Exactly-once release (the cancel/completion race core)
    // =====================================================================
    {
        QuotaEngine engine;
        engine.set_quota("proj-a", ProjectQuota{});
        check_status(engine.reserve("proj-a", "job-1", 2, 100).status, SS::Ok, "reserve");
        check(engine.release("job-1"), "first release succeeds");
        check(!engine.release("job-1"), "second release is REFUSED");
        check(!engine.release("job-1"), "third release refused too");
        QuotaUsage usage = engine.usage("proj-a");
        check(usage.active_jobs == 0 && usage.running_shards == 0 &&
                  usage.reserved_memory_bytes == 0,
              "usage back to zero after ONE release");
        check(!engine.has_reservation("job-1"), "no lingering reservation");

        // A released slot can be reserved again by another job.
        check_status(engine.reserve("proj-a", "job-2", 2, 100).status, SS::Ok,
                     "the freed capacity is reusable");
    }

    // =====================================================================
    // 3. Replay and conflict (retry must never double-charge)
    // =====================================================================
    {
        QuotaEngine engine;
        engine.set_quota("proj-a", ProjectQuota{1, 4, 1000});
        check_status(engine.reserve("proj-a", "job-1", 2, 100).status, SS::Ok, "reserve");
        QuotaUsage before = engine.usage("proj-a");

        QuotaEngine::Decision replay = engine.reserve("proj-a", "job-1", 2, 100);
        check_status(replay.status, SS::Ok, "same dimensions = replay");
        check(engine.usage("proj-a").active_jobs == before.active_jobs &&
                  engine.usage("proj-a").reserved_memory_bytes == before.reserved_memory_bytes,
              "replay does NOT double-charge");

        QuotaEngine::Decision conflict = engine.reserve("proj-a", "job-1", 3, 100);
        check_status(conflict.status, SS::Conflict, "different dimensions = conflict");
        conflict = engine.reserve("proj-b", "job-1", 2, 100);
        check_status(conflict.status, SS::Conflict, "different project = conflict");
    }

    // =====================================================================
    // 4. Per-project isolation and the default quota
    // =====================================================================
    {
        QuotaEngine engine;
        ProjectQuota tight;
        tight.max_concurrent_jobs = 1;
        tight.max_running_shards = 1;
        tight.max_memory_bytes = 10;
        engine.set_quota("proj-tight", tight);
        // proj-default uses the (generous) default quota.
        check_status(engine.reserve("proj-tight", "job-1", 1, 5).status, SS::Ok, "tight fit");
        check_status(engine.reserve("proj-tight", "job-2", 1, 0).status, SS::QuotaExceeded,
                     "tight exceeded");
        check_status(engine.reserve("proj-default", "job-d1", 4, 100).status, SS::Ok,
                     "default quota independent");

        check(engine.quota("proj-tight").max_concurrent_jobs == 1, "per-project quota read");
        check(engine.quota("proj-unknown").max_concurrent_jobs ==
                  ProjectQuota{}.max_concurrent_jobs,
              "unknown project falls back to the default");
    }

    // =====================================================================
    // 5. Concurrency: parallel reserves never overcommit; releases never
    //    double-credit (the race the ledger exists for)
    // =====================================================================
    {
        QuotaEngine engine;
        ProjectQuota quota;
        quota.max_concurrent_jobs = 8;   // exactly the number of winners
        quota.max_running_shards = 80;
        quota.max_memory_bytes = 8000;
        engine.set_quota("proj-a", quota);

        constexpr int kThreads = 16;   // 16 compete for 8 slots
        std::atomic<int> accepted{0};
        std::atomic<int> exceeded{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&engine, &accepted, &exceeded, t]() {
                const QuotaEngine::Decision decision =
                    engine.reserve("proj-a", "job-" + std::to_string(t), 5, 500);
                if (decision.status == SS::Ok) {
                    accepted.fetch_add(1);
                } else if (decision.status == SS::QuotaExceeded) {
                    exceeded.fetch_add(1);
                }
            });
        }
        for (std::thread& worker : workers) worker.join();
        check(accepted.load() == 8 && exceeded.load() == 8,
              "exactly the quota's worth accepted (no overcommit)");
        QuotaUsage usage = engine.usage("proj-a");
        check(usage.active_jobs == 8 && usage.running_shards == 40 &&
                  usage.reserved_memory_bytes == 4000,
              "ledger sums are exact after the race");

        // Everyone releases twice concurrently: usage must land at zero.
        std::vector<std::thread> releasers;
        std::atomic<int> successful_releases{0};
        for (int t = 0; t < kThreads; ++t) {
            releasers.emplace_back([&engine, &successful_releases, t]() {
                const std::string id = "job-" + std::to_string(t);
                if (engine.release(id)) successful_releases.fetch_add(1);
                engine.release(id);  // the second (racing) release must be refused
            });
        }
        for (std::thread& releaser : releasers) releaser.join();
        check(successful_releases.load() == 8, "exactly one release per job");
        usage = engine.usage("proj-a");
        check(usage.active_jobs == 0 && usage.running_shards == 0 &&
                  usage.reserved_memory_bytes == 0,
              "usage exactly zero after concurrent releases");
    }

    if (failures == 0) {
        std::cout << "Service quota tests passed.\n";
        return 0;
    }
    std::cerr << failures << " service quota test(s) failed.\n";
    return 1;
}
