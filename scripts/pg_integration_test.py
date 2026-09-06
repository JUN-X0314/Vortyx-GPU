#!/usr/bin/env python3
"""Phase 15.0.1 — REAL PostgreSQL integration + concurrency verification.

Applies the Supabase migrations 0001 -> 0004 VERBATIM, in lexicographic
order, to a REAL PostgreSQL 17 server, then re-produces the exact races
Phase 15.0.1 hardens (quota, artifact capacity, rate limit, terminal
immutability, worker claim) and proves the database closes them.

Runs in CI (GitHub Actions `postgres-integration` job against a
postgres:17 service container) and locally against any reachable server.

Requirements:
  * `psql` on PATH (postgresql-client).
  * A REACHABLE PostgreSQL SERVER with a superuser role; the test database
    is dropped and recreated fresh on every run (full isolation, PART of the
    test contract — no leftover state can fake a pass).
  * Connection comes ENTIRELY from the standard libpq environment:
      PGHOST, PGPORT, PGUSER, PGPASSWORD, PGDATABASE (test database name,
      default `vortyx`), plus optional PGAPPNAME.
    No credentials are stored in this repository (CI uses the ephemeral
    service-container password, created and destroyed with the job).

Supabase compatibility stubs (the `auth` schema, `auth.uid()`, the roles
the migrations grant to) are created by this script — they stand in for
the parts of Supabase that are NOT ours to migrate.

Every assertion prints PASS/FAIL; the script exits non-zero on any FAIL.
Test-only: nothing here touches a production database.
"""

import os
import subprocess
import sys
import threading

# The database the migrations are applied to (drop/recreated every run).
TEST_DB = os.environ.get("PGDATABASE", "vortyx")
# The maintenance database used for the drop/create (every cluster has it).
MAINT_DB = os.environ.get("VORTYX_PG_MAINT_DB", "postgres")
# Repository-relative migration directory (this script lives in scripts/).
MIGRATIONS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "platform", "supabase", "migrations")

failures = []


def check(name: str, ok: bool, detail: str = ""):
    print(f"{'PASS' if ok else 'FAIL'}: {name}" + (f"  [{detail}]" if detail else ""))
    if not ok:
        failures.append(name)


def psql(sql: str, db: str = None, tuples: bool = False) -> str:
    """Runs psql against the target server (connection via PG* env)."""
    cmd = ["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1",
           "-d", db if db is not None else TEST_DB]
    if tuples:
        cmd += ["-A", "-t"]
    env = dict(os.environ)
    env.setdefault("PGCONNECT_TIMEOUT", "10")
    result = subprocess.run(cmd, input=sql, capture_output=True, text=True, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"psql failed ({result.returncode}): {result.stderr.strip()[:500]}")
    return result.stdout.strip()


def psql_expect_error(sql: str) -> str:
    """Runs psql expecting a SQL error; returns the error text ('' if none)."""
    env = dict(os.environ)
    env.setdefault("PGCONNECT_TIMEOUT", "10")
    result = subprocess.run(
        ["psql", "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1",
         "-d", TEST_DB, "-c", sql],
        capture_output=True, text=True, env=env)
    if result.returncode == 0:
        return ""
    return (result.stderr or result.stdout).strip()


# ---------------------------------------------------------------------------
# 0. Server identity (the log line IS the PostgreSQL-version evidence)
# ---------------------------------------------------------------------------
version = psql("select version()", db=MAINT_DB, tuples=True)
print("== PostgreSQL integration test ==")
print(f"   server: {version}")
if "PostgreSQL 17" not in version:
    print("FAIL: this suite is pinned to PostgreSQL 17 (the documented target)")
    sys.exit(1)
print()

# ---------------------------------------------------------------------------
# 1. Fresh test database (isolation: no leftover state can fake a pass)
# ---------------------------------------------------------------------------
psql(f"DROP DATABASE IF EXISTS {TEST_DB} WITH (FORCE)", db=MAINT_DB)
psql(f"CREATE DATABASE {TEST_DB}", db=MAINT_DB)

