#!/usr/bin/env node
// Phase 17 — production HTTP smoke test (the executable form of the smoke
// checks in docs/platform/deployment-checklist.md §3).
//
// Deterministic, secret-free, redirect-intolerant: a deployment-protection
// or SSO redirect is NEVER a passing check (fetch runs with
// redirect: "manual" so a 3xx is reported as the failure it is).
//
// Env:
//   VORTYX_PROD_URL         the production origin
//                           (default https://vortyx-gpu-platform.vercel.app)
//   VORTYX_EXPECT_VERSION   expected software_version
//                           (default: read from platform/api/src/version.ts)
//
// Exit codes: 0 = every check passes; 1 = at least one FAIL.
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const repo = join(dirname(fileURLToPath(import.meta.url)), "..");
const base = (process.env.VORTYX_PROD_URL ?? "https://vortyx-gpu-platform.vercel.app").replace(/\/+$/, "");

let failures = 0;
function check(name, ok, detail = "") {
  console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures += 1;
}

const expectedVersion =
  process.env.VORTYX_EXPECT_VERSION ??
  (await import(pathToFileURL(join(repo, "platform", "api", "src", "version.ts")).href)).SOFTWARE_VERSION;

async function request(method, path, init = {}) {
  return fetch(`${base}${path}`, { redirect: "manual", ...init, method });
}

function isRedirect(response) {
  return response.status >= 300 && response.status < 400;
}

// 1. The root serves the real console HTML.
{
  const response = await request("GET", "/");
  const body = response.status === 200 ? await response.text() : "";
  const redirected = isRedirect(response);
  check(
    "GET / -> 200 (static console, no deployment-protection/SSO redirect)",
    response.status === 200 && !redirected && /<html/i.test(body),
    redirected
      ? `status ${response.status} with location ${response.headers.get("location") ?? "?"} (an SSO redirect page is NOT a passing smoke check)`
      : `status ${response.status}`,
  );
}

// 2. The committed publishable browser config, as actually served.
let supabaseUrl = null;
{
  const response = await request("GET", "/js/config.js");
  const body = response.status === 200 ? await response.text() : "";
  const keys = [...body.matchAll(/^\s{2}([A-Za-z_][A-Za-z0-9_]*)\s*:/gm)].map((m) => m[1]);
  const exactlyThree =
    keys.length === 3 && keys.includes("supabaseUrl") && keys.includes("supabaseAnonKey") && keys.includes("apiBaseUrl");
  const urlMatch = /supabaseUrl:\s*"(https:\/\/[^"]+)"/.exec(body);
  supabaseUrl = urlMatch === null ? null : urlMatch[1];
  const secretFree =
    !/sb_secret_/.test(body) && !/service_role/.test(body) && !/process\.env/.test(body) && !/eyJ/.test(body);
  check(
    "GET /js/config.js -> 200 with EXACTLY the three publishable fields",
    response.status === 200 && exactlyThree && secretFree,
    `status ${response.status}; fields [${keys.join(", ")}]`,
  );
  check(
    "served config.js carries no placeholder and no secret marker",
    supabaseUrl !== null &&
      /sb_publishable_/.test(body) &&
      !/PASTE_|YOUR_|_HERE|placeholder/i.test(body),
  );
}

// 3. /api/health — the deployment's own readiness contract.
{
  const response = await request("GET", "/api/health");
  let body = null;
  try {
    body = await response.json();
  } catch {
    body = null;
  }
  check(
    "GET /api/health -> 200 with store=supabase and config_error=null",
    response.status === 200 &&
      body?.status === "ok" &&
      body?.store === "supabase" &&
      body?.config_error === null,
    `status ${response.status}; body ${JSON.stringify(body)}`,
  );
  check(
    "/api/health reports the deployed software version",
    body?.software_version === expectedVersion,
    `got ${JSON.stringify(body?.software_version)} expected ${expectedVersion}`,
  );
  check(
    "/api/health exposes no CORS surface (same-origin deployment)",
    response.headers.get("access-control-allow-origin") === null,
    `got ${JSON.stringify(response.headers.get("access-control-allow-origin"))}`,
  );
}

// 4. /api/platform/info — the version contract.
{
  const response = await request("GET", "/api/platform/info");
  let body = null;
  try {
    body = await response.json();
  } catch {
    body = null;
  }
  check(
    "GET /api/platform/info -> 200 with protocol 1 and the deployed version",
    response.status === 200 && body?.protocol_version === "1" && body?.software_version === expectedVersion,
    `status ${response.status}; body ${JSON.stringify({ protocol_version: body?.protocol_version, software_version: body?.software_version })}`,
  );
}

// 5. Error semantics stay honest through the deployed function.
{
  const missing = await request("GET", "/api/phase17-smoke-no-such-route");
  let missingBody = null;
  try {
    missingBody = await missing.json();
  } catch {
    missingBody = null;
  }
  check(
    "GET /api/<unknown> -> 404 not_found (never a 500)",
    missing.status === 404 && missingBody?.error?.code === "not_found",
    `status ${missing.status}; body ${JSON.stringify(missingBody)}`,
  );

  const wrongMethod = await request("POST", "/api/health");
  let methodBody = null;
  try {
    methodBody = await wrongMethod.json();
  } catch {
    methodBody = null;
  }
  check(
    "POST /api/health -> 405 method_not_allowed (never a 500)",
    wrongMethod.status === 405 && methodBody?.error?.code === "method_not_allowed",
    `status ${wrongMethod.status}; body ${JSON.stringify(methodBody)}`,
  );

  const preflight = await request("OPTIONS", "/api/projects");
  check("OPTIONS /api/projects -> 204 (preflight answered)", preflight.status === 204, `status ${preflight.status}`);
}

// 6. Supabase Auth connectivity from the DEPLOYED config (the real wiring).
{
  if (supabaseUrl === null) {
    check("Supabase auth connectivity via the deployed config", false, "no supabaseUrl in the served config.js");
  } else {
    const response = await fetch(`${supabaseUrl}/auth/v1/health`, {
      headers: { apikey: process.env.VORTYX_SMOKE_ANON_KEY ?? "" },
    });
    check(
      "Supabase /auth/v1/health -> 200 with the publishable key",
      response.status === 200,
      `status ${response.status}`,
    );
  }
}

console.log(failures === 0 ? `[PASS] production smoke: ${base}` : `[FAIL] production smoke: ${failures} failing check(s)`);
process.exit(failures === 0 ? 0 : 1);
