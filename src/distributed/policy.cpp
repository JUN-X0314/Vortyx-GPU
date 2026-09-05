// Scheduling policies implementation (Phase 12).
//
// All three policies share one placement skeleton (plan_with_picker):
// request validation -> ownership/capability-filtered candidates -> shard
// sizing (the fallback policy) -> deterministic ranges -> per-shard device
// choice. A policy's entire freedom is the RANKING of the capacity-fitting
// candidates for each shard; the shared chooser prefers an untaken device
// and falls back to reuse only when every candidate is already taken (more
// shards than devices) and capacity allows.

#include "distributed/policy.hpp"

#include <algorithm>

namespace vortyx::distributed {

const char* to_string(PlacementRejection code) {
    switch (code) {
        case PlacementRejection::None: return "none";
        case PlacementRejection::InvalidRequest: return "invalid_request";
        case PlacementRejection::ClusterEmpty: return "cluster_empty";
        case PlacementRejection::UnsupportedCapability: return "unsupported_capability";
        case PlacementRejection::NoDeviceAvailable: return "no_device_available";
        case PlacementRejection::InsufficientResource: return "insufficient_resource";
        case PlacementRejection::DeviceUnhealthy: return "device_unhealthy";
        case PlacementRejection::StalePlan: return "stale_plan";
    }
    return "unknown";
}

namespace {

// The shared chooser: 'ranked' lists candidate indices in the policy's
// preference order. Returns the first UNTAKEN ranked index, or — when every
// ranked device is already taken by this plan — the first ranked index
// (reuse; the skeleton has already capacity-checked every candidate).
// Returns fits.size() only when fits is empty.
std::size_t choose_candidate(const std::vector<std::size_t>& ranked,
                             const std::vector<DeviceSnapshot>& fits,
                             const std::vector<DeviceId>& taken) {
    std::size_t reuse_fallback = fits.size();
    for (std::size_t idx : ranked) {
        bool already_taken = false;
        for (const DeviceId& id : taken) {
            if (id == fits[idx].device_id) {
                already_taken = true;
                break;
            }
        }
        if (!already_taken) return idx;
        if (reuse_fallback == fits.size()) reuse_fallback = idx;
    }
    return reuse_fallback;
}

}  // namespace

// ---------------------------------------------------------------------------
// The shared placement skeleton
// ---------------------------------------------------------------------------

PlacementPlan plan_with_picker(
    const PlacementRequest& request, const ClusterSnapshot& snapshot,
    const std::function<std::size_t(const std::vector<DeviceSnapshot>& fits,
                                    const ResourceVector& needed,
                                    const std::vector<DeviceId>& taken)>& pick) {
    PlacementPlan plan;
    plan.cluster_revision = snapshot.revision;

    // --- request validation ---------------------------------------------------
    if (request.element_count == 0 || request.requested_shard_count == 0) {
        plan.rejection = PlacementRejection::InvalidRequest;
        plan.message = "element_count and requested_shard_count must be positive";
        return plan;
    }

    // --- candidate filter: ownership (in the snapshot), state, health,
    //     capability (claims only — unknown capability never matches),
    //     then the request's explicit exclusions -----------------------------
    std::vector<DeviceSnapshot> capable;
    for (const DeviceSnapshot& device : snapshot.candidates_for(request.owner_user_id)) {
        if (device_supports(device.capabilities, request.operation, request.requested_backend)) {
            capable.push_back(device);
        }
    }

    if (capable.empty()) {
        if (snapshot.visible_for(request.owner_user_id).empty()) {
            plan.rejection = PlacementRejection::ClusterEmpty;
            plan.message = "no device is registered for this owner";
            return plan;
        }
        // The owner HAS devices but no candidate: distinguish honestly.
        bool any_schedulable = false;
        for (const DeviceSnapshot& device : snapshot.visible_for(request.owner_user_id)) {
            if (device_state_schedulable(device.state)) any_schedulable = true;
        }
        if (!any_schedulable) {
            plan.rejection = PlacementRejection::DeviceUnhealthy;
            plan.message = "the owner's devices are offline, draining or failed";
            return plan;
        }
        plan.rejection = PlacementRejection::UnsupportedCapability;
        plan.message = "no schedulable device claims operation '" +
                       std::string(vortyx::compute::workload_label(request.operation)) + "'" +
                       (request.requested_backend.empty()
                            ? std::string()
                            : " on backend '" + request.requested_backend + "'");
        return plan;
    }

    // Exclusions (applied AFTER the capability filter so a wrong exclusion
    // list can never be misreported as a capability problem).
    std::vector<DeviceSnapshot> candidates;
    for (const DeviceSnapshot& device : capable) {
        bool excluded = false;
        for (const DeviceId& banned : request.excluded_devices) {
            if (banned == device.device_id) {
                excluded = true;
                break;
            }
        }
        if (!excluded) candidates.push_back(device);
    }
    if (candidates.empty()) {
        // Every capable device is excluded (e.g. it just failed this shard).
        plan.rejection = PlacementRejection::NoDeviceAvailable;
        plan.message = "all " + std::to_string(capable.size()) +
                       " capable devices are excluded for this placement";
        return plan;
    }

    // --- shard sizing (the fallback policy) ------------------------------------
    std::uint64_t effective = request.requested_shard_count;
    if (request.allow_fallback) {
        if (effective > candidates.size()) effective = candidates.size();
    } else if (effective > candidates.size()) {
        plan.rejection = PlacementRejection::NoDeviceAvailable;
        plan.message = "requested " + std::to_string(effective) + " shards but only " +
                       std::to_string(candidates.size()) +
                       " capable devices exist (fallback disabled)";
        return plan;
    }
    if (effective > request.element_count) effective = request.element_count;  // no empty shards

    // --- deterministic ranges ---------------------------------------------------
    std::vector<ElementRange> ranges;
    std::string error;
    if (vortyx::platform::Status status =
            partition_element_count(request.element_count,
                                    static_cast<std::uint32_t>(effective), ranges, error);
        status != vortyx::platform::Status::Ok) {
        plan.rejection = PlacementRejection::InvalidRequest;
        plan.message = "sharding failed: " + error;
        return plan;
    }

    // --- per-shard device choice -------------------------------------------------
    std::vector<DeviceId> taken;  // devices already chosen in THIS plan
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const ElementRange& range = ranges[i];

        std::int64_t memory_needed = 0;
        if (!shard_memory_bytes(range.size(), request.operation, memory_needed, error)) {
            plan.rejection = PlacementRejection::InvalidRequest;
            plan.message = "shard memory accounting failed: " + error;
            return plan;
        }
        ResourceVector needed;
        // Compute-unit cost is NOT fabricated: nothing in Phase 12 maps an
        // operation to compute units, so placement gates on memory +
        // concurrency (the honest subset) only.
        needed.memory_bytes = memory_needed;
        needed.concurrent_jobs = 1;

        // Capacity-filtered candidate view for THIS shard.
        std::vector<DeviceSnapshot> fits;
        for (const DeviceSnapshot& device : candidates) {
            if (resource_vector_fits(device.capabilities.capacity, device.allocated, needed)) {
                fits.push_back(device);
            }
        }

        const std::size_t chosen = pick(fits, needed, taken);
        if (chosen >= fits.size()) {
            plan.rejection = PlacementRejection::InsufficientResource;
            plan.message = "shard " + std::to_string(i) + " (" +
                           std::to_string(range.size()) +
                           " elements) does not fit any capable device's remaining capacity";
            return plan;
        }

        ShardPlan entry;
        entry.shard_index = static_cast<std::uint32_t>(i);
        entry.range = range;
        entry.device_id = fits[chosen].device_id;
        entry.resources = needed;
        plan.shards.push_back(entry);
        taken.push_back(entry.device_id);
    }

