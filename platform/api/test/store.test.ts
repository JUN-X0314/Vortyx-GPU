// Store tests (Phase 11) — the TypeScript mirror of the C++ store tests
// (tests/test_platform.cpp). They pin the InMemoryPlatformStore as the
// executable specification of the control-plane contract: ownership,
// idempotency, lifecycle, failure honesty, and the RLS-equivalence rule
// (foreign records are invisible -> not_found, never forbidden).

import { test } from "node:test";
import assert from "node:assert/strict";

import { makeAuthenticated } from "../src/auth.ts";
import { InMemoryPlatformStore } from "../src/memory-store.ts";
import type { DeviceMetadata, JobEnvelope, ResultEnvelope } from "../src/types.ts";
import { PROTOCOL_VERSION } from "../src/types.ts";

const alice = makeAuthenticated("user-alice");
const bob = makeAuthenticated("user-bob");

function validMetadata(): DeviceMetadata {
  return {
    protocol_version: PROTOCOL_VERSION,
    software_version: "0.11.0",
    operating_system: "linux",
    architecture: "x86_64",
    backends: ["cpu"],
    operations: ["vector_add"],
    display_name: "workstation",
  };
}

function validEnvelope(jobId: string): JobEnvelope {
  return {
    job_id: jobId,
    operation: "vector_add",
    element_count: 1024,
    requested_backend: "cpu",
    priority: 0,
    protocol_version: PROTOCOL_VERSION,
    created_at_ms: null,
  };
}

test("device registration fills server-managed fields from the authenticated subject", async () => {
  const store = new InMemoryPlatformStore();
  const result = await store.registerDevice(alice, "dev-1", validMetadata());
  assert.equal(result.status, "ok");
  if (result.status !== "ok") return;
  assert.equal(result.record.owner_user_id, "user-alice");
  assert.equal(result.record.status, "online");
  assert.ok(result.record.last_seen_ms !== null);
  assert.equal(result.record.last_seen_ms, result.record.created_at_ms);
});

test("device registration refusals are precise", async () => {
  const store = new InMemoryPlatformStore();
  const badProtocol = { ...validMetadata(), protocol_version: "9" };
  assert.equal((await store.registerDevice(alice, "dev-bad", badProtocol)).status, "invalid_input");
  assert.equal((await store.registerDevice(makeAuthenticated(""), "dev-x", validMetadata())).status, "unauthenticated");
  assert.equal((await store.registerDevice({ authenticated: false, user_id: "x" }, "dev-x", validMetadata())).status, "unauthenticated");
  await store.registerDevice(alice, "dev-1", validMetadata());
  assert.equal((await store.registerDevice(alice, "dev-1", validMetadata())).status, "conflict");
  assert.equal((await store.registerDevice(bob, "dev-1", validMetadata())).status, "conflict");
});

test("single-record device access: foreign records are invisible", async () => {
  const store = new InMemoryPlatformStore();
  await store.registerDevice(alice, "dev-1", validMetadata());
  const own = await store.device(alice, "dev-1");
  assert.equal(own.status, "ok");
  assert.equal((await store.device(bob, "dev-1")).status, "not_found");
  assert.equal((await store.device(alice, "dev-missing")).status, "not_found");
  assert.equal((await store.heartbeatDevice(bob, "dev-1")).status, "not_found");

  const heartbeat = await store.heartbeatDevice(alice, "dev-1");
  assert.equal(heartbeat.status, "ok");
  if (heartbeat.status === "ok") {
    assert.equal(heartbeat.record.status, "online");
    assert.ok(heartbeat.record.last_seen_ms !== null);
  }
});

test("device lists contain exactly the caller's devices in insertion order", async () => {
  const store = new InMemoryPlatformStore();
  await store.registerDevice(alice, "dev-1", validMetadata());
  await store.registerDevice(bob, "dev-bob", validMetadata());
  await store.registerDevice(alice, "dev-2", validMetadata());
  const list = await store.devices(alice);
  assert.equal(list.status, "ok");
  if (list.status !== "ok") return;
  assert.deepEqual(
    list.record.map((device) => device.device_id),
    ["dev-1", "dev-2"],
  );
});