# Supabase-compatible stubs: the roles the migrations grant to, the auth
# schema (auth.users + auth.uid()) — the parts of Supabase that are not
# ours to migrate. Idempotent by construction.
stub = """
DO $$ BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'anon') THEN
    CREATE ROLE anon NOLOGIN;
  END IF;
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'authenticated') THEN
    CREATE ROLE authenticated NOLOGIN;
  END IF;
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'service_role') THEN
    CREATE ROLE service_role NOLOGIN;
  END IF;
END $$;
CREATE SCHEMA IF NOT EXISTS auth;
CREATE TABLE IF NOT EXISTS auth.users (
  id uuid PRIMARY KEY,
  email text,
  created_at timestamptz NOT NULL DEFAULT now()
);
CREATE OR REPLACE FUNCTION auth.uid() RETURNS uuid
LANGUAGE sql STABLE AS $$ SELECT NULLIF(current_setting('request.jwt.claim.sub', true), '')::uuid $$;
"""
psql(stub)
print("== fresh database + Supabase-compatible stubs ready ==")

# ---------------------------------------------------------------------------
# 2. Apply the migrations VERBATIM, in lexicographic order (the repo's
#    convention; 0001 -> 0002 -> 0003 -> 0004 dependency chain proven by
#    the fact that each file only works after the previous ones)
# ---------------------------------------------------------------------------
names = sorted(
    f for f in os.listdir(MIGRATIONS) if f.endswith(".sql"))
print(f"== applying migrations in lexicographic order: {', '.join(names)} ==")
for name in names:
    sql = open(os.path.join(MIGRATIONS, name)).read()
    try:
        psql(sql)
        check(f"migration {name} applies cleanly", True)
    except RuntimeError as e:
        check(f"migration {name} applies cleanly", False, str(e)[:200])
if failures:
    sys.exit(1)
print()

# ---------------------------------------------------------------------------
# 3. Fixtures: users + projects (deterministic ids; one run owns the DB)
# ---------------------------------------------------------------------------
uuids = {}
for u in ["alice", "bob"]:
    psql(f"INSERT INTO auth.users (id, email) VALUES (gen_random_uuid(), '{u}@test.local') "
         f"ON CONFLICT (id) DO NOTHING")
    uuids[u] = psql(f"SELECT id FROM auth.users WHERE email = '{u}@test.local'", tuples=True)

for pid, pname in [("proj-a", "alpha"), ("proj-b", "beta"), ("proj-c", "gamma")]:
    psql(f"INSERT INTO public.projects (id, owner_user_id, name)"
         f" VALUES ('{pid}', '{uuids['alice']}', '{pname}') ON CONFLICT (id) DO NOTHING")
psql("INSERT INTO public.quota_policies (project_id, max_concurrent_jobs, max_running_shards, max_memory_bytes)"
     " VALUES ('proj-a', 1, 4, 1000000) ON CONFLICT (project_id) DO UPDATE SET max_concurrent_jobs = 1")
print("== fixtures ready ==")
print()

# Shared plumbing for the concurrency sections: every thread lines up on a
# threading.Barrier BEFORE touching the database, so the statements start
# as simultaneously as the runner allows (deterministic START, not timing).
results = {}
lock = threading.Lock()
TS = 1700000000000

JOB_TMPL = ("INSERT INTO public.service_jobs (job_id, project_id, submitted_by, operation,"
            " element_count, requested_backend, requested_shard_count, submitted_at_ms)"
            " VALUES ('{jid}', '{proj}', '{user}', 'vector_add', 100, '', {shards}, {ts})")


def psql_outcome(stmt: str) -> str:
    try:
        psql(stmt)
        return "ok"
    except RuntimeError as e:
        msg = str(e)
        if "quota_exceeded" in msg:
            return "quota"
        if "artifact_capacity" in msg:
            return "capacity"
        return f"other:{msg[:90]}"