    plan.accepted = true;
    return plan;
}

// ---------------------------------------------------------------------------
// RoundRobin
// ---------------------------------------------------------------------------

PlacementPlan RoundRobinPolicy::plan(const PlacementRequest& request,
                                     const ClusterSnapshot& snapshot) {
    // Deterministic rotation: candidates are ranked starting at the cursor
    // position and wrapping. The cursor advances by one per PLACEMENT CALL
    // (not per shard) — the same request sequence over the same cluster
    // shape always rotates identically.
    return plan_with_picker(
        request, snapshot,
        [this](const std::vector<DeviceSnapshot>& fits, const ResourceVector&,
               const std::vector<DeviceId>& taken) -> std::size_t {
            if (fits.empty()) return 1;
            std::vector<std::size_t> ranked;
            ranked.reserve(fits.size());
            for (std::size_t scan = 0; scan < fits.size(); ++scan) {
                ranked.push_back((cursor_ + scan) % fits.size());
            }
            const std::size_t chosen = choose_candidate(ranked, fits, taken);
            cursor_ = (chosen + 1) % fits.size();
            return chosen;
        });
}

// ---------------------------------------------------------------------------
// LeastLoaded
// ---------------------------------------------------------------------------

PlacementPlan LeastLoadedPolicy::plan(const PlacementRequest& request,
                                      const ClusterSnapshot& snapshot) {
    return plan_with_picker(
        request, snapshot,
        [](const std::vector<DeviceSnapshot>& fits, const ResourceVector&,
           const std::vector<DeviceId>& taken) -> std::size_t {
            if (fits.empty()) return 1;
            // Rank: fewest allocated concurrent jobs, then fewest allocated
            // memory, then registration order (stable sort keeps the input
            // order for ties — deterministic).
            std::vector<std::size_t> ranked;
            ranked.reserve(fits.size());
            for (std::size_t i = 0; i < fits.size(); ++i) ranked.push_back(i);
            std::stable_sort(ranked.begin(), ranked.end(),
                             [&fits](std::size_t a, std::size_t b) {
                                 if (fits[a].allocated.concurrent_jobs !=
                                     fits[b].allocated.concurrent_jobs) {
                                     return fits[a].allocated.concurrent_jobs <
                                            fits[b].allocated.concurrent_jobs;
                                 }
                                 return fits[a].allocated.memory_bytes <
                                        fits[b].allocated.memory_bytes;
                             });
            return choose_candidate(ranked, fits, taken);
        });
}

