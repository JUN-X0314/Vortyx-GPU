// Distributed foundation tests (Phase 12) — device model, resource model,
// registry (idempotent registration, leases, expiry, revision), the lease
// guard and the heartbeat monitor.
//
// Every time-dependent behavior runs on an injected FakeClock: no sleeps,
// no flaky timing. Ownership rules mirror the Phase 11 platform layer
// (foreign records are invisible -> NotFound).

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "distributed/distributed.hpp"

using namespace vortyx::distributed;
using vortyx::platform::Status;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

DeviceCapabilities caps(std::int64_t memory_mb, std::int64_t jobs,
                        const std::vector<std::string>& backends = {"cpu"}) {
    DeviceCapabilities c;
    c.metadata.protocol_version = vortyx::platform::kProtocolVersion;
    c.metadata.software_version = "0.12.0";
    c.metadata.operating_system = "linux";
    c.metadata.architecture = "x86_64";
    c.metadata.backends = backends;
    c.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
    c.metadata.display_name = "test-device";
    c.capacity.memory_bytes = memory_mb * 1024 * 1024;
    c.capacity.concurrent_jobs = jobs;
    c.max_concurrent_shards = jobs;
    return c;
}

}  // namespace

int main() {
    FakeClock clock(1000);

    // =====================================================================
    // 1. Device state machine: the documented transition table
    // =====================================================================
    {
        // Legal transitions (from the table in device.hpp).
        check(device_state_transition_valid(DeviceState::Registering, DeviceState::Ready),
              "registering -> ready is legal");
        check(device_state_transition_valid(DeviceState::Registering, DeviceState::Failed),
              "registering -> failed is legal");
        check(device_state_transition_valid(DeviceState::Registering, DeviceState::Offline),
              "registering -> offline is legal");
        check(device_state_transition_valid(DeviceState::Ready, DeviceState::Busy),
              "ready -> busy is legal");
        check(device_state_transition_valid(DeviceState::Ready, DeviceState::Draining),
              "ready -> draining is legal");
        check(device_state_transition_valid(DeviceState::Ready, DeviceState::Offline),
              "ready -> offline is legal");
        check(device_state_transition_valid(DeviceState::Ready, DeviceState::Failed),
              "ready -> failed is legal");
        check(device_state_transition_valid(DeviceState::Busy, DeviceState::Ready),
              "busy -> ready is legal");
        check(device_state_transition_valid(DeviceState::Draining, DeviceState::Offline),
              "draining -> offline is legal");
        check(device_state_transition_valid(DeviceState::Offline, DeviceState::Ready),
              "offline -> ready (heartbeat proof) is legal");
        check(device_state_transition_valid(DeviceState::Offline, DeviceState::Registering),
              "offline -> registering (re-registration) is legal");
        check(device_state_transition_valid(DeviceState::Failed, DeviceState::Offline),
              "failed -> offline (retire) is legal");
        check(device_state_transition_valid(DeviceState::Failed, DeviceState::Registering),
              "failed -> registering (remediation) is legal");

        // Illegal transitions (the table refuses silent revival).
        check(!device_state_transition_valid(DeviceState::Offline, DeviceState::Busy),
              "offline -> busy is refused");
        check(!device_state_transition_valid(DeviceState::Failed, DeviceState::Ready),
              "failed -> ready is refused (no silent revival)");
        check(!device_state_transition_valid(DeviceState::Draining, DeviceState::Ready),
              "draining -> ready is refused");
        check(!device_state_transition_valid(DeviceState::Draining, DeviceState::Busy),
              "draining -> busy is refused");
        check(!device_state_transition_valid(DeviceState::Ready, DeviceState::Ready),
              "self-transition ready -> ready is refused (state changes are events)");
        check(!device_state_transition_valid(DeviceState::Registering, DeviceState::Busy),
              "registering -> busy is refused");

        // Schedulability.
        check(device_state_schedulable(DeviceState::Ready), "ready is schedulable");
        check(device_state_schedulable(DeviceState::Busy),
              "busy is schedulable (capacity permitting)");
        check(!device_state_schedulable(DeviceState::Draining), "draining is not schedulable");
        check(!device_state_schedulable(DeviceState::Offline), "offline is not schedulable");
        check(!device_state_schedulable(DeviceState::Failed), "failed is not schedulable");
        check(!device_state_schedulable(DeviceState::Registering),
              "registering is not schedulable");

        // Vocabulary strings are stable.
        check(std::string(to_string(DeviceState::Ready)) == "ready", "ready label");
        check(std::string(to_string(DeviceHealth::Unhealthy)) == "unhealthy",
              "unhealthy label");
    }

    // =====================================================================
    // 2. Capability validation + matching (unknown capability never matches)
    // =====================================================================
    {
        std::string error;
        DeviceCapabilities good = caps(8, 2);
        check(validate_device_capabilities(good, error) == Status::Ok, "valid caps accepted");

        DeviceCapabilities bad_protocol = caps(8, 2);
        bad_protocol.metadata.protocol_version = "999";
        check(validate_device_capabilities(bad_protocol, error) == Status::InvalidInput,
              "wrong protocol version refused");

        DeviceCapabilities unknown_backend = caps(8, 2);
        unknown_backend.metadata.backends = {"cuda"};
        check(validate_device_capabilities(unknown_backend, error) == Status::InvalidInput,
              "unknown backend name refused");

        DeviceCapabilities unknown_op = caps(8, 2);
        unknown_op.metadata.operations = {"matmul"};
        check(validate_device_capabilities(unknown_op, error) == Status::InvalidInput,
              "unknown operation label refused");

        DeviceCapabilities negative = caps(8, 2);
        negative.capacity.memory_bytes = -1;
        check(validate_device_capabilities(negative, error) == Status::InvalidInput,
              "negative capacity refused");

        DeviceCapabilities no_ops = caps(8, 2);
        no_ops.metadata.operations.clear();
        check(!device_supports(no_ops, vortyx::compute::ComputeOp::VectorAdd, "cpu"),
              "a device claiming NOTHING supports nothing (unknown != support)");

        DeviceCapabilities claimed = caps(8, 2);
        check(device_supports(claimed, vortyx::compute::ComputeOp::VectorAdd, "cpu"),
              "claimed op+backend matches");
        check(!device_supports(claimed, vortyx::compute::ComputeOp::VectorAdd, "vulkan"),
              "unclaimed backend does not match");
        check(device_supports(claimed, vortyx::compute::ComputeOp::VectorScale, ""),
              "empty backend preference matches any claimed op");

        check(std::string(claimed.preferred_backend()) == "cpu",
              "preferred backend is the first claimed backend");
    }

    // =====================================================================
    // 3. Resource model invariants
    // =====================================================================
    {
        ResourceVector zero;
        check(resource_vector_valid(zero), "zero vector is valid");
        ResourceVector negative;
        negative.memory_bytes = -5;
        check(!resource_vector_valid(negative), "negative fields are invalid");

        ResourceVector capacity;
        capacity.memory_bytes = 1000;
        capacity.concurrent_jobs = 2;
        ResourceVector used;
        used.memory_bytes = 400;
        used.concurrent_jobs = 1;
        ResourceVector fits_exactly;
        fits_exactly.memory_bytes = 600;
        fits_exactly.concurrent_jobs = 1;
        check(resource_vector_fits(capacity, used, fits_exactly),
              "used + request == capacity fits (boundary is inclusive)");
        ResourceVector over;
        over.memory_bytes = 601;
        check(!resource_vector_fits(capacity, used, over), "over-capacity refused");
        ResourceVector over_jobs;
        over_jobs.memory_bytes = 100;
        over_jobs.concurrent_jobs = 2;
        check(!resource_vector_fits(capacity, used, over_jobs),
              "concurrency over-capacity refused");

        ResourceVector sum = resource_vector_add(used, fits_exactly);
        check(sum.memory_bytes == 1000 && sum.concurrent_jobs == 2, "add is component-wise");
        ResourceVector diff = resource_vector_sub(sum, fits_exactly);
        check(diff.memory_bytes == 400 && diff.concurrent_jobs == 1, "sub is component-wise");
        ResourceVector under = resource_vector_sub(fits_exactly, capacity);
        check(under.memory_bytes == 0 && under.concurrent_jobs == 0,
              "sub clamps at zero (release never goes negative)");
        check(resource_vector_le(used, capacity), "le holds for used <= capacity");
        check(!resource_vector_le(capacity, used), "le refuses used > capacity");

        std::int64_t bytes = 0;
        std::string error;
        check(shard_memory_bytes(1000, vortyx::compute::ComputeOp::VectorAdd, bytes, error) &&
                  bytes == 1000 * 4 * 3,
              "vector_add shard = 3 buffers of 4 bytes/element");
        check(shard_memory_bytes(1000, vortyx::compute::ComputeOp::VectorScale, bytes, error) &&
                  bytes == 1000 * 4 * 2,
              "vector_scale shard = 2 buffers");
        check(!shard_memory_bytes(UINT64_MAX, vortyx::compute::ComputeOp::VectorAdd, bytes, error),
              "absurd element counts are refused, never wrapped");

        check(to_string(capacity) == "compute=0 memory=1000 jobs=2",
              "resource debug string is stable");
    }

    // =====================================================================
    // 4. Registry: registration, idempotency, conflicts, ownership
    // =====================================================================
    {
        LocalDeviceRegistry registry(std::make_shared<FakeClock>(clock));

        DeviceDescriptor out;
        bool created = false;

        check(registry.register_device("dev-1", "user-a", caps(8, 2), out, created) ==
                      Status::Ok &&
                  created,
              "first registration creates");
        check(out.state == DeviceState::Registering && out.health == DeviceHealth::Unknown,
              "a fresh device is Registering/Unknown (not fabricated as ready)");

        // Idempotent replay: same owner + identical payload.
        DeviceDescriptor replay;
        bool replay_created = true;
        check(registry.register_device("dev-1", "user-a", caps(8, 2), replay, replay_created) ==
                      Status::Ok &&
                  !replay_created,
              "identical re-registration is an idempotent replay");
        check(replay.state == DeviceState::Ready && replay.health == DeviceHealth::Healthy,
              "the replay records liveness and activates a Registering device (re-join)");

        // Conflicts never reveal the existing owner.
        DeviceDescriptor conflicting;
        check(registry.register_device("dev-1", "user-b", caps(8, 2), conflicting,
                                       replay_created) == Status::Conflict,
              "same id, different owner -> Conflict");
        check(registry.register_device("dev-1", "user-a", caps(16, 2), conflicting,
                                       replay_created) == Status::Conflict,
              "same id, different payload -> Conflict");

        // Ownership-scoped visibility (foreign -> NotFound, anti-enumeration).
        DeviceDescriptor foreign;
        check(registry.device("user-b", "dev-1", foreign) == Status::NotFound,
              "a foreign device is invisible (NotFound, not Forbidden)");
        check(registry.device("user-a", "dev-x", foreign) == Status::NotFound,
              "an unknown device is NotFound");

        std::vector<DeviceDescriptor> listing;
        registry.register_device("dev-2", "user-a", caps(16, 1), out, created);
        registry.register_device("dev-3", "user-a", caps(32, 4), out, created);
        registry.devices("user-a", listing);
        check(listing.size() == 3 && listing[0].device_id == "dev-1" &&
                  listing[2].device_id == "dev-3",
              "listing is in registration order (deterministic)");
        registry.devices("user-b", listing);
        check(listing.empty(), "foreign listings are empty");

        // Activation through the transition table on a FRESH device.
        DeviceDescriptor fresh;
        registry.register_device("dev-4", "user-a", caps(4, 1), fresh, created);
        check(registry.update_device_state("user-a", "dev-4", DeviceState::Ready) == Status::Ok,
              "registering -> ready activates");
        check(registry.update_device_state("user-a", "dev-4", DeviceState::Registering) ==
                  Status::InvalidInput,
              "ready -> registering is refused by the table");

        // The documented recovery: a heartbeat brings an Offline device back.
        registry.update_device_state("user-a", "dev-4", DeviceState::Offline);
        registry.heartbeat_device("user-a", "dev-4");
        registry.device("user-a", "dev-4", fresh);
        check(fresh.state == DeviceState::Ready && fresh.health == DeviceHealth::Healthy,
              "a heartbeat recovers an offline device (the proof-of-life path)");

        // Unregister: foreign invisible; active leases pin; owner succeeds.
        DeviceLease lease;
        std::string error;
        ResourceVector request;
        request.memory_bytes = 1024;
        request.concurrent_jobs = 1;
        registry.heartbeat_device("user-a", "dev-2");  // stamp + health before reservation
        check(registry.reserve("user-a", "dev-2", "job-1", "job-1-s0", request, 60000, lease,
                               error) == Status::Ok,
              "reservation on dev-2 succeeds");
        check(registry.unregister_device("user-b", "dev-2") == Status::NotFound,
              "foreign unregister is invisible");
        check(registry.unregister_device("user-a", "dev-2") == Status::InvalidInput,
              "unregister with an active lease is refused (explicit leak rule)");
        check(registry.release_lease(lease) == Status::Ok, "release succeeds");
        check(registry.unregister_device("user-a", "dev-2") == Status::Ok,
              "unregister after release succeeds");

        // Capability updates under active leases are refused.
        registry.register_device("dev-5", "user-a", caps(16, 1), out, created);
        registry.update_device_state("user-a", "dev-5", DeviceState::Ready);
        registry.heartbeat_device("user-a", "dev-5");
        check(registry.reserve("user-a", "dev-5", "job-2", "job-2-s0", request, 60000, lease,
                               error) == Status::Ok,
              "reservation for capability test");
        check(registry.update_device_capabilities("user-a", "dev-5", caps(64, 8)) ==
                  Status::Conflict,
              "capability change under a live lease is Conflict (no silent shrink)");
        registry.release_lease(lease);
        check(registry.update_device_capabilities("user-a", "dev-5", caps(64, 8)) == Status::Ok,
              "capability change after release succeeds");
    }

    // =====================================================================
    // 5. Registry: reservation gates, release integrity, expiry (FakeClock)
    // =====================================================================
    {
        auto test_clock = std::make_shared<FakeClock>(5000);
        LocalDeviceRegistry registry(test_clock);

        DeviceDescriptor out;
        bool created = false;
        registry.register_device("dev", "user", caps(10, 2), out, created);
        registry.update_device_state("user", "dev", DeviceState::Ready);
        registry.heartbeat_device("user", "dev");  // the health evidence

        std::string error;
        DeviceLease first;
        ResourceVector request;
        request.memory_bytes = 6 * 1024 * 1024;
        request.concurrent_jobs = 1;

        check(registry.reserve("user", "dev", "job", "job-s0", request, 1000, first, error) ==
                  Status::Ok,
              "first reservation fits");
        check(first.lease_id == "lease-1", "lease ids are deterministic");
        check(first.expires_at_ms == first.created_at_ms + 1000, "expiry = created + ttl");

        DeviceLease second;
        ResourceVector too_big;
        too_big.memory_bytes = 6 * 1024 * 1024;
        too_big.concurrent_jobs = 1;
        check(registry.reserve("user", "dev", "job", "job-s1", too_big, 1000, second, error) ==
                  Status::InvalidInput &&
              error.find("memory") != std::string::npos,
              "over-memory reservation is refused with the honest reason");
        ResourceVector too_many;
        too_many.memory_bytes = 1024;
        too_many.concurrent_jobs = 2;
        check(registry.reserve("user", "dev", "job", "job-s1", too_many, 1000, second, error) ==
                  Status::InvalidInput &&
              error.find("concurrency") != std::string::npos,
              "over-concurrency reservation is refused with the honest reason");

        // Release integrity: a mismatched record is refused; double release
        // is refused; the capacity math stays exact.
        DeviceLease altered = first;
        altered.resources.memory_bytes += 1;
        check(registry.release_lease(altered) == Status::InvalidInput,
              "a mismatched lease record is refused");
        check(registry.release_lease(first) == Status::Ok, "the true record releases");
        check(registry.release_lease(first) == Status::InvalidInput,
              "double release is refused (never a double free)");
        check(registry.lease("lease-1", second) == Status::NotFound,
              "released leases leave the registry (no bookkeeping residue)");

        // Busy devices still reserve (capacity permitting); Offline refuse.
        registry.reserve("user", "dev", "job", "job-s0", request, 1000, first, error);
        registry.update_device_state("user", "dev", DeviceState::Busy);
        ResourceVector one_job;
        one_job.concurrent_jobs = 1;
        one_job.memory_bytes = 1024;
        check(registry.reserve("user", "dev", "job", "job-s1", one_job, 1000, second, error) ==
                  Status::Ok,
              "a busy device is schedulable while capacity remains");
        registry.update_device_state("user", "dev", DeviceState::Draining);
        check(registry.reserve("user", "dev", "job", "job-s2", one_job, 1000, second, error) ==
                  Status::InvalidInput,
              "a draining device accepts no reservations");
        registry.update_device_state("user", "dev", DeviceState::Offline);
        check(registry.reserve("user", "dev", "job", "job-s2", one_job, 1000, second, error) ==
                  Status::InvalidInput,
              "an offline device accepts no reservations");

        // Expiry: deterministic with the injected clock.
        registry.update_device_state("user", "dev", DeviceState::Ready);
        FakeClock* fake = test_clock.get();
        fake->set(60000);  // every lease above has long expired
        const std::size_t reclaimed = registry.expire_leases(fake->now_ms());
        check(reclaimed >= 2, "expired leases are reclaimed");
        DeviceDescriptor after;
        registry.device("user", "dev", after);
        check(after.allocated.memory_bytes == 0 && after.allocated.concurrent_jobs == 0,
              "expired leases free their capacity (no permanent leak)");
        DeviceLease fresh;
        check(registry.reserve("user", "dev", "job", "job-s3", request, 1000, fresh, error) ==
                  Status::Ok,
              "capacity is available again after expiry");
    }

    // =====================================================================
    // 6. LeaseGuard: RAII release on every path
    // =====================================================================
    {
        auto test_clock = std::make_shared<FakeClock>(100);
        LocalDeviceRegistry registry(test_clock);

        DeviceDescriptor out;
        bool created = false;
        registry.register_device("dev", "user", caps(10, 2), out, created);
        registry.update_device_state("user", "dev", DeviceState::Ready);
        registry.heartbeat_device("user", "dev");  // the health evidence

        std::string error;
        DeviceLease lease;
        ResourceVector request;
        request.memory_bytes = 1024;
        request.concurrent_jobs = 1;
        registry.reserve("user", "dev", "job", "job-s0", request, 60000, lease, error);

        {
            LeaseGuard guard(&registry, lease);
            check(guard.holding(), "guard holds the lease");
            DeviceDescriptor during;
            registry.device("user", "dev", during);
            check(during.allocated.memory_bytes == 1024, "capacity is held while guarded");
        }  // scope exit -> RAII release
        DeviceDescriptor after;
        registry.device("user", "dev", after);
        check(after.allocated.memory_bytes == 0, "guard destruction released the capacity");

        // detach: ownership handover without release.
        registry.reserve("user", "dev", "job", "job-s1", request, 60000, lease, error);
        DeviceLease handed_over;
        {
            LeaseGuard guard(&registry, lease);
            handed_over = guard.detach();
            check(!guard.holding(), "detached guard no longer holds");
        }
        DeviceDescriptor after_detach;
        registry.device("user", "dev", after_detach);
        check(after_detach.allocated.memory_bytes == 1024,
              "detached lease survives the guard");
        registry.release_lease(handed_over);
    }

    // =====================================================================
    // 7. Heartbeat monitor: judgments on the injected clock
    // =====================================================================
    {
        auto test_clock = std::make_shared<FakeClock>(1000);
        LocalDeviceRegistry registry(test_clock);

        DeviceDescriptor out;
        bool created = false;
        registry.register_device("dev-a", "user", caps(8, 2), out, created);
        registry.register_device("dev-b", "user", caps(8, 2), out, created);
        registry.update_device_state("user", "dev-a", DeviceState::Ready);
        registry.update_device_state("user", "dev-b", DeviceState::Ready);

        HeartbeatMonitor monitor(registry, test_clock, 5000);
        check(monitor.valid(), "monitor parameters valid");

        FakeClock* fake = test_clock.get();
        check(monitor.check("user") == 0, "fresh devices are not timed out");

        fake->advance(3000);
        check(monitor.check("user") == 0, "within the timeout: still fresh");

        fake->advance(2500);  // last stamp 1000, now 6500: stale by 5500 > 5000
        check(monitor.check("user") == 2, "stale devices are newly judged");
        DeviceDescriptor judged;
        registry.device("user", "dev-a", judged);
        check(judged.state == DeviceState::Offline &&
                  judged.health == DeviceHealth::Unhealthy,
              "a timed-out device is Unhealthy and Offline");

        check(monitor.check("user") == 0, "a second check finds nothing NEW");
        check(monitor.total_timeouts_observed() == 2, "the counter counts once per device");

        // Recovery: a heartbeat is the proof-of-life path.
        registry.heartbeat_device("user", "dev-a");
        registry.device("user", "dev-a", judged);
        check(judged.state == DeviceState::Ready && judged.health == DeviceHealth::Healthy,
              "a heartbeat recovers an offline device (documented path)");
        fake->advance(1000);
        check(monitor.check("user") == 0, "the recovered device is fresh again");

        // A Failed device is left to its own path (the monitor judges
        // health but never moves a non-schedulable state).
        registry.register_device("dev-c", "user", caps(8, 2), out, created);
        registry.update_device_state("user", "dev-c", DeviceState::Ready);
        registry.update_device_state("user", "dev-c", DeviceState::Failed);
        fake->advance(100000);
        monitor.check("user");
        registry.device("user", "dev-c", judged);
        check(judged.state == DeviceState::Failed,
              "a failed device is not silently revived by the monitor");

        // Ownership: the monitor sees only the asked owner's devices.
        registry.register_device("dev-c", "other-user", caps(8, 2), out, created);
        registry.update_device_state("other-user", "dev-c", DeviceState::Ready);
        check(monitor.check("other-user") == 0, "another owner's fresh device is untouched");
    }

    // =====================================================================
    // 8. Snapshot: revision monotonicity + candidate filtering
    // =====================================================================
    {
        auto test_clock = std::make_shared<FakeClock>(100);
        LocalDeviceRegistry registry(test_clock);

        const std::uint64_t revision0 = registry.revision();
        DeviceDescriptor out;
        bool created = false;
        registry.register_device("dev-1", "user-a", caps(8, 2), out, created);
        const std::uint64_t revision1 = registry.revision();
        check(revision1 > revision0, "mutations bump the revision");
        registry.register_device("dev-2", "user-b", caps(8, 2), out, created);
        const std::uint64_t revision2 = registry.revision();
        check(revision2 > revision1, "revision is monotonically increasing");

        ClusterSnapshot snapshot = registry.snapshot();
        check(snapshot.revision == revision2, "snapshot carries the current revision");
        check(snapshot.devices.size() == 2, "snapshot carries every device");
        check(snapshot.candidates_for("user-a").empty(),
              "registering devices are not candidates");
        registry.update_device_state("user-a", "dev-1", DeviceState::Ready);
        registry.set_device_health("user-a", "dev-1", DeviceHealth::Healthy);
        snapshot = registry.snapshot();
        check(snapshot.candidates_for("user-a").size() == 1,
              "a ready+healthy owned device is a candidate");
        check(snapshot.candidates_for("user-b").size() == 0,
              "a foreign owner sees no candidates from someone else's devices");
        check(!snapshot.empty_for("user-a") && snapshot.empty_for("user-c"),
              "empty_for reflects the owner's view");

        // An unhealthy device is excluded even when Ready.
        registry.set_device_health("user-a", "dev-1", DeviceHealth::Unhealthy);
        snapshot = registry.snapshot();
        check(snapshot.candidates_for("user-a").empty(),
              "unhealthy devices are excluded from candidacy");
    }

    // =====================================================================
    // 9. Concurrency: parallel registration, lookup, reservation
    // =====================================================================
    {
        auto test_clock = std::make_shared<FakeClock>(100);
        LocalDeviceRegistry registry(test_clock);

        constexpr int kThreads = 4;
        constexpr int kPerThread = 25;  // captured explicitly (MSVC C3493-safe)

        std::atomic<int> ok_registrations{0};
        std::atomic<int> failed_reservations{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&registry, &ok_registrations, &failed_reservations, t]() {
                const int per_thread = kPerThread;  // local copy captured by value
                for (int i = 0; i < per_thread; ++i) {
                    const std::string id = "dev-" + std::to_string(t) + "-" + std::to_string(i);
                    DeviceDescriptor out;
                    bool created = false;
                    if (registry.register_device(id, "user", caps(1, 1), out, created) ==
                        vortyx::platform::Status::Ok) {
                        ++ok_registrations;
                    }
                    // Concurrent lookups and reservations against the same
                    // growing registry.
                    DeviceDescriptor lookup;
                    registry.device("user", id, lookup);
                    std::string error;
                    DeviceLease lease;
                    ResourceVector request;
                    request.memory_bytes = 4 * 1024 * 1024;
                    request.concurrent_jobs = 1;
                    if (registry.reserve("user", id, "job", id + "-s0", request, 60000, lease,
                                         error) == vortyx::platform::Status::Ok) {
                        registry.release_lease(lease);
                    } else {
                        ++failed_reservations;
                    }
                }
            });
        }
        for (std::thread& thread : threads) thread.join();

        check(ok_registrations.load() == kThreads * kPerThread,
              "all concurrent registrations succeeded exactly once");
        std::vector<DeviceDescriptor> listing;
        registry.devices("user", listing);
        check(listing.size() == static_cast<std::size_t>(kThreads) * kPerThread,
              "the registry holds every device exactly once (no duplicates)");

        // Capacity invariants held across the whole run: every 1MB/1-job
        // device ended with zero allocation (each lease was released).
        bool all_clean = true;
        for (const DeviceDescriptor& device : listing) {
            if (device.allocated.memory_bytes != 0 || device.allocated.concurrent_jobs != 0) {
                all_clean = false;
            }
        }
        check(all_clean, "no reservation leaked under concurrency");
    }

    if (failures == 0) {
        std::cout << "Distributed foundation tests passed.\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