test("job submission is idempotent by job_id", async () => {
  const store = new InMemoryPlatformStore();
  await store.registerDevice(alice, "dev-1", validMetadata());

  const first = await store.createJob(alice, validEnvelope("job-1"), "dev-1");
  assert.equal(first.status, "ok");
  if (first.status === "ok") {
    assert.equal(first.created, true);
    assert.equal(first.record.status, "queued");
    assert.equal(first.record.owner_user_id, "user-alice");
    assert.equal(first.record.submitted_by_device_id, "dev-1");
    assert.ok(first.record.created_at_ms !== null);
    assert.equal(first.record.started_at_ms, null);
  }

  const replay = await store.createJob(alice, validEnvelope("job-1"), "dev-1");
  assert.equal(replay.status, "ok");
  if (replay.status === "ok") assert.equal(replay.created, false, "replay returns the existing record");

  const changed = { ...validEnvelope("job-1"), element_count: 2048 };
  assert.equal((await store.createJob(alice, changed, "dev-1")).status, "conflict");
  assert.equal((await store.createJob(bob, validEnvelope("job-1"), null)).status, "conflict");
});

test("job submission refusals are precise", async () => {
  const store = new InMemoryPlatformStore();
  await store.registerDevice(alice, "dev-1", validMetadata());
  assert.equal(
    (await store.createJob(alice, validEnvelope("job-2"), "dev-bob")).status,
    "forbidden",
    "foreign submitting device is forbidden",
  );
  assert.equal(
    (await store.createJob(alice, validEnvelope("job-2"), "dev-unknown")).status,
    "forbidden",
    "unknown submitting device is forbidden (no existence leak)",
  );
  const zero = validEnvelope("job-3");
  zero.element_count = 0;
  assert.equal((await store.createJob(alice, zero, null)).status, "invalid_input");
  const badProtocol = validEnvelope("job-4");
  badProtocol.protocol_version = "2";
  assert.equal((await store.createJob(alice, badProtocol, null)).status, "invalid_input");
});

test("single-record job access: foreign records are invisible", async () => {
  const store = new InMemoryPlatformStore();
  await store.createJob(alice, validEnvelope("job-1"), null);
  assert.equal((await store.job(alice, "job-1")).status, "ok");
  assert.equal((await store.job(bob, "job-1")).status, "not_found");
  assert.equal((await store.job(alice, "job-missing")).status, "not_found");

  const list = await store.jobs(bob);
  assert.equal(list.status, "ok");
  if (list.status === "ok") assert.equal(list.record.length, 0);
});

test("lifecycle transitions follow the documented table", async () => {
  const store = new InMemoryPlatformStore();
  await store.createJob(alice, validEnvelope("job-1"), null);

  assert.equal(
    (await store.updateJob(alice, "job-1", "completed", "")).status,
    "invalid_input",
    "queued -> completed is illegal (never ran)",
  );
  assert.equal(
    (await store.updateJob(alice, "job-1", "failed", "boom")).status,
    "invalid_input",
    "queued -> failed is illegal (never ran)",
  );

  const started = await store.updateJob(alice, "job-1", "running", "");
  assert.equal(started.status, "ok");
  if (started.status === "ok") assert.ok(started.record.started_at_ms !== null);

  assert.equal(
    (await store.updateJob(alice, "job-1", "failed", "")).status,
    "invalid_input",
    "a failed transition requires an error reason",
  );

  const failed = await store.updateJob(alice, "job-1", "failed", "executor crashed");
  assert.equal(failed.status, "ok");
  if (failed.status === "ok") {
    assert.equal(failed.record.status, "failed");
    assert.equal(failed.record.error, "executor crashed");
    assert.ok(failed.record.completed_at_ms !== null);
  }
  assert.equal(
    (await store.cancelJob(alice, "job-1")).status,
    "invalid_input",
    "cancelling a terminal job is an illegal transition",
  );
});

