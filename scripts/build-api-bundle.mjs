#!/usr/bin/env node
// Phase 17 — the Vercel build step for the single-project topology.
//
// Bundles the repository-root catch-all (api/[...route].ts) into ONE
// self-contained ESM function file (api/[...route].js) with esbuild: every
// runtime dependency — the platform/api/src modules AND
// @supabase/supabase-js — is inlined, so the deployed lambda never has to
// resolve a cross-directory module path at runtime.
//
// WHY this exists (the Phase 17 root cause, confirmed from the production
// build logs of dpl_3adCauf1GqQmQvd7hQJQ7GZDE5j7): Vercel's per-file
// function build transpiles the entry but preserves its import specifiers
// verbatim, and the literal "../platform/api/src/vercel.ts" import was not
// traced into the lambda bundle. Every /api/* request died with
// ERR_MODULE_NOT_FOUND at runtime while the deployment still showed READY.
// A self-contained bundle removes the runtime resolution step entirely;
// scripts/verify_vercel_bundle.mjs pins the behavior in CI.
//
// After bundling, the .ts entry is REMOVED from the BUILD WORKSPACE so
// Vercel's function discovery sees exactly one function (the bundle). The
// repository source is untouched: this script runs in the deployment
// container only (wired through vercel.json "buildCommand").
import { build } from "esbuild";
import { rmSync, statSync } from "node:fs";

const OUTFILE = "api/[...route].js";

await build({
  entryPoints: ["api/[...route].ts"],
  outfile: OUTFILE,
  bundle: true,
  platform: "node",
  format: "esm",
  target: "node22",
  sourcemap: false,
  legalComments: "none",
  logLevel: "warning",
  packages: "bundle",
});

const info = statSync(OUTFILE);
// Sanity bound: @supabase/supabase-js alone is far larger than this. A tiny
// output would mean the dynamic import chain was NOT inlined (external) —
// the exact silent-partial-bundle failure this build step exists to close.
if (info.size < 100_000) {
  console.error(
    `FAIL: ${OUTFILE} is suspiciously small (${info.size} bytes) — ` +
      "runtime dependencies were not bundled in",
  );
  process.exit(1);
}

rmSync("api/[...route].ts");
console.log(
  `PASS: bundled api/[...route].js (${info.size} bytes, self-contained; source .ts entry removed from the build workspace)`,
);