def race_insert(jid: str, proj: str, shards: int, barrier: threading.Barrier):
    stmt = JOB_TMPL.format(jid=jid, proj=proj, user=uuids['alice'], shards=shards, ts=TS)
    barrier.wait()
    out = psql_outcome(stmt)
    with lock:
        results[jid] = out


def run_race(threads: list):
    for t in threads: t.start()
    for t in threads: t.join()


# ---------------------------------------------------------------------------
# 4. THE QUOTA RACE: limit 1, two SIMULTANEOUS inserts. Without the 0004
#    project-row lock both read usage = 0 and both insert. With it,
#    exactly one wins and the loser leaves NO row behind.
# ---------------------------------------------------------------------------
barrier = threading.Barrier(2)
run_race([threading.Thread(target=race_insert, args=("job-race-1", "proj-a", 1, barrier)),
          threading.Thread(target=race_insert, args=("job-race-2", "proj-a", 1, barrier))])
check("concurrent quota race (limit 1): exactly one submission wins",
      sorted(results.values()) == ["ok", "quota"], str(results))
count = psql("SELECT count(*) FROM public.service_jobs WHERE project_id = 'proj-a'", tuples=True)
check("concurrent quota race: the failed request leaves no job row (count exactly 1)",
      count == "1", f"count={count}")

# ---------------------------------------------------------------------------
# 5. Shard quota under concurrency: 4 connections race 2 shards each
#    against a 4-shard budget -> exactly 2 land, usage stays <= policy.
# ---------------------------------------------------------------------------
psql("INSERT INTO public.quota_policies (project_id, max_concurrent_jobs, max_running_shards, max_memory_bytes)"
     " VALUES ('proj-b', 10, 4, 100000000) ON CONFLICT (project_id) DO UPDATE SET max_running_shards = 4")
results.clear()
barrier = threading.Barrier(4)
run_race([threading.Thread(target=race_insert, args=(f"job-shard-{i}", "proj-b", 2, barrier))
          for i in range(4)])
oks = sum(1 for v in results.values() if v == "ok")
check("concurrent shard race (budget 4, four 2-shard submissions): exactly 2 win",
      oks == 2, str(results))
shards = psql("SELECT coalesce(sum(requested_shard_count),0) FROM public.service_jobs WHERE project_id = 'proj-b'",
              tuples=True)
check("concurrent shard race: final shard usage <= policy", int(shards) <= 4, f"shards={shards}")

# ---------------------------------------------------------------------------
# 6. Memory quota under concurrency: element_count 100 vector_add = 1200
#    bytes each; budget 3000 -> exactly 2 land.
# ---------------------------------------------------------------------------
psql("INSERT INTO public.quota_policies (project_id, max_concurrent_jobs, max_running_shards, max_memory_bytes)"
     " VALUES ('proj-b', 10, 64, 3000) ON CONFLICT (project_id) DO UPDATE SET max_memory_bytes = 3000, max_running_shards = 64")
psql("DELETE FROM public.service_jobs WHERE project_id = 'proj-b'")
results.clear()
barrier = threading.Barrier(4)
run_race([threading.Thread(target=race_insert, args=(f"job-mem-{i}", "proj-b", 1, barrier))
          for i in range(4)])
oks = sum(1 for v in results.values() if v == "ok")
check("concurrent memory race (budget 3000, four 1200-byte submissions): exactly 2 win",
      oks == 2, str(results))

# ---------------------------------------------------------------------------
# 7. THE ARTIFACT RACE: 255 existing rows, two simultaneous inserts.
#    Without the 0004 lock both count 255 and both insert (257).
#    With it: 1 success, 1 refusal, final count exactly 256.
# ---------------------------------------------------------------------------
psql("INSERT INTO public.artifact_metadata (artifact_id, project_id, name, created_by, declared_byte_size, created_at_ms)"
     f" SELECT 'art-seed-' || g, 'proj-c', 'seed ' || g, '{uuids['alice']}', 1, 0 FROM generate_series(1,255) g"
     " ON CONFLICT (artifact_id) DO NOTHING")