test("owner cancellation records the cancelled state and reason", async () => {
  const store = new InMemoryPlatformStore();
  await store.createJob(alice, validEnvelope("job-2"), null);
  const cancelled = await store.cancelJob(alice, "job-2");
  assert.equal(cancelled.status, "ok");
  if (cancelled.status === "ok") {
    assert.equal(cancelled.record.status, "cancelled");
    assert.equal(cancelled.record.error, "cancelled");
    assert.ok(cancelled.record.completed_at_ms !== null);
  }
  assert.equal((await store.cancelJob(bob, "job-2")).status, "not_found");
});

test("results: outcome recording with honest rules", async () => {
  const store = new InMemoryPlatformStore();
  await store.createJob(alice, validEnvelope("job-1"), null);
  await store.updateJob(alice, "job-1", "running", "");

  const result: ResultEnvelope = {
    job_id: "job-1",
    status: "completed",
    backend: "cpu",
    error: "",
    result_element_count: 1024,
  };
  assert.equal(
    (await store.putResult(alice, { ...result, job_id: "job-nope" })).status,
    "not_found",
  );
  const stored = await store.putResult(alice, result);
  assert.equal(stored.status, "ok");

  const job = await store.job(alice, "job-1");
  if (job.status === "ok") {
    assert.equal(job.record.status, "completed");
    assert.ok(job.record.completed_at_ms !== null);
    assert.equal(job.record.error, "");
  }
  assert.equal((await store.putResult(alice, result)).status, "conflict", "one outcome per job");
  const fetched = await store.result(alice, "job-1");
  assert.equal(fetched.status, "ok");
  assert.equal((await store.result(bob, "job-1")).status, "not_found");
  assert.equal((await store.result(alice, "job-no-result")).status, "not_found");
});

test("results: a failure without a reason is refused everywhere", async () => {
  const store = new InMemoryPlatformStore();
  await store.createJob(alice, validEnvelope("job-2"), null);
  await store.updateJob(alice, "job-2", "running", "");
  const failure: ResultEnvelope = {
    job_id: "job-2",
    status: "failed",
    backend: "vulkan",
    error: "",
    result_element_count: null,
  };
  assert.equal((await store.putResult(alice, failure)).status, "invalid_input");
  const recorded = await store.putResult(alice, { ...failure, error: "vulkan device lost" });
  assert.equal(recorded.status, "ok");
  const job = await store.job(alice, "job-2");
  if (job.status === "ok") assert.equal(job.record.error, "vulkan device lost");
});

test("results: cancellation is not an outcome", async () => {
  const store = new InMemoryPlatformStore();
  await store.createJob(alice, validEnvelope("job-3"), null);
  await store.updateJob(alice, "job-3", "running", "");
  // The static result type already forbids "cancelled"; the WIRE can still
  // lie, and this assertion pins that the store refuses it at runtime —
  // the cast exists to reach the runtime boundary the test exists to check.
  const wireLie = {
    job_id: "job-3",
    status: "cancelled",
    backend: "",
    error: "",
    result_element_count: null,
  } as unknown as Parameters<InMemoryPlatformStore["putResult"]>[1];
  assert.equal((await store.putResult(alice, wireLie)).status, "invalid_input");
});

test("results: a result for a never-started job is refused", async () => {
  const store = new InMemoryPlatformStore();
  await store.createJob(alice, validEnvelope("job-4"), null);
  assert.equal(
    (
      await store.putResult(alice, {
        job_id: "job-4",
        status: "completed",
        backend: "cpu",
        error: "",
        result_element_count: null,
      })
    ).status,
    "invalid_input",
  );
});
