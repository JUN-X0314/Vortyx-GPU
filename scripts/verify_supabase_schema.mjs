#!/usr/bin/env node
// Phase 17 — Supabase schema verification + migration drift detection.
//
// Compares the LIVE database against the repository's migration inventory
// (platform/supabase/migrations/*.sql) and reports exactly three outcomes:
//
//   PASS    — every expected object present, no unexplained extra objects
//             in the checked namespaces, migration history complete
//   FAIL    — an expected object is missing, RLS is off, or a privilege
//             boundary is wrong (a production blocker)
//   DRIFT   — the live database contains schema objects or migration
//             history entries the repository does NOT explain (a two-way
//             check: missing files AND surprise objects both drift)
//
// DRIFT and FAIL are both non-zero (FAIL=1, DRIFT=2): production must not
// be called verified in either state.
//
// Env (operator-scoped; the Supabase access token is NEVER printed):
//   SUPABASE_ACCESS_TOKEN   a Supabase personal access token (sbp_...)
//   SUPABASE_PROJECT_REF    the project ref
//
// Read-only: every query is a SELECT against catalogs / the migration
// ledger. Nothing here mutates the database.
const token = process.env.SUPABASE_ACCESS_TOKEN ?? "";
const ref = process.env.SUPABASE_PROJECT_REF ?? "";
if (token.length === 0 || ref.length === 0) {
  console.error("usage: SUPABASE_ACCESS_TOKEN=... SUPABASE_PROJECT_REF=... node scripts/verify_supabase_schema.mjs");
  process.exit(1);
}

let failures = 0;
let drifts = 0;
function pass(name, detail = "") {
  console.log(`PASS: ${name}${detail ? ` — ${detail}` : ""}`);
}
function fail(name, detail = "") {
  console.log(`FAIL: ${name}${detail ? ` — ${detail}` : ""}`);
  failures += 1;
}
function drift(name, detail = "") {
  console.log(`DRIFT: ${name}${detail ? ` — ${detail}` : ""}`);
  drifts += 1;
}

async function query(sql) {
  const response = await fetch(`https://api.supabase.com/v1/projects/${ref}/database/query`, {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
    body: JSON.stringify({ query: sql }),
  });
  const text = await response.text();
  let parsed;
  try {
    parsed = JSON.parse(text);
  } catch {
    parsed = { raw: text.slice(0, 300) };
  }
  if (response.status !== 200 && response.status !== 201) {
    throw new Error(`query failed (HTTP ${response.status}): ${JSON.stringify(parsed).slice(0, 400)}`);
  }
  return parsed;
}

// --- the expected inventory (the repository's migrations) -------------------

const EXPECTED_TABLES = [
  "profiles", "devices", "jobs", "job_results", // 0001
  "distributed_jobs", "distributed_shards", "device_views", // 0002
  "projects", "project_members", "service_jobs", "quota_policies",
  "artifact_metadata", "audit_events", "rate_limit_windows", // 0003
];

const EXPECTED_MIGRATION_VERSIONS = [
  "0001", "0002", "0003", "0004", "0005",
];

const EXPECTED_WORKER_RPCS = [
  ["vortyx_worker_claim", "text, bigint"],
  ["vortyx_worker_heartbeat", "text, text, bigint"],
  ["vortyx_worker_complete", "text, text, text, text, text, bigint, integer, integer, integer"],
  ["vortyx_worker_reconcile", ""],
];

const EXPECTED_RATE_LIMIT_RPC = "vortyx_rate_limit_take";

const EXPECTED_TRIGGERS = [
  ["on_auth_user_created", "auth", "users"],
  ["on_auth_user_created_audit", "auth", "users"],
  ["on_project_created", "public", "projects"],
  ["project_members_single_owner_insert", "public", "project_members"],
  ["project_members_single_owner_update", "public", "project_members"],
  ["service_jobs_quota_check", "public", "service_jobs"],
  ["service_jobs_terminal_immutable", "public", "service_jobs"],
  ["artifact_metadata_capacity_check", "public", "artifact_metadata"],
];

