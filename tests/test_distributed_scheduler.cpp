// Distributed scheduling tests (Phase 12) — deterministic partitioning
// (property-style invariants), the three scheduling policies, placement
// rejections, fallback semantics, topology and cluster snapshots.
//
// The policies are pure functions of (request, snapshot): every expectation
// below is deterministic and needs no hardware, no clock and no threads.

#include <iostream>
#include <set>
#include <string>
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

DeviceSnapshot make_device(const DeviceId& id, const UserId& owner, std::int64_t memory_mb,
                           std::int64_t jobs, DeviceState state = DeviceState::Ready,
                           DeviceHealth health = DeviceHealth::Healthy) {
    DeviceSnapshot device;
    device.device_id = id;
    device.owner_user_id = owner;
    device.capabilities.metadata.protocol_version = vortyx::platform::kProtocolVersion;
    device.capabilities.metadata.software_version = "0.12.0";
    device.capabilities.metadata.backends = {"cpu"};
    device.capabilities.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
    device.capabilities.capacity.memory_bytes = memory_mb * 1024 * 1024;
    device.capabilities.capacity.concurrent_jobs = jobs;
    device.capabilities.max_concurrent_shards = jobs;
    device.state = state;
    device.health = health;
    return device;
}

ClusterSnapshot cluster_of(const std::vector<DeviceSnapshot>& devices, std::uint64_t revision) {
    ClusterSnapshot snapshot;
    snapshot.revision = revision;
    snapshot.devices = devices;
    return snapshot;
}

PlacementRequest request_for(const UserId& owner, std::uint64_t elements,
                             std::uint32_t shards, bool fallback = true) {
    PlacementRequest request;
    request.job_id = "job";
    request.owner_user_id = owner;
    request.operation = vortyx::compute::ComputeOp::VectorAdd;
    request.requested_backend = "cpu";
    request.element_count = elements;
    request.requested_shard_count = shards;
    request.allow_fallback = fallback;
    return request;
}

// The property verifier: coverage exact, no overlap, no empty shard.
bool ranges_are_a_partition(const std::vector<ElementRange>& ranges, std::uint64_t elements) {
    std::uint64_t covered = 0;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const ElementRange& range = ranges[i];
        if (range.begin >= range.end) return false;                       // empty
        if (range.end > elements) return false;                           // out of domain
        if (i > 0 && range.begin != ranges[i - 1].end) return false;      // gap or overlap
        covered += range.end - range.begin;
    }
    return covered == elements;
}

}  // namespace

