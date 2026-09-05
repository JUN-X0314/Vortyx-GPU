// Contract tests (Phase 11) — the TypeScript mirror of the C++ contract
// tests (tests/test_platform_contract.cpp). Both suites pin the SAME wire
// vocabulary: error codes, status mapping, response field order, and strict
// request validation. A failure here means the two layers could drift.

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  ERR_CONFLICT,
  ERR_INVALID_ENUM,
  ERR_INVALID_ID,
  ERR_INVALID_JSON,
  ERR_INVALID_TYPE,
  ERR_INVALID_VALUE,
  ERR_METHOD_NOT_ALLOWED,
  ERR_MISSING_FIELD,
  ERR_NOT_FOUND,
  ERR_UNAUTHENTICATED,
  ERR_UNSUPPORTED_PROTOCOL,
  errorBody,
  httpStatus,
  parseCreateJob,
  parseRegisterDevice,
  platformInfo,
  serializeDevice,
  serializeJob,
  serializeResult,
  storeErrorCode,
} from "../src/contract.ts";
import type {
  DeviceRecord,
  JobRecord,
  PlatformStatus,
  ResultEnvelope,
} from "../src/types.ts";

test("http status mapping matches the C++ contract", () => {
  assert.equal(httpStatus("ok", ""), 200);
  assert.equal(httpStatus("invalid_input", ERR_INVALID_JSON), 400);
  assert.equal(httpStatus("invalid_input", ERR_MISSING_FIELD), 422);
  assert.equal(httpStatus("unauthenticated", ERR_UNAUTHENTICATED), 401);
  assert.equal(httpStatus("forbidden", "forbidden"), 403);
  assert.equal(httpStatus("not_found", ERR_NOT_FOUND), 404);
  assert.equal(httpStatus("conflict", ERR_CONFLICT), 409);
  assert.equal(httpStatus("internal", "internal_error"), 500);
});

test("error body schema is unified", () => {
  assert.deepEqual(errorBody("not_found", "no such job"), {
    error: { code: "not_found", message: "no such job" },
  });
});

test("store outcomes map to stable error codes", () => {
  const mapping: Array<[PlatformStatus, string]> = [
    ["ok", ""],
    ["invalid_input", "invalid_request"],
    ["unauthenticated", "unauthenticated"],
    ["forbidden", "forbidden"],
    ["not_found", "not_found"],
    ["conflict", "conflict"],
    ["internal", "internal_error"],
  ];
  for (const [status, code] of mapping) {
    assert.equal(storeErrorCode(status), code);
  }
});

test("register device: a valid body parses fully", () => {
  const body = {
    device_id: "dev-1",
    protocol_version: "1",
    software_version: "0.11.0",
    operating_system: "linux",
    architecture: "x86_64",
    display_name: "workstation",
    backends: ["cpu", "vulkan"],
    operations: ["vector_add", "vector_scale"],
  };
  const parsed = parseRegisterDevice(body);
  assert.ok(parsed.ok);
  assert.equal(parsed.value.device_id, "dev-1");
  assert.equal(parsed.value.metadata.software_version, "0.11.0");
  assert.equal(parsed.value.metadata.display_name, "workstation");
  assert.deepEqual(parsed.value.metadata.backends, ["cpu", "vulkan"]);
  assert.deepEqual(parsed.value.metadata.operations, ["vector_add", "vector_scale"]);
});

