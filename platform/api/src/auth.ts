// Auth boundary mirror (Phase 11) — AuthN vs AuthZ, one ownership rule.
//
// AuthN ("who are you?") happens at the transport boundary: the router
// resolves the Bearer token into an AuthContext (in production through
// Supabase Auth; in local/mock mode through the documented local scheme).
// AuthZ ("may you touch this record?") is the pure rule below — the same
// rule the C++ store (src/platform/auth.hpp) applies and the Supabase RLS
// policies (platform/supabase/migrations) enforce at the database level.
//
// A user_id claimed inside a request BODY is never an identity. Only the
// verified subject is.

import type { PlatformStatus } from "./types.ts";

export interface AuthContext {
  authenticated: boolean;
  user_id: string;
  /**
   * The caller's access token (supabase mode only). Server-side plumbing:
   * the Supabase adapter uses it so every query runs AS THE USER under RLS.
   * The memory verifier never sets it, and it is never logged or echoed.
   */
  access_token?: string;
}

export function makeAuthenticated(userId: string): AuthContext {
  return { authenticated: true, user_id: userId };
}

export function anonymous(): AuthContext {
  return { authenticated: false, user_id: "" };
}

export interface AuthFailure {
  status: "unauthenticated" | "forbidden";
  message: string;
}

/** Validates that `auth` carries a usable identity (mirror of validate_auth). */
export function validateAuth(
  auth: AuthContext,
): { ok: true } | { ok: false; status: "unauthenticated"; message: string } {
  if (!auth.authenticated || auth.user_id.length === 0) {
    return {
      ok: false,
      status: "unauthenticated",
      message: "authentication required (no usable identity was presented)",
    };
  }
  return { ok: true };
}

/** The single ownership rule (mirror of is_owner). */
export function isOwner(auth: AuthContext, ownerUserId: string): boolean {
  return auth.authenticated && auth.user_id.length > 0 && auth.user_id === ownerUserId;
}

/**
 * Full access decision for a record owned by `ownerUserId`
 * (mirror of authorize_record_access).
 */
export function authorizeRecordAccess(
  auth: AuthContext,
  ownerUserId: string,
): { ok: true } | { ok: false; status: PlatformStatus; message: string } {
  const verdict = validateAuth(auth);
  if (!verdict.ok) return verdict;
  if (!isOwner(auth, ownerUserId)) {
    return {
      ok: false,
      status: "forbidden",
      message: "the authenticated user does not own this resource",
    };
  }
  return { ok: true };
}
