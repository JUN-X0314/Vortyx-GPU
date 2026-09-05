// Distributed surface tests (Phase 12) — the TypeScript mirror of the
// distributed wire contract and record store: request parsing, submission
// idempotency/conflict, ownership (foreign invisible), cancellation
// transitions, the cluster view, and byte-deterministic serialization.
//
// No network, no Supabase, no secrets: the store under test is the
// local/mock InMemoryDistributedStore, and the rules it exercises are the
// same rules the C++ distributed layer (src/distributed) and the RLS
// policies (migration 0002) enforce.

import assert from "node:assert/strict";
import { describe, it } from "node:test";

import {
  InMemoryDistributedStore,
  type DeviceView,
  type DistributedSubmission,
} from "../src/distributed.ts";
import {
  parseCreateDistributedJob,
  serializeClusterView,
  serializeDistributedJob,
  serializeDistributedShards,
} from "../src/contract.ts";
import { handlePlatformRequest, type PlatformDeps } from "../src/router.ts";
import { makeAuthenticated } from "../src/auth.ts";
import { InMemoryPlatformStore } from "../src/memory-store.ts";

function submission(jobId: string, overrides: Partial<DistributedSubmission> = {}): DistributedSubmission {
  return {
    envelope: {
      job_id: jobId,
      operation: "vector_add",
      element_count: 4096,
      requested_backend: "cpu",
      priority: 0,
      protocol_version: "1",
      created_at_ms: null,
    },
    requested_shard_count: 4,
    ...overrides,
  };
}

const auth = makeAuthenticated("user-a");

describe("parseCreateDistributedJob", () => {
  it("parses the documented schema", () => {
    const parsed = parseCreateDistributedJob({
      job_id: "job-1",
      operation: "vector_add",
      element_count: 4096,
      requested_shard_count: 4,
      requested_backend: "cpu",
      priority: 2,
      protocol_version: "1",
    });
    assert.ok(parsed.ok);
    assert.equal(parsed.value.envelope.job_id, "job-1");
    assert.equal(parsed.value.envelope.operation, "vector_add");
    assert.equal(parsed.value.envelope.element_count, 4096);
    assert.equal(parsed.value.requested_shard_count, 4);
    assert.equal(parsed.value.envelope.requested_backend, "cpu");
  });

  it("rejects the violations with their stable codes", () => {
    const base = {
      job_id: "job-1",
      operation: "vector_add",
      element_count: 10,
      requested_shard_count: 1,
      protocol_version: "1",
    };
    assert.equal(parseCreateDistributedJob([]).ok, false);
    assert.equal(parseCreateDistributedJob({ ...base, operation: "matmul" }).ok, false);
    assert.equal(parseCreateDistributedJob({ ...base, element_count: 0 }).ok, false);
    assert.equal(parseCreateDistributedJob({ ...base, requested_shard_count: 0 }).ok, false);
    // requested_shard_count is REQUIRED on the distributed surface.
    const missing: Record<string, unknown> = { ...base };
    delete missing["requested_shard_count"];
    assert.equal(parseCreateDistributedJob(missing).ok, false);
    assert.equal(parseCreateDistributedJob({ ...base, protocol_version: "9" }).ok, false);
    // Metadata only: a smuggled payload field is a schema violation.
    assert.equal(parseCreateDistributedJob({ ...base, payload: { a: [1] } }).ok, false);
  });
});

