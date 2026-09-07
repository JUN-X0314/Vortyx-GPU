#!/usr/bin/env node
// Phase 17 — repository secret-boundary scan (the automated gate for the
// documented policy: browser-visible config carries publishable values
// only; server-only secrets never enter the repository).
//
// Scans every GIT-TRACKED file for credential-shaped content:
//   * JWT-shaped values (the legacy Supabase anon/service_role keys are
//     JWTs — a committed one is a finding; the publishable sb_publishable_
//     format is NOT a JWT and is allowlisted by design),
//   * Supabase secret keys (sb_secret_*), access tokens (sbp_*),
//   * GitHub (ghp_/gho_/ghu_/ghs_/ghr_) and Vercel (vcp_) tokens,
//   * env assignments that carry a server-only secret VALUE,
//   * private key blocks and credential-bearing database URLs,
//   * tracked .env files (only .env.example is allowed).
//
// Findings print file:line + PATTERN NAME only — never the matched text.
// Exit codes: 0 = clean; 1 = at least one finding.
import { execFileSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const repo = join(dirname(fileURLToPath(import.meta.url)), "..");

const tracked = execFileSync("git", ["ls-files", "-z"], { cwd: repo })
  .toString()
  .split("\0")
  .filter(Boolean);

const patterns = [
  { name: "jwt-credential-shape", re: /eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}/ },
  { name: "supabase-secret-key", re: /sb_secret_[A-Za-z0-9_-]{10,}/ },
  { name: "supabase-access-token", re: /\bsbp_[A-Za-z0-9]{20,}/ },
  { name: "github-token", re: /\bgh[pousr]_[A-Za-z0-9]{20,}/ },
  { name: "vercel-token", re: /\bvcp_[A-Za-z0-9]{20,}/ },
  { name: "service-role-env-value", re: /SUPABASE_SERVICE_ROLE_KEY\s*=\s*\S/ },
  { name: "worker-token-env-value", re: /VORTYX_WORKER_TOKEN\s*=\s*[A-Za-z0-9_-]{16,}/ },
  { name: "reconcile-token-env-value", re: /VORTYX_RECONCILE_TOKEN\s*=\s*[A-Za-z0-9_-]{16,}/ },
  { name: "private-key-block", re: /-----BEGIN [A-Z ]*PRIVATE KEY-----/ },
  { name: "database-url-credentials", re: /postgres(?:ql)?:\/\/[^\s"'<>]*:[^\s"'<>@]+@[^\s"'<>]+/ },
];

const findings = [];
for (const file of tracked) {
  // A tracked .env file is itself a finding (only the .env.example template
  // is allowed) — checked before content scanning.
  if (/(^|\/)\.env(\..+)?$/.test(file) && !file.endsWith(".env.example")) {
    findings.push({ file, line: 0, pattern: "tracked-env-file" });
    continue;
  }
  let text;
  try {
    const buffer = readFileSync(join(repo, file));
    if (buffer.subarray(0, 8000).includes(0)) continue; // binary
    text = buffer.toString("utf8");
  } catch {
    continue; // deleted in the working tree, still listed by git
  }
  const lines = text.split("\n");
  for (let index = 0; index < lines.length; index += 1) {
    for (const { name, re } of patterns) {
      if (re.test(lines[index])) {
        findings.push({ file, line: index + 1, pattern: name });
      }
    }
  }
}

if (findings.length > 0) {
  console.error("[FAIL] secret-boundary scan found problems (values are NOT printed):");
  for (const { file, line, pattern } of findings) {
    console.error(`  ${file}:${line}  ${pattern}`);
  }
  process.exit(1);
}
console.log("[PASS] no credential-shaped content in tracked files");