psql("DELETE FROM public.service_jobs WHERE project_id = 'proj-c'")
art_results = {}
art_barrier = threading.Barrier(2)


def insert_artifact(aid: str):
    stmt = ("INSERT INTO public.artifact_metadata (artifact_id, project_id, name, created_by, declared_byte_size, created_at_ms)"
            f" VALUES ('{aid}', 'proj-c', '{aid}', '{uuids['alice']}', 1, 0)")
    art_barrier.wait()
    out = psql_outcome(stmt)
    with lock:
        art_results[aid] = out


ta = threading.Thread(target=insert_artifact, args=("art-race-a",))
tb = threading.Thread(target=insert_artifact, args=("art-race-b",))
ta.start(); tb.start(); ta.join(); tb.join()
check("concurrent artifact race (255 + 2 simultaneous): exactly one lands",
      sorted(art_results.values()) == ["capacity", "ok"], str(art_results))
art_count = psql("SELECT count(*) FROM public.artifact_metadata WHERE project_id = 'proj-c'", tuples=True)
check("concurrent artifact race: final count is exactly 256", art_count == "256", f"count={art_count}")
err = psql_expect_error("INSERT INTO public.artifact_metadata (artifact_id, project_id, name, created_by, declared_byte_size, created_at_ms)"
                        f" VALUES ('art-over', 'proj-c', 'over', '{uuids['alice']}', 1, 0)")
check("full project (256) refuses further artifacts", "artifact_capacity" in err, err[:80])

# ---------------------------------------------------------------------------
# 8. Idempotency BACKSTOP at the storage layer: service_jobs.job_id is the
#    primary key, so the database itself rejects a second row for the same
#    job id — same payload (replay) and different payload (conflict) both
#    surface as unique violations the adapter maps honestly. The API's
#    replay/conflict logic RIDES on this; here we pin the backstop itself.
#    proj-b (quota 10) is used ON PURPOSE: the quota trigger is a BEFORE
#    INSERT trigger and fires BEFORE the unique check, so the duplicate
#    must not first trip a full quota — this asserts the PK backstop, not
#    the quota.
# ---------------------------------------------------------------------------
psql("DELETE FROM public.service_jobs WHERE project_id = 'proj-b'")
psql(JOB_TMPL.format(jid="job-idem", proj="proj-b", user=uuids['alice'], shards=1, ts=TS))
err = psql_expect_error(JOB_TMPL.format(jid="job-idem", proj="proj-b", user=uuids['alice'], shards=1, ts=TS))
check("duplicate job_id, identical payload: the primary key rejects a second row",
      "duplicate key" in err.lower(), err[:80])
err = psql_expect_error(JOB_TMPL.format(jid="job-idem", proj="proj-b", user=uuids['alice'], shards=2, ts=TS))
check("duplicate job_id, different payload: the primary key rejects a second row",
      "duplicate key" in err.lower(), err[:80])

# ---------------------------------------------------------------------------
# 9. Cross-project independence: proj-a's serialization must not block
#    proj-c — concurrent inserts on DIFFERENT projects both succeed.
# ---------------------------------------------------------------------------
psql("TRUNCATE public.service_jobs")
results.clear()
barrier = threading.Barrier(2)
run_race([threading.Thread(target=race_insert, args=("job-par-a", "proj-a", 1, barrier)),
          threading.Thread(target=race_insert, args=("job-par-b", "proj-c", 1, barrier))])
check("different projects stay independent under concurrent inserts",
      results.get("job-par-a") == "ok" and results.get("job-par-b") == "ok",
      str(results))

