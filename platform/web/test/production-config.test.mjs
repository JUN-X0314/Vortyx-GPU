// Production configuration test (Phase 16.0.1).
//
// Since Phase 16.0.1 the committed js/config.js IS the deployed runtime
// configuration of the web console (publishable values only, by design).
// A regression here is exactly the production failure this phase fixes:
// a missing or placeholder config makes every boot land on the
// "Configuration required" screen. These tests pin the file at the repo
// level so the failure mode can never silently return:
//
//   * the file parses and sets window.VORTYX_CONFIG with EXACTLY the
//     three documented allowlisted fields (an allowlist violation — e.g.
//     a copied process.env — is a test failure, not a review catch);
//   * no placeholder values (the example file's YOUR-* markers);
//   * no server-only secret markers of any kind;
//   * the Supabase URL is a real https Supabase project origin;
//   * index.html loads config.js BEFORE the main.js module entry (the
//     load order the boot sequence depends on);
//   * the REAL boot path (auth.loadConfig) accepts the committed values.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import vm from "node:vm";

const webRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

function readConfigSource() {
  return readFileSync(path.join(webRoot, "js", "config.js"), "utf8");
}

function loadCommittedConfig() {
  // config.js is a classic browser script that assigns window.VORTYX_CONFIG;
  // run it in a sandbox with a stub window and read the result back.
  const sandbox = { window: {} };
  vm.runInNewContext(readConfigSource(), sandbox, { filename: "js/config.js" });
  return sandbox.window.VORTYX_CONFIG;
}

test("the committed runtime config parses and sets window.VORTYX_CONFIG", () => {
  const config = loadCommittedConfig();
  assert.ok(config !== undefined && typeof config === "object", "window.VORTYX_CONFIG must be set");
});

test("the config carries EXACTLY the three allowlisted publishable fields", () => {
  const config = loadCommittedConfig();
  assert.deepEqual(
    Object.keys(config).sort(),
    ["apiBaseUrl", "supabaseAnonKey", "supabaseUrl"],
    "no field beyond the documented allowlist may reach the browser",
  );
});

test("no placeholder values remain in the committed config", () => {
  const source = readConfigSource();
  assert.doesNotMatch(source, /YOUR-PROJECT|YOUR-PUBLISHABLE-ANON-KEY/i);
  assert.doesNotMatch(source, /placeholder/i);
});

test("no server-only secret value reaches the browser through the config", () => {
  // The values are what actually ships to the browser; comments may still
  // document that secrets are forbidden. The allowlisted-keys test above
  // already proves no extra field can ride along.
  const config = loadCommittedConfig();
  const shipped = JSON.stringify(config);
  assert.doesNotMatch(
    shipped,
    /sb_secret_|service[_-]?role[_-]?key|WORKER_TOKEN|RECONCILE_TOKEN|CRON_SECRET|POSTGRES_PASSWORD|DB_PASSWORD/i,
    "a server-only secret value must never reach the browser config",
  );
  const key = config.supabaseAnonKey;
  if (key.startsWith("sb_")) {
    // Supabase's new key format self-identifies: publishable keys start
    // with sb_publishable_, secret keys with sb_secret_ (excluded above).
    assert.match(key, /^sb_publishable_/);
  } else if (key.startsWith("eyJ")) {
    // Legacy JWT-shaped keys carry their role in the payload: only the
    // anon role may ship to a browser.
    const payload = JSON.parse(Buffer.from(key.split(".")[1], "base64url").toString("utf8"));
    assert.notEqual(payload.role, "service_role");
    assert.equal(payload.role, "anon");
  } else {
    assert.fail("the anon key is neither an sb_publishable_ key nor a recognizable legacy JWT");
  }
});

test("the config never copies process.env into the browser", () => {
  assert.doesNotMatch(readConfigSource(), /process\.env/);
});

test("supabaseUrl is a real https Supabase project origin", () => {
  const config = loadCommittedConfig();
  const url = new URL(config.supabaseUrl);
  assert.equal(url.protocol, "https:");
  assert.match(url.hostname, /\.supabase\.co$/);
});

test("every publishable value is a non-empty string", () => {
  const config = loadCommittedConfig();
  for (const key of ["supabaseUrl", "supabaseAnonKey"]) {
    assert.equal(typeof config[key], "string");
    assert.ok(config[key].length > 0, `${key} must be present`);
  }
  // apiBaseUrl "" is the documented same-origin single-project setting;
  // an absolute https origin is the documented multi-project alternative.
  assert.equal(typeof config.apiBaseUrl, "string");
  assert.ok(
    config.apiBaseUrl === "" || config.apiBaseUrl.startsWith("https://"),
    "apiBaseUrl must be the same-origin empty string or an https origin",
  );
});

test("index.html loads config.js before the main.js module entry", () => {
  const html = readFileSync(path.join(webRoot, "index.html"), "utf8");
  const configPos = html.indexOf("/js/config.js");
  const mainPos = html.indexOf("/js/main.js");
  assert.ok(configPos !== -1, "index.html must load /js/config.js");
  assert.ok(mainPos !== -1, "index.html must load /js/main.js");
  assert.ok(configPos < mainPos, "the runtime config must execute before the boot module");
});

test("the real boot path (auth.loadConfig) accepts the committed config", async () => {
  const config = loadCommittedConfig();
  globalThis.VORTYX_CONFIG = config;
  const { loadConfig } = await import("../js/auth.js");
  const loaded = loadConfig();
  assert.equal(loaded.supabaseUrl, config.supabaseUrl.replace(/\/$/, ""));
  assert.equal(loaded.supabaseAnonKey, config.supabaseAnonKey);

  // And the honest failure mode is preserved: without the config object
  // the boot still refuses (never a fake configured state).
  delete globalThis.VORTYX_CONFIG;
  assert.throws(() => loadConfig(), /not configured/);
});