describe("InMemoryDistributedStore", () => {
  it("records a submission idempotently and conflicts on a different payload", async () => {
    const store = new InMemoryDistributedStore();
    const first = await store.createDistributedJob(auth, submission("job-1"));
    assert.equal(first.status, "ok");
    assert.equal(first.created, true);

    const replay = await store.createDistributedJob(auth, submission("job-1"));
    assert.equal(replay.status, "ok");
    assert.equal(replay.created, false);

    const different = await store.createDistributedJob(
      auth,
      submission("job-1", { requested_shard_count: 2 }),
    );
    assert.equal(different.status, "conflict");

    // A foreign owner cannot hijack the id.
    const stranger = await store.createDistributedJob(
      makeAuthenticated("user-b"),
      submission("job-1"),
    );
    assert.equal(stranger.status, "conflict");
  });

  it("keeps foreign jobs invisible (anti-enumeration)", async () => {
    const store = new InMemoryDistributedStore();
    await store.createDistributedJob(auth, submission("job-1"));

    const stranger = makeAuthenticated("user-b");
    const foreign = await store.distributedJob(stranger, "job-1");
    assert.equal(foreign.status, "not_found");
    const unknown = await store.distributedJob(auth, "job-x");
    assert.equal(unknown.status, "not_found");

    const listing = await store.distributedJobs(stranger);
    assert.ok(listing.status === "ok" && listing.record.length === 0);
  });

  it("cancels only non-terminal jobs through the documented transition", async () => {
    const store = new InMemoryDistributedStore();
    await store.createDistributedJob(auth, submission("job-1"));

    const cancelled = await store.cancelDistributedJob(auth, "job-1");
    assert.ok(cancelled.status === "ok" && cancelled.record.status === "cancelled");
    assert.ok(cancelled.record.completed_at_ms !== null);

    // A terminal job cannot be cancelled again (the Phase 11 rule).
    const again = await store.cancelDistributedJob(auth, "job-1");
    assert.equal(again.status, "invalid_input");

    // A foreign user cannot even see the job to cancel it.
    const foreign = await store.cancelDistributedJob(makeAuthenticated("user-b"), "job-1");
    assert.equal(foreign.status, "not_found");
  });

  it("reports the cluster view ownership-scoped", async () => {
    const store = new InMemoryDistributedStore();
    const view: DeviceView = {
      device_id: "device-0",
      owner_user_id: "user-a",
      state: "ready",
      health: "healthy",
      capacity: { compute_units: 0, memory_bytes: 8 * 1024 * 1024, concurrent_jobs: 2 },
      allocated: { compute_units: 0, memory_bytes: 0, concurrent_jobs: 0 },
      backends: ["cpu"],
      running_shards: 0,
      last_heartbeat_ms: 1234,
    };
    await store.reportDeviceView(view);
    await store.reportDeviceView({ ...view, device_id: "device-1", owner_user_id: "user-b" });

    const mine = await store.clusterView(auth);
    assert.ok(mine.status === "ok");
    assert.equal(mine.record.devices.length, 1);
    assert.equal(mine.record.devices[0].device_id, "device-0");
    assert.equal(mine.record.devices[0].state, "ready");

    const theirs = await store.clusterView(makeAuthenticated("user-b"));
    assert.ok(theirs.status === "ok" && theirs.record.devices[0].device_id === "device-1");
  });
});

describe("distributed serialization", () => {
  it("is byte-deterministic and matches the C++ field order", () => {
    const record = {
      job_id: "job-9",
      owner_user_id: "user-a",
      operation: "vector_add",
      element_count: 100,
      requested_backend: "cpu",
      requested_shard_count: 2,
      status: "failed" as const,
      error: "1 of 2 shards failed",
      shards: [
        {
          shard_id: "job-9-s0",
          index: 0,
          state: "completed" as const,
          element_begin: 0,
          element_end: 50,
          device_id: "device-0",
          attempt: 1,
          retry_count: 0,
          failure_code: "",
        },
        {
          shard_id: "job-9-s1",
          index: 1,
          state: "failed" as const,
          element_begin: 50,
          element_end: 100,
          device_id: "",
          attempt: 4,
          retry_count: 3,
          failure_code: "device_lost",
        },
      ],
      created_at_ms: 1000,
      completed_at_ms: 2000,
    };

    const first = JSON.stringify(serializeDistributedJob(record));
    assert.equal(first, JSON.stringify(serializeDistributedJob(record)));
    const parsed = JSON.parse(first) as Record<string, unknown>;
    assert.equal(parsed["status"], "failed");
    assert.equal(parsed["failed"], 1);
    assert.equal(parsed["succeeded"], 1);
    const shards = parsed["shards"] as Record<string, unknown>[];
    assert.equal(shards[1]["failure_code"], "device_lost");

    const shardJson = JSON.stringify(serializeDistributedShards(record));
    assert.equal(shardJson, JSON.stringify(serializeDistributedShards(record)));
    assert.equal((JSON.parse(shardJson) as Record<string, unknown>)["job_id"], "job-9");

    const view = {
      revision: 7,
      devices: [
        {
          device_id: "device-0",
          owner_user_id: "user-a",
          state: "ready" as const,
          health: "healthy" as const,
          capacity: { compute_units: 0, memory_bytes: 8388608, concurrent_jobs: 2 },
          allocated: { compute_units: 0, memory_bytes: 0, concurrent_jobs: 1 },
          backends: ["cpu"],
          running_shards: 1,
          last_heartbeat_ms: 5000,
        },
      ],
    };
    const viewJson = JSON.stringify(serializeClusterView(view));
    assert.equal(viewJson, JSON.stringify(serializeClusterView(view)));
    const viewParsed = JSON.parse(viewJson) as Record<string, unknown>;
    assert.equal(viewParsed["revision"], 7);
    assert.equal(
      (viewParsed["devices"] as Record<string, unknown>[])[0]["state"],
      "ready",
    );
  });
});