# ---------------------------------------------------------------------------
# 10. rate_limit_take: 20 concurrent takes on ONE key with max 10 ->
#     exactly 10 true; the ON CONFLICT row lock keeps the count exact
#     and the stored counter counts every take (refused ones included).
# ---------------------------------------------------------------------------
psql("TRUNCATE public.rate_limit_windows")
take_results = []
take_barrier = threading.Barrier(20)


def take():
    take_barrier.wait()
    out = psql("SELECT public.vortyx_rate_limit_take('submit:test-user', 60000, 10)", tuples=True)
    with lock:
        take_results.append(out.strip())


threads = [threading.Thread(target=take) for _ in range(20)]
run_race(threads)
trues = sum(1 for v in take_results if v == "t")
check("rate_limit_take: exactly 10 of 20 concurrent takes admitted (max 10)",
      trues == 10, f"true={trues}")
stored = psql("SELECT attempts FROM public.rate_limit_windows WHERE key = 'submit:test-user'", tuples=True)
check("rate_limit_take: the stored counter counts every take (20)", stored == "20", f"attempts={stored}")

# Window boundary semantics (deterministic parameters, no clock waiting):
# a FRESH key with max 1 admits the first take and refuses the second.
out = psql("SELECT public.vortyx_rate_limit_take('submit:fresh-user', 60000, 1)", tuples=True)
out2 = psql("SELECT public.vortyx_rate_limit_take('submit:fresh-user', 60000, 1)", tuples=True)
check("rate_limit_take: max 1 admits the first take of a key, refuses the second",
      out == "t" and out2 == "f", f"{out},{out2}")

# ---------------------------------------------------------------------------
# 11. Terminal immutability: a terminal row refuses ANY rewrite (even by
#     the table owner — the same gate an RLS client hits; the trigger is
#     the "ANY writer freezes terminal state" guarantee, not an app rule).
# ---------------------------------------------------------------------------
psql("UPDATE public.service_jobs SET status = 'completed', terminal_at_ms = 1, error = ''"
     " WHERE job_id = 'job-par-b'")
err = psql_expect_error("UPDATE public.service_jobs SET status = 'queued', terminal_at_ms = NULL"
                        " WHERE job_id = 'job-par-b'")
check("terminal job cannot be resurrected (terminal_immutable)", "terminal_immutable" in err, err[:90])
err = psql_expect_error("UPDATE public.service_jobs SET result_element_count = 999999"
                        " WHERE job_id = 'job-par-b'")
check("terminal job result cannot be rewritten", "terminal_immutable" in err, err[:90])

# Non-terminal paths still work (the documented transitions are untouched).
psql("UPDATE public.service_jobs SET cancel_requested = true WHERE job_id = 'job-par-a' AND status = 'queued'")
psql("UPDATE public.service_jobs SET status = 'cancelled', error = 'cancelled', terminal_at_ms = 2"
     " WHERE job_id = 'job-par-a' AND status = 'queued'")
status_row = psql("SELECT status FROM public.service_jobs WHERE job_id = 'job-par-a'", tuples=True)
check("non-terminal transitions unaffected (queued -> cancelled)", status_row == "cancelled", status_row)

# ---------------------------------------------------------------------------
# 12. Worker claim race (FOR UPDATE SKIP LOCKED): 3 concurrent claims over
#     2 queued jobs -> 2 claims land, one empty; no job claimed twice.
# ---------------------------------------------------------------------------
psql("TRUNCATE public.service_jobs")
psql(JOB_TMPL.format(jid="job-w1", proj="proj-c", user=uuids['alice'], shards=1, ts=TS))
psql(JOB_TMPL.format(jid="job-w2", proj="proj-c", user=uuids['alice'], shards=1, ts=TS))
claim_results = []
claim_barrier = threading.Barrier(3)


