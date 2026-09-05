// Local development server for the Vortyx platform API (Phase 11).
//
// Mimics the minimal Vercel Node.js function shape (method/url/headers/body
// -> status/json) so the SAME api/ handlers run locally with zero cloud
// resources. The store is the LOCAL/MOCK InMemoryPlatformStore unless
// VORTYX_STORE=supabase is set (which requires a real Supabase project —
// deliberately deferred until after Phase 11).
//
//   npm run dev            # http://localhost:3000/api/health
//   PORT=8080 npm run dev  # custom port
//
// Memory-mode state lives in THIS process: restart = empty control plane.
// That is the documented local/mock behavior, not a bug.

import http from "node:http";
import { readConfig } from "./src/config.ts";
import { resolvePlatform } from "./src/config.ts";
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

function localTokenVerifierHint() {
  // Memory mode accepts the documented local scheme: Authorization: Bearer local:<user_id>
  console.warn('[vortyx-platform] Local auth scheme: "Authorization: Bearer local:<user_id>" (mock only).');
}
localTokenVerifierHint();

const port = Number(process.env["PORT"] ?? 3000);

const server = http.createServer(async (req, res) => {
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

  const authorization = req.headers["authorization"];
  const response = await handlePlatformRequest(
    {
      method: req.method ?? "GET",
      path: new URL(req.url ?? "/", "http://localhost").pathname,
      body,
      authorization: Array.isArray(authorization) ? authorization[0] : authorization,
    },
    {
      store: resolved.store,
      verifier: resolved.verifier,
      storeKind: resolved.storeKind,
      softwareVersion: "0.11.0",
    },
  );

  res.writeHead(response.status, { "content-type": "application/json" });
  res.end(JSON.stringify(response.body));
});

server.listen(port, () => {
  console.log(`[vortyx-platform] listening on http://localhost:${port} (store: ${resolved.storeKind})`);
});