test("register device: every violation carries its precise code", () => {
  const cases: Array<{ body: unknown; code: string; label: string }> = [
    {
      body: { protocol_version: "1", software_version: "v" },
      code: ERR_MISSING_FIELD,
      label: "missing device_id",
    },
    {
      body: { device_id: "dev-1", software_version: "v" },
      code: ERR_MISSING_FIELD,
      label: "missing protocol_version",
    },
    {
      body: { device_id: "dev-1", protocol_version: "1" },
      code: ERR_MISSING_FIELD,
      label: "missing software_version",
    },
    {
      body: { device_id: "dev-1", protocol_version: "9", software_version: "v" },
      code: ERR_UNSUPPORTED_PROTOCOL,
      label: "unsupported protocol version",
    },
    {
      body: { device_id: "bad id", protocol_version: "1", software_version: "v" },
      code: ERR_INVALID_ID,
      label: "invalid id characters",
    },
    {
      body: { device_id: 5, protocol_version: "1", software_version: "v" },
      code: ERR_INVALID_TYPE,
      label: "wrong field type",
    },
    {
      body: {
        device_id: "dev-1",
        protocol_version: "1",
        software_version: "v",
        backends: ["cuda"],
      },
      code: ERR_INVALID_ENUM,
      label: "unknown backend",
    },
    {
      body: {
        device_id: "dev-1",
        protocol_version: "1",
        software_version: "v",
        operations: ["vector_add", "vector_add"],
      },
      code: ERR_INVALID_VALUE,
      label: "duplicate operation",
    },
    {
      body: { device_id: "dev-1", protocol_version: "1", software_version: "v", ghost: 1 },
      code: ERR_INVALID_VALUE,
      label: "unknown field rejected",
    },
    {
      body: { device_id: "dev-1", protocol_version: "1", software_version: "v", backends: "cpu" },
      code: ERR_INVALID_TYPE,
      label: "backends must be an array",
    },
  ];
  for (const testCase of cases) {
    const parsed = parseRegisterDevice(testCase.body);
    assert.ok(!parsed.ok, `${testCase.label}: expected rejection`);
    assert.equal(parsed.code, testCase.code, testCase.label);
    assert.ok(parsed.message.length > 0, `${testCase.label}: needs a human message`);
  }
});

test("create job: a valid body parses fully (including optionals)", () => {
  const parsed = parseCreateJob({
    job_id: "job-1",
    operation: "vector_add",
    element_count: 1024,
    requested_backend: "cpu",
    priority: 3,
    protocol_version: "1",
    created_at_ms: 1700000000000,
  });
  assert.ok(parsed.ok);
  assert.equal(parsed.value.envelope.job_id, "job-1");
  assert.equal(parsed.value.envelope.operation, "vector_add");
  assert.equal(parsed.value.envelope.element_count, 1024);
  assert.equal(parsed.value.envelope.requested_backend, "cpu");
  assert.equal(parsed.value.envelope.priority, 3);
  assert.equal(parsed.value.envelope.created_at_ms, 1700000000000);
});

test("create job: optional fields default honestly", () => {
  const parsed = parseCreateJob({
    job_id: "j",
    operation: "vector_scale",
    element_count: 8,
    protocol_version: "1",
  });
  assert.ok(parsed.ok);
  assert.equal(parsed.value.envelope.requested_backend, "");
  assert.equal(parsed.value.envelope.priority, 0);
  assert.equal(parsed.value.envelope.created_at_ms, null);
});

