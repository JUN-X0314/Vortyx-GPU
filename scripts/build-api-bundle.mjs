#!/usr/bin/env node
// Phase 17 — the Vercel build step for the single-project topology.
//
// Bundles the repository-root catch-all (api/[...route].ts) into ONE
// self-contained ESM function file with esbuild: every runtime dependency —
// the platform/api/src modules AND @supabase/supabase-js — is inlined, so
// the deployed lambda never has to resolve a cross-directory module path
// at runtime.
//
// WHY this exists (the Phase 17 root cause, confirmed from the production
// build logs of dpl_3adCauf1GqQmQvd7hQJQ7GZDE5j7): Vercel's per-file
// function build transpiles the entry but preserves its import specifiers
// verbatim, and the literal "../platform/api/src/vercel.ts" import was not
// traced into the lambda bundle. Every /api/* request died with
// ERR_MODULE_NOT_FOUND at runtime while the deployment still showed READY.
//
// HOW the artifact ships: the bundle OVERWRITES the entry file in place
// (api/[...route].ts — plain JavaScript is valid TypeScript, so the
// function builder processes the self-contained bundle as the entry).
// Deleting the .ts does NOT work: Vercel's function builder opens the
// entry path it discovered before the build step ran (observed live:
// ENOENT on dpl_ADNP55LDD23BDkuioThAeSyGS3iE). Overwriting keeps exactly
// one function, one route, zero runtime resolution. The repository source
// is untouched: this runs in the deployment container only (wired through
// vercel.json "buildCommand").
import { build } from "esbuild";
import { readFileSync, statSync } from "node:fs";

const OUTFILE = "api/[...route].ts";

await build({
  entryPoints: [OUTFILE],
  outfile: OUTFILE,
  allowOverwrite: true,
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

// The shipped entry must carry NO runtime .ts import specifier (the residue
// that produced ERR_MODULE_NOT_FOUND in production). Checked on the artifact.
const bundled = readFileSync(OUTFILE, "utf8");
if (/(?:from\s*|import\()\s*["'][^"']*\.ts["']/.test(bundled)) {
  console.error("FAIL: the bundle still contains a runtime .ts import specifier");
  process.exit(1);
}

console.log(
  `PASS: bundled api/[...route].ts in place (${info.size} bytes, self-contained)`,
);