def claim(w: str):
    claim_barrier.wait()
    out = psql(f"SELECT public.vortyx_worker_claim('{w}', 60000) ->> 'claimed'", tuples=True)
    with lock:
        claim_results.append(out.strip())


threads = [threading.Thread(target=claim, args=(f"worker-{i}",)) for i in range(3)]
run_race(threads)
claimed = sum(1 for v in claim_results if v == "true")
running = psql("SELECT count(*) FROM public.service_jobs WHERE status = 'running'", tuples=True)
distinct = psql("SELECT count(DISTINCT claimed_by) FROM public.service_jobs WHERE status = 'running'", tuples=True)
check("worker claim race: exactly 2 of 3 claims get a job", claimed == 2, str(claim_results))
check("worker claim race: no double claim (2 running, distinct claimants)",
      running == "2" and distinct == "2", f"running={running} distinct={distinct}")

# A wrong worker cannot complete a job it does not hold.
holder = psql("SELECT claimed_by FROM public.service_jobs WHERE job_id = 'job-w1'", tuples=True)
non_holder = "worker-0" if holder != "worker-0" else "worker-1"
err = psql_expect_error(f"SELECT public.vortyx_worker_complete('{non_holder}', 'job-w1', 'completed', '', 'cpu', 100, 1, 1, 0)")
check("worker_complete refuses a non-holder", "not_claimed_by_this_worker" in err, err[:90])

# The holder completes; a duplicate report replays without rewriting.
out = psql(f"SELECT public.vortyx_worker_complete('{holder}', 'job-w1', 'completed', '', 'cpu', 100, 1, 1, 0) ->> 'recorded'",
           tuples=True)
check("worker_complete records the holder's report", out == "true", out)
out = psql(f"SELECT v ->> 'recorded', v ->> 'status' FROM (SELECT public.vortyx_worker_complete('{holder}', 'job-w1', 'failed', 'boom', '', NULL, NULL, NULL, NULL) AS v) x",
           tuples=True)
check("duplicate completion replays idempotently (recorded=false)", out == "false|completed", out)

# ---------------------------------------------------------------------------
# 13. project_missing: an insert for a nonexistent project is refused
#     honestly by the trigger (mappable to the standard not_found).
# ---------------------------------------------------------------------------
err = psql_expect_error("INSERT INTO public.service_jobs (job_id, project_id, submitted_by, operation, element_count, requested_backend, requested_shard_count, submitted_at_ms)"
                        f" VALUES ('job-ghost', 'proj-ghost', '{uuids['alice']}', 'vector_add', 100, '', 1, {TS})")
check("nonexistent project insert refused", ("project_missing" in err) or ("foreign key" in err.lower()), err[:90])

# ---------------------------------------------------------------------------
# 14. Single-owner invariant at the database level: the DB refuses a
#     second owner through ANY write path (this is what the API's guard
#     also relies on — defense in depth, tested at both layers).
# ---------------------------------------------------------------------------
err = psql_expect_error(f"INSERT INTO public.project_members (project_id, user_id, role)"
                        f" VALUES ('proj-a', '{uuids['bob']}', 'owner')")
check("single-owner invariant: owner role not grantable", "owner_not_grantable" in err, err[:90])

# ---------------------------------------------------------------------------
# 15. Migration idempotency: 0004 re-applies cleanly (its documented
#     CREATE OR REPLACE / IF NOT EXISTS structure, tested for real).
# ---------------------------------------------------------------------------
sql4 = open(os.path.join(MIGRATIONS, "0004_service_hardening.sql")).read()
try:
    psql(sql4)
    check("migration 0004 is idempotent (re-applies cleanly)", True)
except RuntimeError as e:
    check("migration 0004 is idempotent (re-applies cleanly)", False, str(e)[:120])

print()
if failures:
    print(f"{len(failures)} CHECK(S) FAILED: {failures}")
    sys.exit(1)
print("ALL POSTGRESQL INTEGRATION CHECKS PASSED")