test("create job: every violation carries its precise code", () => {
  const base = { job_id: "j", operation: "vector_add", element_count: 4, protocol_version: "1" };
  const cases: Array<[unknown, string, string]> = [
    [{ operation: "vector_add", element_count: 4, protocol_version: "1" }, ERR_MISSING_FIELD, "missing job_id"],
    [{ job_id: "j", element_count: 4, protocol_version: "1" }, ERR_MISSING_FIELD, "missing operation"],
    [
      { ...base, operation: "matrix_multiply" },
      ERR_INVALID_ENUM,
      "unknown operation",
    ],
    [{ ...base, operation: 7 }, ERR_INVALID_TYPE, "operation must be a string"],
    [{ ...base, element_count: undefined }, ERR_MISSING_FIELD, "missing element_count"],
    [{ ...base, element_count: 0 }, ERR_INVALID_VALUE, "zero element_count"],
    [{ ...base, element_count: -5 }, ERR_INVALID_VALUE, "negative element_count"],
    [{ ...base, element_count: 1.5 }, ERR_INVALID_VALUE, "non-integral element_count"],
    [{ ...base, element_count: "4" }, ERR_INVALID_TYPE, "element_count must be a number"],
    [{ ...base, requested_backend: "cuda" }, ERR_INVALID_ENUM, "unknown requested_backend"],
    [{ ...base, priority: 99999999999 }, ERR_INVALID_VALUE, "priority outside int32"],
    [{ ...base, protocol_version: "2" }, ERR_UNSUPPORTED_PROTOCOL, "unsupported protocol"],
    [{ ...base, surprise: true }, ERR_INVALID_VALUE, "unknown field rejected"],
    [{ ...base, submitted_by_device_id: 7 }, ERR_INVALID_TYPE, "device reference must be a string"],
  ];
  for (const [body, code, label] of cases) {
    const parsed = parseCreateJob(body);
    assert.ok(!parsed.ok, `${label}: expected rejection`);
    assert.equal(parsed.code, code, label);
  }
});

function sampleDevice(): DeviceRecord {
  return {
    device_id: "dev-1",
    owner_user_id: "user-alice",
    metadata: {
      protocol_version: "1",
      software_version: "0.11.0",
      operating_system: "linux",
      architecture: "x86_64",
      backends: ["cpu"],
      operations: ["vector_add"],
      display_name: "workstation",
    },
    status: "online",
    last_seen_ms: 1700000000000,
    created_at_ms: 1700000000000,
  };
}

function sampleJob(): JobRecord {
  return {
    job: {
      job_id: "job-1",
      operation: "vector_scale",
      element_count: 64,
      requested_backend: "vulkan",
      priority: -2,
      protocol_version: "1",
      created_at_ms: null,
    },
    owner_user_id: "user-alice",
    submitted_by_device_id: "dev-1",
    status: "queued",
    error: "",
    created_at_ms: 1700000000000,
    started_at_ms: null,
    completed_at_ms: null,
  };
}

test("serializers use the documented field order and exact values", () => {
  const deviceJson = JSON.stringify(serializeDevice(sampleDevice()));
  assert.ok(deviceJson.startsWith('{"device_id":"dev-1"'), "device_id comes first");
  assert.ok(deviceJson.endsWith('"created_at_ms":1700000000000}'), "created_at_ms comes last");

  const jobJson = JSON.stringify(serializeJob(sampleJob()));
  const job = JSON.parse(jobJson) as Record<string, unknown>;
  assert.equal(job["operation"], "vector_scale");
  assert.equal(job["element_count"], 64);
  assert.equal(job["priority"], -2);
  assert.equal(job["status"], "queued");
  assert.equal(job["error"], "");
  assert.equal(job["submitted_by_device_id"], "dev-1");
  assert.equal(job["started_at_ms"], null, "unset timestamps are null, never 0");
  assert.equal(job["completed_at_ms"], null);

  const result: ResultEnvelope = {
    job_id: "job-1",
    status: "completed",
    backend: "vulkan",
    error: "",
    result_element_count: 64,
  };
  const resultJson = JSON.parse(JSON.stringify(serializeResult(result))) as Record<string, unknown>;
  assert.equal(resultJson["status"], "completed");
  assert.equal(resultJson["result_element_count"], 64);

  const info = JSON.parse(JSON.stringify(platformInfo("0.11.0"))) as Record<string, unknown>;
  assert.equal(info["protocol_version"], "1");
  assert.equal(info["software_version"], "0.11.0");
  assert.equal((info["operations"] as string[]).length, 3);
  assert.equal((info["backends"] as string[]).length, 2);
});

test("method-not-allowed has its own status", () => {
  assert.equal(ERR_METHOD_NOT_ALLOWED, "method_not_allowed");
});