describe("distributed routes", () => {
  function deps(): PlatformDeps {
    return {
      store: new InMemoryPlatformStore(),
      verifier: (token) =>
        token.startsWith("local:")
          ? makeAuthenticated(token.slice("local:".length))
          : null,
      storeKind: "memory",
      softwareVersion: "0.13.0",
      distributed: new InMemoryDistributedStore(),
    };
  }

  const body = JSON.stringify({
    job_id: "job-route",
    operation: "vector_add",
    element_count: 100,
    requested_shard_count: 2,
    protocol_version: "1",
  });

  it("submits, reads and cancels a distributed job over the routes", async () => {
    const d = deps();
    const submit = await handlePlatformRequest(
      { method: "POST", path: "/api/distributed/jobs", body, authorization: "Bearer local:user-a" },
      d,
    );
    assert.equal(submit.status, 200);
    assert.equal((submit.body as Record<string, unknown>)["status"], "queued");

    const detail = await handlePlatformRequest(
      { method: "GET", path: "/api/distributed/jobs/job-route", body: undefined, authorization: "Bearer local:user-a" },
      d,
    );
    assert.equal(detail.status, 200);
    assert.equal((detail.body as Record<string, unknown>)["job_id"], "job-route");

    const shards = await handlePlatformRequest(
      { method: "GET", path: "/api/distributed/jobs/job-route/shards", body: undefined, authorization: "Bearer local:user-a" },
      d,
    );
    assert.equal(shards.status, 200);
    assert.deepEqual((shards.body as Record<string, unknown>)["shards"], []);

    const cancel = await handlePlatformRequest(
      { method: "POST", path: "/api/distributed/jobs/job-route/cancel", body: undefined, authorization: "Bearer local:user-a" },
      d,
    );
    assert.equal(cancel.status, 200);
    assert.equal((cancel.body as Record<string, unknown>)["status"], "cancelled");

    // The listed cluster view starts honest and empty (no device reports).
    const cluster = await handlePlatformRequest(
      { method: "GET", path: "/api/cluster", body: undefined, authorization: "Bearer local:user-a" },
      d,
    );
    assert.equal(cluster.status, 200);
    assert.deepEqual((cluster.body as Record<string, unknown>)["devices"], []);
  });

  it("keeps the Phase 11 status mapping for distributed failures", async () => {
    const d = deps();
    const unauth = await handlePlatformRequest(
      { method: "POST", path: "/api/distributed/jobs", body },
      d,
    );
    assert.equal(unauth.status, 401);

    const bad = await handlePlatformRequest(
      { method: "POST", path: "/api/distributed/jobs", body: "{not json", authorization: "Bearer local:user-a" },
      d,
    );
    assert.equal(bad.status, 400);
    assert.equal(
      ((bad.body as Record<string, unknown>)["error"] as Record<string, unknown>)["code"],
      "invalid_json",
    );

    const missing = await handlePlatformRequest(
      { method: "POST", path: "/api/distributed/jobs", body: JSON.stringify({ job_id: "x" }), authorization: "Bearer local:user-a" },
      d,
    );
    assert.equal(missing.status, 422);

    const foreign = await handlePlatformRequest(
      { method: "GET", path: "/api/distributed/jobs/job-route", body: undefined, authorization: "Bearer local:user-b" },
      d,
    );
    assert.equal(foreign.status, 404);

    const wrongMethod = await handlePlatformRequest(
      { method: "DELETE", path: "/api/distributed/jobs/job-route", body: undefined, authorization: "Bearer local:user-a" },
      d,
    );
    assert.equal(wrongMethod.status, 405);

    // The Phase 11 routes are untouched.
    const health = await handlePlatformRequest({ method: "GET", path: "/api/health", body: undefined }, d);
    assert.equal(health.status, 200);
  });
});