// ---------------------------------------------------------------------------
// CapabilityFit (best fit)
// ---------------------------------------------------------------------------

PlacementPlan CapabilityFitPolicy::plan(const PlacementRequest& request,
                                        const ClusterSnapshot& snapshot) {
    return plan_with_picker(
        request, snapshot,
        [](const std::vector<DeviceSnapshot>& fits, const ResourceVector& needed,
           const std::vector<DeviceId>& taken) -> std::size_t {
            if (fits.empty()) return 1;
            // Rank: smallest remaining-memory slack over the shard's need
            // (best fit), then registration order.
            std::vector<std::size_t> ranked;
            ranked.reserve(fits.size());
            for (std::size_t i = 0; i < fits.size(); ++i) ranked.push_back(i);
            std::stable_sort(ranked.begin(), ranked.end(),
                             [&fits, &needed](std::size_t a, std::size_t b) {
                                 const std::int64_t slack_a =
                                     fits[a].available().memory_bytes - needed.memory_bytes;
                                 const std::int64_t slack_b =
                                     fits[b].available().memory_bytes - needed.memory_bytes;
                                 return slack_a < slack_b;
                             });
            return choose_candidate(ranked, fits, taken);
        });
}

// ---------------------------------------------------------------------------
// Policy registry
// ---------------------------------------------------------------------------

std::unique_ptr<ISchedulingPolicy> make_scheduling_policy(const std::string& name) {
    if (name == "round_robin") return std::make_unique<RoundRobinPolicy>();
    if (name == "least_loaded") return std::make_unique<LeastLoadedPolicy>();
    if (name == "capability_fit") return std::make_unique<CapabilityFitPolicy>();
    return nullptr;  // unknown configuration is refused, never defaulted
}

const std::vector<std::string>& known_scheduling_policies() {
    static const std::vector<std::string> kPolicies = {"round_robin", "least_loaded",
                                                       "capability_fit"};
    return kPolicies;
}

}  // namespace vortyx::distributed
