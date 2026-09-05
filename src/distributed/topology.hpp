#pragma once

// Topology abstraction (Phase 12) — device-to-device relationships,
// provider-neutral and honest about what it does not know.
//
// Phase 12 does NOT implement hardware topology discovery (no PCIe scan,
// no NVLink probe). What exists here is the SEAM: a topology provider can
// describe links between devices, schedulers can later use it, and until a
// real provider is plugged in the default view is EMPTY — the absence of
// topology information is expressed as "no links", never as fabricated
// bandwidth/latency numbers, and no code path treats "no data" as "optimal
// interconnect".
//
// Link kinds are the vocabulary a future discovery provider would report:
//   local (same process/host), shared_memory, pcie, network, unknown.
// Bandwidth/latency stay optional metadata that only a real provider fills;
// the defaults are "unknown" (0/false), documented as unknown, not guessed.

#include <string>
#include <vector>

#include "platform/identity.hpp"  // DeviceId

namespace vortyx::distributed {

using vortyx::platform::DeviceId;  // reused platform identity (see device.hpp)

enum class LinkType { Unknown, Local, SharedMemory, Pcie, Network };

const char* to_string(LinkType type);

struct TopologyLink {
    DeviceId device_a;   // undirected pair (a, b) with a < b by convention
    DeviceId device_b;
    LinkType type = LinkType::Unknown;

    // Optional provider-reported metadata. 0 / false = NOT REPORTED —
    // consumers must not interpret these as measurements.
    std::int64_t bandwidth_bytes_per_second = 0;
    std::int64_t latency_microseconds = 0;
};

// A static topology provider's answer: the link list it was configured
// with. The interface exists so a future discovery provider can answer the
// same question.
struct TopologyView {
    std::vector<TopologyLink> links;

    // The link between two devices, or nullptr when none is known. Finds
    // the link regardless of the (a, b) orientation. Pointer into this
    // view's links vector — valid while the view lives.
    const TopologyLink* link_between(const DeviceId& a, const DeviceId& b) const;
};

// Configures a fixed link set (the mock/static provider for tests and the
// local simulator). Pure value type; no discovery happens here.
TopologyView make_static_topology(const std::vector<TopologyLink>& links);

}  // namespace vortyx::distributed
