// Local development server for the Vortyx platform API (Phase 11, Phase 15
// service surface + static web console).
//
// Mimics the minimal Vercel Node.js function shape (method/url/headers/body
// -> status/json) so the SAME api/ handlers run locally with zero cloud
// resources. The store is the LOCAL/MOCK InMemoryPlatformStore +
// InMemoryServiceStore unless VORTYX_STORE=supabase is set (which requires
// a real Supabase project with migration 0003 applied).
//
//   npm run dev            # http://localhost:3000/api/health
//   PORT=8080 npm run dev  # custom port
//   VORTYX_WORKER_TOKEN=secret npm run dev   # enables the worker protocol
//
// Memory-mode state lives in THIS process: restart = empty control plane.
// That is the documented local/mock behavior, not a bug.
//
// Static web console (Phase 15): when ../web exists, GET / serves the
// console files from platform/web (same origin — no CORS needed locally).

import fs from "node:fs";
import path from "node:path";
import http from "node:http";
import { readConfig, resolvePlatform } from "./src/config.ts";
import { handlePlatformRequest } from "./src/router.ts";

const config = readConfig(process.env);
let resolved;
try {
  resolved = await resolvePlatform(config);
} catch (error) {
  console.error("[vortyx-platform] configuration error:", error instanceof Error ? error.message : error);
  console.error("[vortyx-platform] falling back to the local memory store so you can keep developing.");
  resolved = await resolvePlatform({ ...config, store: "memory", configError: null });
  resolved.storeKind = "memory";
}

if (resolved.storeKind === "memory") {
  console.warn(
    "[vortyx-platform] LOCAL/MOCK MODE (in-memory store): state is per-process, " +
      "persists nothing, and must never back a real deployment. " +
      "Set VORTYX_STORE=supabase with a configured project for the real backend.",
  );
}

if (resolved.workerToken === null) {
  console.warn(
    "[vortyx-platform] The worker protocol is DISABLED (no VORTYX_WORKER_TOKEN). " +
      "Jobs will stay queued with no native worker able to claim them.",
  );
}

function localTokenVerifierHint() {
  // Memory mode accepts the documented local scheme: Authorization: Bearer local:<user_id>
  console.warn('[vortyx-platform] Local auth scheme: "Authorization: Bearer local:<user_id>" (mock only).');
}
localTokenVerifierHint();

const port = Number(process.env["PORT"] ?? 3000);

// The web console directory (served at / when present).
const WEB_ROOT = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "web");
const MIME = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json",
  ".svg": "image/svg+xml",
  ".png": "image/png",
};

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url ?? "/", "http://localhost");

  // ---- static web console (same origin) ------------------------------------
  if (!url.pathname.startsWith("/api/") && fs.existsSync(WEB_ROOT)) {
    const relative = url.pathname === "/" ? "index.html" : url.pathname.slice(1);
    const candidate = path.normalize(path.join(WEB_ROOT, relative));
    if (candidate.startsWith(WEB_ROOT) && fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
      const extension = path.extname(candidate);
      res.writeHead(200, { "content-type": MIME[extension] ?? "application/octet-stream" });
      res.end(fs.readFileSync(candidate));
      return;
    }
    // SPA fallback: unknown non-API paths render the console shell.
    const shell = path.join(WEB_ROOT, "index.html");
    if (fs.existsSync(shell)) {
      res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      res.end(fs.readFileSync(shell));
      return;
    }
  }

  // ---- the API (the Vercel-function shape) ----------------------------------
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const rawBody = Buffer.concat(chunks).toString("utf8");

  let body = undefined;
  if (rawBody.length > 0) {
    // Pass the raw string: the router's parseBody replicates the Vercel
    // behavior (parsed JSON object, or the original string on parse failure
    // -> 400 invalid_json).
    try {
      body = JSON.parse(rawBody);
    } catch {
      body = rawBody;
    }
  }

  const query = Object.fromEntries(url.searchParams.entries());
  const authorization = req.headers["authorization"];
  const response = await handlePlatformRequest(
    {
      method: req.method ?? "GET",
      path: url.pathname,
      body,
      authorization: Array.isArray(authorization) ? authorization[0] : authorization,
      query,
    },
    {
      store: resolved.store,
      verifier: resolved.verifier,
      storeKind: resolved.storeKind,
      softwareVersion: resolved.softwareVersion,
      service: resolved.service,
      workerToken: resolved.workerToken ?? undefined,
      reconcileToken: resolved.reconcileToken ?? undefined,
      allowedOrigin: resolved.allowedOrigin,
    },
  );

  res.writeHead(response.status, {
    "content-type": "application/json",
    // Explicit framing: local clients (the C++ worker among them) get
    // Content-Length instead of chunked streaming when the body fits.
    ...(response.body === undefined ? {} : { "content-length": Buffer.byteLength(JSON.stringify(response.body)) }),
    ...(response.headers ?? {}),
  });
  res.end(JSON.stringify(response.body));
});

server.listen(port, () => {
  console.log(`[vortyx-platform] listening on http://localhost:${port} (store: ${resolved.storeKind})`);
  if (fs.existsSync(WEB_ROOT)) {
    console.log(`[vortyx-platform] web console: http://localhost:${port}/`);
  }
});
