# Device Model (Phase 12)

## Identity — reused, never redefined

A device's identity is `vortyx::platform::DeviceId`; its ownership is
`vortyx::platform::UserId`; its self-description is
`vortyx::platform::DeviceMetadata` (protocol version, software version,
OS, architecture, backends, operations, display name). Phase 12 adds no
second identity system and collects no hardware fingerprint, MAC address
or serial number — the Phase 11 rules hold verbatim.

## Device state machine

`DeviceState` is the registry-level lifecycle of one logical device.
("Unknown" is the state of an id that is not in the registry — a lookup
miss, not a stored state.)

```
Registering → Ready | Failed | Offline
Ready       → Busy | Draining | Offline | Failed
Busy        → Ready | Draining | Offline | Failed
Draining    → Offline | Failed
Offline     → Registering | Ready        (re-registration / heartbeat proof)
Failed      → Registering | Offline      (remediation / retirement)
```

The table is the pure function `device_state_transition_valid`; the
registry enforces it on every update. There is **no silent revival**: an
offline device returns to Ready only through a heartbeat (proof of life)
or explicit re-registration, and a failed device never becomes Ready
without remediation.

Schedulability (`device_state_schedulable`): only `ready` and `busy`
devices are placement candidates. `busy` devices stay candidates while
their remaining capacity allows — that is the point of the resource model.

## Health — an explicit judgment

`DeviceHealth` is `healthy` / `unhealthy` / `unknown`. It is a
classification made from evidence (heartbeats, execution reports), never a
measurement and never guessed:

- `unknown` (fresh registration) is **not** treated as usable — unknown
  capability or health never matches a placement.
- The heartbeat monitor marks a device `unhealthy` (and `offline` when it
  was schedulable) when its last liveness evidence is older than the
  configured timeout. Recovery is a heartbeat.
- A `failed` device is left to its own remediation path.

## Capabilities — claims only

`DeviceCapabilities` bundles the Phase 11 self-description plus:

- `capacity`: a `ResourceVector` (compute units, memory bytes, concurrent
  jobs) — **self-reported configuration**. Nothing in Phase 12 measures
  hardware; a field that does not exist cannot lie.
- `max_concurrent_shards`: the device's own declaration of how many shard
  executions it accepts at once; the registry refuses reservations beyond
  it.
- `backends` / `operations`: validated against the canonical vocabularies
  (`is_known_backend`, the Phase 10 workload labels). Duplicates are
  refused. An empty list claims nothing — and a device claiming nothing is
  scheduled nowhere (`unknown capability is never guessed into support`).
- `preferred_backend()` is derived (the first claimed backend), not a
  second configuration field.

## Resource model

`ResourceVector { compute_units, memory_bytes, concurrent_jobs }` is the
extensible resource abstraction. Invariants (centralized in
`resource.hpp`, pinned by tests):

- no field is ever negative;
- `allocated ≤ capacity`, hence `available ≥ 0`;
- a release never drives accounting negative (`sub` clamps at 0, and the
  registry refuses releases that do not match an issued lease);
- shard memory is computed honestly per operation (`shard_memory_bytes`):
  two inputs + output for `vector_add`/`vector_multiply`, input + output
  for `vector_scale`, 4 bytes per int32 element — overflow-sized requests
  are refused, never wrapped.

## Registry semantics

`LocalDeviceRegistry` (the local/mock implementation of `IDeviceRegistry`):

- **Registration is idempotent**: the same id + owner + identical
  capability payload is a replay (`created == false`) that refreshes the
  liveness stamp and applies the documented recovery transitions. A
  different owner or payload is a `Conflict` that never reveals who owns
  the existing record (the Phase 11 anti-enumeration rule).
- **Visibility is ownership-scoped**: every method takes the requester;
  foreign devices are `NotFound` (invisible), mirroring RLS.
- **Reservation is atomic**: `reserve()` checks and records the lease
  under the registry lock — two concurrent schedulers can never both hold
  capacity that does not exist. Releases must match the issued record
  exactly (no double free, no foreign release).
- **Leases expire**: a lease past its TTL is reclaimed lazily (on the next
  reserve/snapshot/heartbeat-observation) and deterministically with the
  injected clock. Unregistering a device with active leases is refused —
  the leak rule is explicit.
- **Revision**: every mutation bumps a monotonically increasing cluster
  revision; snapshots carry it (see `scheduling.md` for the stale-plan
  use).

The registry is thread-safe (one mutex, the same shape as the Phase 11
in-memory store) and honest about scale: linear scans, deterministic
registration-order listings.
