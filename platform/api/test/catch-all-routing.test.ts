// Deployment wiring test (Phase 16.0.1).
//
// The repository-root Vercel catch-all (api/[...route].ts) is the
// production entry point for /api/* on the single-project deployment: the
// same Vercel project that serves platform/web routes every /api request
// through THIS file. These tests import that file — the real deployment
// glue, never a copy — and pin its contract:
//
//   * it reaches the REAL router (platform/api/src/vercel.ts), so the
//     responses are the documented API contract, not a stub;
//   * the default store is the honest local default (memory) with a null
//     config_error, so a missing server-side env cannot silently fake a
//     Supabase deployment (production must report store "supabase");
//   * the deployed software version is 0.16.1 (the version agreement is
//     checked at the outermost wiring layer, not only inside the package);
//   * unknown routes stay honest 404s and wrong methods stay 405s through
//     the glue (a catch-all that swallows routing errors would mask both).
//
// No network, no secrets: the handler runs exactly as a cold Vercel
// function would, against the memory store.

import { test } from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// The catch-all binds the platform from process.env on first use; the tests
// must observe the documented default (memory store), not an ambient
// variable inherited from the invoking shell.
delete process.env["VORTYX_STORE"];

// Bracketed catch-all filenames are valid on disk but must not be parsed
// as raw URL syntax — resolve through the filesystem and convert explicitly.
const here = path.dirname(fileURLToPath(import.meta.url));
// platform/api/test -> platform/api -> platform -> repository root.
const catchAllPath = path.resolve(here, "..", "..", "..", "api", "[...route].ts");
const catchAll = await import(pathToFileURL(catchAllPath).href);

interface CapturedResponse {
  status: number;
  body: unknown;
}

function mockResponse() {
  const captured: CapturedResponse = { status: 0, body: undefined };
  const res = {
    status(code: number) {
      captured.status = code;
      return res;
    },
    json(body: unknown) {
      captured.body = body;
    },
  };
  return { res, captured };
}

async function call(method: string, url: string): Promise<CapturedResponse> {
  const { res, captured } = mockResponse();
  await catchAll.default({ method, url, headers: {} }, res);
  return captured;
}

test("the root catch-all is the real createApiHandler binding", () => {
  assert.equal(typeof catchAll.default, "function");
});

test("GET /api/health through the deployment glue reaches the real router", async () => {
  const response = await call("GET", "https://vortyx.test/api/health");
  assert.equal(response.status, 200);
  const body = response.body as Record<string, unknown>;
  assert.equal(body.status, "ok");
  assert.equal(body.protocol_version, "1");
  // The honest local default: the memory store, no configuration error.
  // A real deployment must report store "supabase" (checked in the smoke
  // checklist); a missing server-side env never fakes one here.
  assert.equal(body.store, "memory");
  assert.equal(body.config_error, null);
});

test("GET /api/platform/info through the glue reports the deployed version", async () => {
  const response = await call("GET", "https://vortyx.test/api/platform/info");
  assert.equal(response.status, 200);
  const body = response.body as Record<string, unknown>;
  assert.equal(body.software_version, "0.16.1");
  assert.equal(body.protocol_version, "1");
});

test("an unknown /api route stays an honest 404 through the glue", async () => {
  const response = await call("GET", "https://vortyx.test/api/definitely/not/a/route");
  assert.equal(response.status, 404);
  const body = response.body as { error?: { code?: string } };
  assert.equal(body.error?.code, "not_found");
});

test("a wrong method stays a 405 through the glue", async () => {
  const response = await call("POST", "https://vortyx.test/api/health");
  assert.equal(response.status, 405);
  const body = response.body as { error?: { code?: string } };
  assert.equal(body.error?.code, "method_not_allowed");
});

test("CORS preflight is answered through the glue", async () => {
  const response = await call("OPTIONS", "https://vortyx.test/api/projects");
  assert.equal(response.status, 204);
});
