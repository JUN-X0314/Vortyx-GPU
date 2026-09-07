#!/usr/bin/env python3
"""Phase 17 — apply the repository's Supabase migrations to a REAL project.

Applies platform/supabase/migrations/*.sql VERBATIM in lexicographic order
through the Supabase Management API SQL endpoint, records each applied file
in the standard `supabase_migrations.schema_migrations` ledger (the same
shape `supabase db push` maintains, so the CLI and the drift detector stay
compatible) and prints a post-apply inventory.

IDEMPOTENT: versions already present in the ledger are skipped (the ledger
is the source of truth for what is applied); the migration FILES are never
modified to make a re-run pass.

Operator tooling: requires the Supabase access token; no application data is
touched beyond the migration DDL itself. Credentials come from the
environment and are NEVER printed:
  SUPABASE_ACCESS_TOKEN   a Supabase personal access token (sbp_...)
  SUPABASE_PROJECT_REF    the target project ref
"""
import json
import os
import sys
import urllib.request

MIGRATIONS = os.environ.get(
    "VORTYX_MIGRATIONS_DIR",
    os.path.join("/home/z/my-project/Vortyx-GPU", "platform", "supabase", "migrations"),
)
API = "https://api.supabase.com/v1"

token = os.environ.get("SUPABASE_ACCESS_TOKEN", "")
ref = os.environ.get("SUPABASE_PROJECT_REF", "")
if not token or not ref:
    print("usage: SUPABASE_ACCESS_TOKEN=... SUPABASE_PROJECT_REF=... apply_migrations.py")
    sys.exit(1)


def api_call(path: str, sql: str):
    body = json.dumps({"query": sql}).encode("utf-8")
    request = urllib.request.Request(
        f"{API}/projects/{ref}/database/query",
        data=body,
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json",
                 "User-Agent": "curl/8.5.0", "Accept": "*/*"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=180) as response:
        return response.status, response.read().decode("utf-8")


names = sorted(f for f in os.listdir(MIGRATIONS) if f.endswith(".sql"))

# The ledger is the source of truth for what is already applied.
applied_versions = set()
status, payload = api_call(f"projects/{ref}/database/query",
                           "select version from supabase_migrations.schema_migrations")
if status in (200, 201):
    applied_versions = {row["version"] for row in json.loads(payload)}
    print(f"== ledger already records {len(applied_versions)} applied version(s) ==")
else:
    print(f"== ledger not readable yet (HTTP {status}) — applying everything ==")

print(f"== {len(names)} migration file(s) in the repository ==")
for name in names:
    version = name.split("_")[0]
    if version in applied_versions:
        print(f"SKIP: {name} (already recorded in the ledger)")
        continue
    with open(os.path.join(MIGRATIONS, name), "r", encoding="utf-8") as handle:
        sql = handle.read()
    try:
        status, payload = api_call(f"projects/{ref}/database/query", sql)
    except urllib.error.HTTPError as error:
        print(f"FAIL: {name} — HTTP {error.code}: {error.read().decode('utf-8')[:400]}")
        sys.exit(1)
    if status not in (200, 201):
        print(f"FAIL: {name} — HTTP {status}: {payload[:400]}")
        sys.exit(1)
    print(f"PASS: {name} applied ({len(sql)} bytes)")

# The migration ledger (the standard Supabase CLI shape) so the project's
# history is inspectable and the drift detector has a source of truth.
ledger_sql = """
create schema if not exists supabase_migrations;
create table if not exists supabase_migrations.schema_migrations (
  version text primary key,
  statements text[],
  name text not null,
  created_at timestamptz not null default now()
);
"""
status, payload = api_call(f"projects/{ref}/database/query", ledger_sql)
if status not in (200, 201):
    print(f"FAIL: ledger bootstrap — HTTP {status}: {payload[:400]}")
    sys.exit(1)
print("PASS: migration ledger ready")

for name in names:
    version = name.split("_")[0]
    with open(os.path.join(MIGRATIONS, name), "r", encoding="utf-8") as handle:
        content = handle.read()

    def sql_literal(value: str) -> str:
        return "'" + value.replace("'", "''") + "'"

    record = (f"insert into supabase_migrations.schema_migrations (version, name, statements) "
              f"values ({sql_literal(version)}, {sql_literal(name)}, array[{sql_literal(content)}]::text[]) "
              f"on conflict (version) do nothing;")
    status, payload = api_call(f"projects/{ref}/database/query", record)
    if status not in (200, 201):
        print(f"FAIL: ledger insert {name} — HTTP {status}: {payload[:400]}")
        sys.exit(1)
    print(f"PASS: ledger entry {version} ({name})")

print("== post-apply inventory ==")
inventory_sql = """
select 'tables' as kind, count(*)::text as value from pg_tables where schemaname = 'public'
union all
select 'tables_with_rls', count(*)::text from pg_tables where schemaname = 'public' and rowsecurity
union all
select 'policies', count(*)::text from pg_policies where schemaname = 'public'
union all
select 'functions_public', count(*)::text from pg_proc p join pg_namespace n on n.oid = p.pronamespace where n.nspname = 'public'
union all
select 'ledger_rows', count(*)::text from supabase_migrations.schema_migrations
"""
status, payload = api_call(f"projects/{ref}/database/query", inventory_sql)
if status in (200, 201):
    for row in json.loads(payload):
        print(f"   {row['kind']}: {row['value']}")
else:
    print(f"WARN: inventory query failed — HTTP {status}: {payload[:200]}")
print("[PASS] all migrations applied")