// SECURITY DEFINER functions that are NOT repository objects but are known
// platform-side helpers, each with its recorded justification (0005 locks
// their EXECUTE away from anon/authenticated; they run as event/trigger
// plumbing owned by the platform, not callable by API clients).
const KNOWN_PLATFORM_DEFINERS = new Map([
  ["rls_auto_enable", "platform-owned RLS helper; EXECUTE revoked from anon/authenticated by 0005 (verified live)"],
]);

// --- run the checks ----------------------------------------------------------

try {
  // 1. Migration history (the ledger the deployment maintains).
  const ledger = await query(
    "select version, name from supabase_migrations.schema_migrations order by version",
  );
  const applied = new Set(ledger.map((row) => String(row.version).slice(0, 4)));
  for (const version of EXPECTED_MIGRATION_VERSIONS) {
    if (applied.has(version)) {
      pass(`migration ${version} applied`);
    } else {
      fail(`migration ${version} applied`, "missing from supabase_migrations.schema_migrations");
    }
  }
  for (const row of ledger) {
    if (!EXPECTED_MIGRATION_VERSIONS.includes(String(row.version).slice(0, 4))) {
      drift(`migration history entry ${row.version} (${row.name})`, "not explained by the repository's migrations");
    }
  }

  // 2. Tables: exactly the expected set.
  const tables = await query(
    "select table_name from information_schema.tables where table_schema = 'public' and table_type = 'BASE TABLE' order by table_name",
  );
  const liveTables = tables.map((row) => row.table_name);
  for (const name of EXPECTED_TABLES) {
    if (liveTables.includes(name)) pass(`table public.${name} exists`);
    else fail(`table public.${name} exists`);
  }
  for (const name of liveTables) {
    if (!EXPECTED_TABLES.includes(name)) drift(`extra table public.${name}`, "not created by any repository migration");
  }

  // 3. RLS enabled on every application table.
  const rls = await query(
    "select c.relname, c.relrowsecurity from pg_class c join pg_namespace n on n.oid = c.relnamespace where n.nspname = 'public' and c.relkind = 'r'",
  );
  for (const row of rls) {
    if (row.relrowsecurity) pass(`RLS enabled on ${row.relname}`);
    else fail(`RLS enabled on ${row.relname}`, "row level security is OFF");
  }

  // 4. Policies: the database-level authorization backstop exists.
  const policies = await query("select tablename, policyname from pg_policies where schemaname = 'public'");
  const policyNames = new Set(policies.map((row) => `${row.tablename}.${row.policyname}`));
  const expectedPolicies = [
    "profiles.profiles_select_own", "devices.devices_select_own", "jobs.jobs_select_own",
    "projects.projects_select_member", "project_members.project_members_select_member",
    "service_jobs.service_jobs_select_member", "audit_events.audit_events_insert_self",
  ];
  for (const name of expectedPolicies) {
    if (policyNames.has(name)) pass(`policy ${name} present`);
    else fail(`policy ${name} present`);
  }
  if (policyNames.has("audit_events.audit_events_insert_any_authenticated")) {
    fail("audit insert policy binds the actor", "the unconstrained 0003 policy is still installed (0005 not applied)");
  }

  // 5. Worker-protocol RPC privilege boundary (service_role ONLY).
  for (const [fn, signature] of EXPECTED_WORKER_RPCS) {
    const rows = await query(
      `select has_function_privilege('anon', 'public.${fn}(${signature})'::regprocedure, 'EXECUTE') as anon_can, ` +
        `has_function_privilege('authenticated', 'public.${fn}(${signature})'::regprocedure, 'EXECUTE') as auth_can, ` +
        `has_function_privilege('service_role', 'public.${fn}(${signature})'::regprocedure, 'EXECUTE') as service_can`,
    );
    const row = rows[0] ?? {};
    if (row.anon_can || row.auth_can) {
      fail(`worker RPC ${fn} locked to service_role`, "anon/authenticated still hold EXECUTE");
    } else {
      pass(`worker RPC ${fn} locked to service_role`);
    }
    if (!row.service_can) {
      fail(`worker RPC ${fn} callable by service_role`, "the API's worker path would break");
    }
  }

  // 6. The centralized rate limiter stays reachable by authenticated users.
  {
    const rows = await query(
      `select has_function_privilege('authenticated', 'public.${EXPECTED_RATE_LIMIT_RPC}(text, bigint, bigint)'::regprocedure, 'EXECUTE') as auth_can, ` +
        `has_function_privilege('anon', 'public.${EXPECTED_RATE_LIMIT_RPC}(text, bigint, bigint)'::regprocedure, 'EXECUTE') as anon_can`,
    );
    const row = rows[0] ?? {};
    if (row.auth_can && !row.anon_can) pass(`rate-limit RPC ${EXPECTED_RATE_LIMIT_RPC}: authenticated only`);
    else fail(`rate-limit RPC ${EXPECTED_RATE_LIMIT_RPC}: authenticated only`, JSON.stringify(row));
  }

  // 7. The integrity triggers exist.
  for (const [triggerName, schemaName, tableName] of EXPECTED_TRIGGERS) {
    const rows = await query(
      "select t.tgname from pg_trigger t join pg_class c on c.oid = t.tgrelid join pg_namespace n on n.oid = c.relnamespace " +
        "where not t.tgisinternal and t.tgname = '" + triggerName + "' and c.relname = '" + tableName + "' and n.nspname = '" + schemaName + "'",
    );
    if (rows.length > 0) pass(`trigger ${triggerName} on ${schemaName}.${tableName}`);
    else fail(`trigger ${triggerName} on ${schemaName}.${tableName}`);
  }

  // 8. SECURITY DEFINER inventory: every definer function is known, and the
  //    platform-side helper (when present) is not callable by API roles.
  {
    const definers = await query(
      "select p.proname, pg_get_userbyid(p.proowner) as owner " +
        "from pg_proc p join pg_namespace n on n.oid = p.pronamespace " +
        "where n.nspname = 'public' and p.prosecdef order by p.proname",
    );
    const knownDefiners = new Set([
      "vortyx_handle_new_user", "vortyx_enforce_single_owner", "vortyx_handle_new_project",
      "vortyx_enforce_service_quota", "vortyx_enforce_artifact_capacity", "vortyx_audit_new_user",
      "vortyx_worker_reconcile", "vortyx_worker_claim", "vortyx_worker_heartbeat",
      "vortyx_worker_complete", "vortyx_rate_limit_take", "vortyx_enforce_terminal_immutable",
    ]);
    for (const row of definers) {
      if (KNOWN_PLATFORM_DEFINERS.has(row.proname)) {
        pass(`SECURITY DEFINER public.${row.proname} — EXPLICITLY JUSTIFIED`, KNOWN_PLATFORM_DEFINERS.get(row.proname));
      } else if (!knownDefiners.has(row.proname)) {
        drift(`SECURITY DEFINER function public.${row.proname}`, `owner ${row.owner} — not created by any repository migration`);
      }
    }
    pass(`SECURITY DEFINER inventory: ${definers.length} function(s), unknown ones reported as drift`);
    const auto = await query(
      "select exists (select 1 from pg_proc p join pg_namespace n on n.oid = p.pronamespace where n.nspname = 'public' and p.proname = 'rls_auto_enable') as present",
    );
    if (auto[0]?.present) {
      const grants = await query(
        "select has_function_privilege('anon', 'public.rls_auto_enable()'::regprocedure, 'EXECUTE') as anon_can, " +
          "has_function_privilege('authenticated', 'public.rls_auto_enable()'::regprocedure, 'EXECUTE') as auth_can",
      );
      if (grants[0]?.anon_can || grants[0]?.auth_can) {
        fail("platform helper rls_auto_enable not callable by API roles", "anon/authenticated still hold EXECUTE");
      } else {
        pass("platform helper rls_auto_enable locked away from API roles (resolved)");
      }
    } else {
      pass("platform helper rls_auto_enable absent (nothing to resolve)");
    }
  }
} catch (error) {
  fail("verification queries", error instanceof Error ? error.message : String(error));
}

if (failures > 0) {
  console.log(`[FAIL] schema verification: ${failures} failure(s), ${drifts} drift(s)`);
  process.exit(1);
}
if (drifts > 0) {
  console.log(`[DRIFT] schema verification: ${drifts} unexplained live object(s)/history entrie(s)`);
  process.exit(2);
}
console.log("[PASS] schema verification: live database matches the repository inventory");
