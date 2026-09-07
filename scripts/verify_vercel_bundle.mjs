#!/usr/bin/env node
// Phase 17 — bundle integrity + runtime smoke for the production API
// function (the deployment gate the old pipeline lacked).
//
// What it does (runtime behavior, NOT source-existence checks):
//   1. Builds the EXACT artifact Vercel builds — the same esbuild bundling
//      step as scripts/build-api-bundle.mjs, into a temp directory.
//   2. Asserts the bundle carries no literal "*.ts" runtime import
//      specifier (the residue that produced ERR_MODULE_NOT_FOUND in
//      production).
//   3. IMPORTS the bundle under the real Node runtime — any missing module
//      fails here, with the failing specifier named.
//   4. Invokes the handler end to end against the memory store:
//      /api/health, /api/platform/info, an unknown route (404), a wrong
//      method (405) and a CORS preflight (204) — the deployment contract.
//   5. Cross-checks the deployed software version against the single
//      version source (platform/api/src/version.ts).
//
// Exit codes: 0 = all checks pass; 1 = at least one FAIL.
import { build } from "esbuild";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, "..");

let failures = 0;
function check(name, ok, detail = "") {
  console.log(`${ok ? "PASS" : "FAIL"}: ${name}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures += 1;
}

const outDir = mkdtempSync(join(tmpdir(), "vortyx-bundle-"));
const outfile = join(outDir, "api", "[...route].js");

try {
  // 1. Build the same artifact the deployment builds.
  await build({
    entryPoints: [join(repo, "api", "[...route].ts")],
    outfile,
    bundle: true,
    platform: "node",
    format: "esm",
    target: "node22",
    sourcemap: false,
    legalComments: "none",
    logLevel: "silent",
    packages: "bundle",
  });
  const bytes = readFileSync(outfile);
  check("the API bundle builds", bytes.length > 0, `${bytes.length} bytes`);

  // 2. No residual cross-directory .ts import may survive bundling.
  const bundled = bytes.toString("utf8");
  const residual = /(?:from\s*|import\()\s*["'][^"']*\.ts["']/.exec(bundled);
  check(
    "the bundle contains no runtime .ts import specifier",
    residual === null,
    residual === null ? "" : `residual specifier pattern found (production ERR_MODULE_NOT_FOUND shape)`,
  );

  // 5a. The expected version, straight from the single source.
  const { SOFTWARE_VERSION } = await import(
    pathToFileURL(join(repo, "platform", "api", "src", "version.ts")).href
  );

  // 3. Import the artifact — the real resolution test.
  const mod = await import(pathToFileURL(outfile).href);
  check(
    "the bundled function module imports (every runtime dependency resolves)",
    typeof mod.default === "function",
  );

  // 4. Invoke the handler exactly like the deployment wiring tests do.
  function mockResponse() {
    const captured = { status: 0, body: undefined, headers: {} };
    const res = {
      status(code) {
        captured.status = code;
        return res;
      },
      json(body) {
        captured.body = body;
      },
    };
    return { res, captured };
  }
  async function call(method, url) {
    const { res, captured } = mockResponse();
    await mod.default({ method, url, headers: {} }, res);
    return captured;
  }

  const health = await call("GET", "https://vortyx.test/api/health");
  check("bundled GET /api/health -> 200", health.status === 200, `got ${health.status}`);
  check(
    "health body reports the documented contract",
    health.body?.status === "ok" &&
      health.body?.protocol_version === "1" &&
      health.body?.store === "memory" &&
      health.body?.config_error === null,
    JSON.stringify(health.body),
  );
  check(
    "health reports the deployed software version",
    health.body?.software_version === SOFTWARE_VERSION,
    `bundle=${health.body?.software_version} source=${SOFTWARE_VERSION}`,
  );

  const info = await call("GET", "https://vortyx.test/api/platform/info");
  check("bundled GET /api/platform/info -> 200", info.status === 200, `got ${info.status}`);
  check(
    "platform/info agrees on the version and protocol",
    info.body?.software_version === SOFTWARE_VERSION && info.body?.protocol_version === "1",
    JSON.stringify({ software_version: info.body?.software_version, protocol_version: info.body?.protocol_version }),
  );

  const missing = await call("GET", "https://vortyx.test/api/definitely/not/a/route");
  check(
    "bundled unknown route stays an honest 404",
    missing.status === 404 && missing.body?.error?.code === "not_found",
    `got ${missing.status}`,
  );

  const wrongMethod = await call("POST", "https://vortyx.test/api/health");
  check(
    "bundled wrong method stays a 405",
    wrongMethod.status === 405 && wrongMethod.body?.error?.code === "method_not_allowed",
    `got ${wrongMethod.status}`,
  );

  const preflight = await call("OPTIONS", "https://vortyx.test/api/projects");
  check("bundled CORS preflight answers 204", preflight.status === 204, `got ${preflight.status}`);
} catch (error) {
  const message = error instanceof Error ? error.message : String(error);
  check("bundle build + import + invoke", false, message);
} finally {
  rmSync(outDir, { recursive: true, force: true });
}

console.log(failures === 0 ? "[PASS] all production API runtime dependencies resolved" : "[FAIL] bundle integrity gate failed");
process.exit(failures === 0 ? 0 : 1);