int main() {
    // =====================================================================
    // 1. partition_element_count: the partition property (property-style)
    // =====================================================================
    {
        std::string error;
        for (std::uint64_t elements : {1ULL, 2ULL, 3ULL, 7ULL, 8ULL, 100ULL, 1001ULL, 65537ULL}) {
            for (std::uint32_t shards : {1u, 2u, 3u, 4u, 5u, 8u, 16u, 100u}) {
                std::vector<ElementRange> ranges;
                if (partition_element_count(elements, shards, ranges, error) != Status::Ok) {
                    check(false, "partition failed for " + std::to_string(elements) + "/" +
                                     std::to_string(shards));
                    continue;
                }
                // K > N: no empty shards, exactly N one-element shards.
                const std::uint64_t expected_count =
                    shards < elements ? static_cast<std::uint64_t>(shards) : elements;
                check(ranges.size() == expected_count,
                      "shard count capped (no empty shards) for " + std::to_string(elements) +
                          "/" + std::to_string(shards));
                check(ranges_are_a_partition(ranges, elements),
                      "ranges are an exact partition for " + std::to_string(elements) + "/" +
                          std::to_string(shards));
                // Balance: sizes differ by at most 1.
                if (!ranges.empty()) {
                    const std::uint64_t smallest = ranges.front().size();
                    const std::uint64_t largest = ranges.back().size();
                    check(largest - smallest <= 1, "sizes balanced within 1");
                }
            }
        }

        // Determinism: the same inputs always produce the same plan.
        std::vector<ElementRange> a, b;
        partition_element_count(1000, 7, a, error);
        partition_element_count(1000, 7, b, error);
        check(a == b, "partitioning is deterministic");

        // Refusals.
        std::vector<ElementRange> ranges;
        check(partition_element_count(0, 4, ranges, error) == Status::InvalidInput,
              "zero elements refused");
        check(partition_element_count(100, 0, ranges, error) == Status::InvalidInput,
              "zero shards refused");
    }

    // =====================================================================
    // 2. make_shard_id: deterministic derivation, cap enforcement
    // =====================================================================
    {
        std::string id, error;
        check(make_shard_id("job-1", 0, id, error) == Status::Ok && id == "job-1-s0",
              "shard id derivation is <job>-s<index>");
        make_shard_id("job-1", 42, id, error);
        check(id == "job-1-s42", "decimal index");
        check(is_derived_shard_id("job-1", "job-1-s42"), "shape check accepts real ids");
        check(!is_derived_shard_id("job-1", "job-1-sX"), "non-numeric suffix refused");
        check(!is_derived_shard_id("job-1", "other-s0"), "foreign prefix refused");
        check(!is_derived_shard_id("job-1", "job-1-s"), "empty suffix refused");

        const std::string fits_job(vortyx::platform::kMaxIdLength - 3, 'a');
        check(make_shard_id(fits_job, 0, id, error) == Status::Ok,
              "a job id whose shard ids just fit is accepted");
        const std::string too_long_job(vortyx::platform::kMaxIdLength - 2, 'a');
        check(make_shard_id(too_long_job, 0, id, error) == Status::InvalidInput,
              "a job id whose shard ids would overflow the cap is REFUSED (never truncated)");
    }

    // =====================================================================
    // 3. RoundRobin: deterministic rotation, prefers untaken devices
    // =====================================================================
    {
        ClusterSnapshot snapshot = cluster_of(
            {make_device("d0", "user", 64, 4), make_device("d1", "user", 64, 4),
             make_device("d2", "user", 64, 4), make_device("d3", "user", 64, 4)},
            7);

        RoundRobinPolicy policy;
        PlacementPlan plan = policy.plan(request_for("user", 40000, 4), snapshot);
        check(plan.accepted, "round-robin accepts a 4-shard plan on 4 devices");
        check(plan.cluster_revision == 7, "the plan records the snapshot revision");
        check(plan.shards.size() == 4, "four shard entries");
        std::set<DeviceId> devices;
        for (const ShardPlan& entry : plan.shards) devices.insert(entry.device_id);
        check(devices.size() == 4, "round-robin spreads one shard per device");
        check(ranges_are_a_partition({plan.shards[0].range, plan.shards[1].range,
                                      plan.shards[2].range, plan.shards[3].range},
                                     40000),
              "the plan's ranges partition the domain");

        // Determinism: identical request + snapshot -> identical plan.
        RoundRobinPolicy again;
        PlacementPlan plan2 = again.plan(request_for("user", 40000, 4), snapshot);
        check(plan2.shards.size() == plan.shards.size(), "same shape");
        bool same = true;
        for (std::size_t i = 0; i < plan.shards.size(); ++i) {
            same = same && plan2.shards[i].device_id == plan.shards[i].device_id &&
                   plan2.shards[i].range.begin == plan.shards[i].range.begin;
        }
        check(same, "a fresh policy over the same state rotates identically");

        // The rotation ADVANCES between calls: two sequential single-shard
        // placements hit different devices (deterministic rotation).
        RoundRobinPolicy rotator;
        PlacementPlan first = rotator.plan(request_for("user", 40000, 1), snapshot);
        PlacementPlan second = rotator.plan(request_for("user", 40000, 1), snapshot);
        check(first.accepted && second.accepted, "both rotations place");
        check(first.shards[0].device_id != second.shards[0].device_id,
              "the cursor rotated between calls");
    }

    // =====================================================================
    // 4. LeastLoaded: capacity-aware pick (fewest allocated jobs first)
    // =====================================================================
    {
        DeviceSnapshot loaded = make_device("loaded", "user", 64, 4);
        loaded.allocated.concurrent_jobs = 3;  // busy device
        DeviceSnapshot idle = make_device("idle", "user", 64, 4);

        ClusterSnapshot snapshot = cluster_of({loaded, idle}, 3);
        LeastLoadedPolicy policy;
        PlacementPlan plan = policy.plan(request_for("user", 1000, 1), snapshot);
        check(plan.accepted && plan.shards[0].device_id == "idle",
              "least loaded picks the device with fewer allocated jobs");

        // Tie on jobs -> fewer allocated memory bytes wins.
        DeviceSnapshot memheavy = make_device("memheavy", "user", 64, 4);
        memheavy.allocated.memory_bytes = 10 * 1024 * 1024;
        DeviceSnapshot memlight = make_device("memlight", "user", 64, 4);
        memlight.allocated.memory_bytes = 1 * 1024 * 1024;
        ClusterSnapshot tie = cluster_of({memheavy, memlight}, 4);
        PlacementPlan tie_plan = policy.plan(request_for("user", 1000, 1), tie);
        check(tie_plan.shards[0].device_id == "memlight",
              "the tie breaks on allocated memory");
    }

    // =====================================================================
    // 5. CapabilityFit: best (tightest) fit on remaining memory
    // =====================================================================
    {
        DeviceSnapshot big = make_device("big", "user", 64, 4);      // 64MB free
        DeviceSnapshot snug = make_device("snug", "user", 12, 4);    // 12MB free
        ClusterSnapshot snapshot = cluster_of({big, snug}, 5);

        CapabilityFitPolicy policy;
        // A ~10MB shard fits both; the tightest fit is 'snug'.
        PlacementPlan plan = policy.plan(request_for("user", 800000, 1), snapshot);
        check(plan.accepted && plan.shards[0].device_id == "snug",
              "capability fit chooses the smallest sufficient slack");
    }

    // =====================================================================
    // 6. Rejections: every stable code is reachable and honest
    // =====================================================================
    {
        ClusterSnapshot snapshot = cluster_of(
            {make_device("d0", "user", 64, 4), make_device("d1", "user", 64, 4)}, 9);

        RoundRobinPolicy policy;

        // Invalid request.
        PlacementPlan invalid = policy.plan(request_for("user", 0, 2), snapshot);
        check(!invalid.accepted && invalid.rejection == PlacementRejection::InvalidRequest,
              "zero elements -> invalid_request");

        // Cluster empty (no devices at all).
        ClusterSnapshot empty = cluster_of({}, 1);
        PlacementPlan nothing = policy.plan(request_for("user", 100, 2), empty);
        check(!nothing.accepted && nothing.rejection == PlacementRejection::ClusterEmpty,
              "no devices -> cluster_empty");

        // Devices exist but all are offline/draining/failed.
        ClusterSnapshot dead = cluster_of(
            {make_device("d0", "user", 64, 4, DeviceState::Offline),
             make_device("d1", "user", 64, 4, DeviceState::Draining)},
            2);
        PlacementPlan unhealthy = policy.plan(request_for("user", 100, 1), dead);
        check(!unhealthy.accepted && unhealthy.rejection == PlacementRejection::DeviceUnhealthy,
              "no schedulable devices -> device_unhealthy");

        // Capability mismatch: devices healthy but the op is not claimed.
        ClusterSnapshot wrong_ops = cluster_of({make_device("d0", "user", 64, 4)}, 3);
        PlacementRequest mismatch = request_for("user", 100, 1);
        mismatch.operation = vortyx::compute::ComputeOp::VectorAdd;
        mismatch.requested_backend = "vulkan";  // the devices claim only cpu
        PlacementPlan rejected = policy.plan(mismatch, wrong_ops);
        check(!rejected.accepted &&
                  rejected.rejection == PlacementRejection::UnsupportedCapability,
              "unclaimed backend -> unsupported_capability (unknown is never guessed)");

        // Not enough devices with fallback DISABLED.
        PlacementPlan too_few = policy.plan(request_for("user", 100, 4, false), snapshot);
        check(!too_few.accepted && too_few.rejection == PlacementRejection::NoDeviceAvailable,
              "shards > devices without fallback -> no_device_available");

        // Insufficient memory: one device, a shard that cannot fit.
        ClusterSnapshot tiny = cluster_of({make_device("tiny", "user", 1, 4)}, 4);
        PlacementPlan does_not_fit = policy.plan(request_for("user", 1000000, 1), tiny);
        check(!does_not_fit.accepted &&
                  does_not_fit.rejection == PlacementRejection::InsufficientResource,
              "a shard that fits nowhere -> insufficient_resource");

        // Unknown policy name: refused, never defaulted.
        check(make_scheduling_policy("nope") == nullptr, "unknown policy name refused");
        check(known_scheduling_policies().size() == 3, "three policies are registered");
    }

    // =====================================================================
    // 7. Fallback: coalesce to the devices that exist (policy-driven)
    // =====================================================================
    {
        ClusterSnapshot snapshot = cluster_of(
            {make_device("d0", "user", 64, 4), make_device("d1", "user", 64, 4)}, 5);
        RoundRobinPolicy policy;

        PlacementPlan coalesced = policy.plan(request_for("user", 100, 4, true), snapshot);
        check(coalesced.accepted && coalesced.shards.size() == 2,
              "4 requested over 2 devices with fallback -> 2 shards");
        check(ranges_are_a_partition({coalesced.shards[0].range, coalesced.shards[1].range}, 100),
              "the coalesced plan still covers the domain exactly");

        // Single-device cluster, multi-device request, fallback on.
        ClusterSnapshot solo = cluster_of({make_device("d0", "user", 64, 4)}, 6);
        PlacementPlan one = policy.plan(request_for("user", 100, 4, true), solo);
        check(one.accepted && one.shards.size() == 1,
              "single device + fallback -> single-device execution");

        // More shards than elements: capped, no empty shards.
        PlacementPlan tiny_job = policy.plan(request_for("user", 2, 4, true), snapshot);
        check(tiny_job.accepted && tiny_job.shards.size() == 2,
              "2 elements over 2 devices -> 2 one-element shards");
        PlacementPlan tinier = policy.plan(request_for("user", 1, 4, true), solo);
        check(tinier.accepted && tinier.shards.size() == 1 && tinier.shards[0].range.size() == 1,
              "1 element over 4 devices (fallback) -> exactly 1 shard");
    }

    // =====================================================================
    // 8. Snapshot candidacy: ownership, state, health filters
    // =====================================================================
    {
        ClusterSnapshot snapshot = cluster_of(
            {make_device("mine-ready", "user", 8, 1), make_device("mine-off", "user", 8, 1,
                                                                  DeviceState::Offline),
             make_device("mine-sick", "user", 8, 1, DeviceState::Ready,
                         DeviceHealth::Unhealthy),
             make_device("foreign", "other", 8, 1)},
            8);

        const std::vector<DeviceSnapshot> candidates = snapshot.candidates_for("user");
        check(candidates.size() == 1 && candidates[0].device_id == "mine-ready",
              "only owned+ready+healthy devices are candidates");
        check(snapshot.visible_for("user").size() == 3,
              "the owner sees all their devices (any state)");
        check(snapshot.visible_for("other").size() == 1,
              "the other owner sees only their own device");
        check(snapshot.visible_for("other")[0].device_id == "foreign",
              "foreign devices are invisible in every view");
    }

    // =====================================================================
    // 9. Topology: the seam with honest unknowns
    // =====================================================================
    {
        TopologyView empty_view;
        check(empty_view.link_between("a", "b") == nullptr,
              "no topology data -> no links (never fabricated)");

        TopologyLink link;
        link.device_a = "a";
        link.device_b = "b";
        link.type = LinkType::Pcie;
        TopologyView view = make_static_topology({link});
        check(view.link_between("a", "b") != nullptr &&
                  view.link_between("a", "b")->type == LinkType::Pcie,
              "the static provider answers the configured link");
        check(view.link_between("b", "a") != nullptr,
              "the link is undirected (orientation-free lookup)");
        check(view.link_between("a", "c") == nullptr, "unknown pairs are nullptr");
        check(link.bandwidth_bytes_per_second == 0 && link.latency_microseconds == 0,
              "unreported bandwidth/latency stay unknown (0 = not reported)");
        check(std::string(to_string(LinkType::SharedMemory)) == "shared_memory",
              "link type labels are stable");
    }

    if (failures == 0) {
        std::cout << "Distributed scheduler tests passed.\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
