// Environment configuration (Phase 11).
//
// Secret classification — the whole point of this module is to keep the
// classes explicit and impossible to mix up accidentally:
//
//   SERVER-ONLY (never expose to any client, never commit):
//     SUPABASE_SERVICE_ROLE_KEY  — bypasses RLS. Phase 15 uses it ONLY for
//                                  the worker-protocol paths (claim/heartbeat/
//                                  complete run outside any user's identity)
//                                  and reconciliation. Never logged, never
//                                  serialized, never sent to a browser.
//     VORTYX_WORKER_TOKEN        — the native worker's bearer secret.
//     VORTYX_RECONCILE_TOKEN     — the reconciliation/cron secret.
//
//   SERVER-SIDE CONFIG (not secrets, not for browsers):
//     SUPABASE_URL, SUPABASE_ANON_KEY, VORTYX_STORE, VORTYX_ALLOWED_ORIGIN
//
//   PUBLISHABLE BY DESIGN:
//     SUPABASE_ANON_KEY — the anon key is meant to be client-visible; data
//                         safety comes from RLS, never from key secrecy.
//
// There is deliberately no NEXT_PUBLIC_* naming: this is a plain Vercel
// Functions project, nothing here is shipped to a browser, and the future
// device agent (C++) receives its connection settings through its own
// configuration, not through a web framework convention.
//
// NO REAL VALUES ARE COMMITTED ANYWHERE IN PHASE 11. .env.example is the
// template; .env* files are git-ignored.

import { makeAuthenticated, type AuthContext } from "./auth.ts";
import { SOFTWARE_VERSION } from "./version.ts";
import type { IPlatformStore } from "./store.ts";
import { InMemoryPlatformStore } from "./memory-store.ts";
import { InMemoryServiceStore, type IServiceStore } from "./service-store.ts";

export type StoreKind = "memory" | "supabase";

export interface PlatformConfig {
  store: StoreKind;
  supabaseUrl: string | null;
  supabaseAnonKey: string | null;
  /** Presence only — the value is never read, logged or echoed. */
  serviceRoleKeyPresent: boolean;
  /** The native worker's bearer secret (server-side only; presence-counted). */
  workerToken: string | null;
  /** The reconcile/cron secret (server-side only). */
  reconcileToken: string | null;
  /** The single origin allowed to call this API from a browser ("" = none). */
  allowedOrigin: string;
  /** Set when VORTYX_STORE=supabase but required settings are missing. */
  configError: string | null;
}

export function readConfig(env: Record<string, string | undefined>): PlatformConfig {
  const storeRaw = env["VORTYX_STORE"] ?? "memory";
  const store: StoreKind = storeRaw === "supabase" ? "supabase" : "memory";
  const supabaseUrl = env["SUPABASE_URL"] ?? null;
  const supabaseAnonKey = env["SUPABASE_ANON_KEY"] ?? null;
  const allowedOrigin = env["VORTYX_ALLOWED_ORIGIN"] ?? "";

  let configError: string | null = null;
  if (store === "supabase") {
    if (!supabaseUrl || !supabaseAnonKey) {
      configError =
        "VORTYX_STORE=supabase requires SUPABASE_URL and SUPABASE_ANON_KEY (see .env.example)";
    }
  }
  return {
    store,
    supabaseUrl,
    supabaseAnonKey,
    serviceRoleKeyPresent: Boolean(env["SUPABASE_SERVICE_ROLE_KEY"]),
    workerToken: env["VORTYX_WORKER_TOKEN"] ?? null,
    reconcileToken: env["VORTYX_RECONCILE_TOKEN"] ?? env["CRON_SECRET"] ?? null,
    allowedOrigin,
    configError,
  };
}

// ---------------------------------------------------------------------------
// Store + token verifier resolution
// ---------------------------------------------------------------------------

/**
 * Resolves the Bearer token into an AuthContext, or null when the token is
 * unusable. The memory verifier accepts ONLY the documented local scheme
 * `local:<user_id>` — a local/mock convenience that must never reach a real
 * deployment (there is no real credential behind it). The supabase verifier
 * validates a genuine Supabase Auth access token server-side.
 */
export type TokenVerifier = (token: string) => Promise<AuthContext | null>;

function memoryVerifier(): TokenVerifier {
  return async (token: string) => {
    if (!token.startsWith("local:")) return null;
    const userId = token.slice("local:".length);
    if (userId.length === 0) return null;
    return makeAuthenticated(userId);
  };
}

function supabaseVerifier(url: string, anonKey: string): TokenVerifier {
  return async (token: string) => {
    // Dynamic import: nothing in the test or local-dev path ever loads the
    // supabase adapter (or @supabase/supabase-js).
    const adapter = await import("./supabase-store.ts");
    const userId = await adapter.verifyAccessToken(url, anonKey, token);
    if (userId === null) return null;
    return {
      authenticated: true,
      user_id: userId,
      access_token: token, // the adapter runs RLS-scoped queries as this user
    };
  };
}

export interface ResolvedPlatform {
  store: IPlatformStore;
  verifier: TokenVerifier;
  storeKind: StoreKind;
  /** The API's software version (single source: version.ts). */
  softwareVersion: string;
  service: IServiceStore;
  workerToken: string | null;
  reconcileToken: string | null;
  allowedOrigin: string;
}

/**
 * Resolves the concrete provider behind the platform interfaces from the
 * environment. Throws Error with a diagnostic message on misconfiguration —
 * the router turns that into a 500 config_error response (no fake success).
 */
export async function resolvePlatform(config: PlatformConfig): Promise<ResolvedPlatform> {
  if (config.store === "supabase") {
    if (config.configError !== null || config.supabaseUrl === null || config.supabaseAnonKey === null) {
      throw new Error(config.configError ?? "supabase store is not configured");
    }
    // Dynamic import: nothing in the test or local-dev path ever loads the
    // supabase adapters (or @supabase/supabase-js).
    const adapter = await import("./supabase-store.ts");
    const serviceAdapter = await import("./supabase-service-store.ts");
    const service = serviceAdapter.createSupabaseServiceStore(
      config.supabaseUrl,
      config.supabaseAnonKey,
      env_service_role(),
    );
    return {
      store: adapter.createSupabaseStore(config.supabaseUrl, config.supabaseAnonKey),
      verifier: supabaseVerifier(config.supabaseUrl, config.supabaseAnonKey),
      storeKind: "supabase",
      softwareVersion: SOFTWARE_VERSION,
      service,
      workerToken: config.workerToken,
      reconcileToken: config.reconcileToken,
      allowedOrigin: config.allowedOrigin,
    };
  }
  return {
    store: new InMemoryPlatformStore(),
    verifier: memoryVerifier(),
    storeKind: "memory",
    softwareVersion: SOFTWARE_VERSION,
    service: new InMemoryServiceStore(),
    workerToken: config.workerToken,
    reconcileToken: config.reconcileToken,
    allowedOrigin: config.allowedOrigin,
  };
}

/** The service-role key never leaves the server environment. */
function env_service_role(): string | null {
  return process.env["SUPABASE_SERVICE_ROLE_KEY"] ?? null;
}
