// Supabase Auth session (Phase 15 web console) — plain ES module (no build).
//
// The browser talks to Supabase Auth's REST API DIRECTLY for signup /
// login / refresh / logout — with the PUBLISHABLE anon key only (it is
// public by design; data safety comes from RLS). The service-role key NEVER
// reaches this code, and no password material is ever stored here: the
// session lives in localStorage as the provider issued it (access +
// refresh token), refreshed when expired, cleared on logout.
//
// The Vortyx API is called with the Supabase ACCESS token as the Bearer
// credential; the API verifies it server-side (the Phase 11 verifier) and
// resolves the verified subject — a client-claimed user id is never an
// identity.
//
// A 401 from the API (expired/invalid session) is surfaced as
// SessionExpiredError: the router renders the login screen — never a fake
// authenticated state.

const REFRESH_SKEW_MS = 60000; // refresh this long before expiry
const STORAGE_KEY = "vortyx.session";

export class SessionExpiredError extends Error {
  constructor() {
    super("the session has expired; sign in again");
    this.name = "SessionExpiredError";
  }
}

/**
 * Reads the deployer's publishable config (js/config.js fills
 * window.VORTYX_CONFIG). No secret belongs here — ever.
 * @returns {{supabaseUrl: string, supabaseAnonKey: string}}
 */
export function loadConfig() {
  const globalConfig = globalThis.VORTYX_CONFIG ?? {};
  const url = globalConfig.supabaseUrl ?? "";
  const anonKey = globalConfig.supabaseAnonKey ?? "";
  if (url.length === 0 || anonKey.length === 0) {
    throw new Error(
      "the web console is not configured: set supabaseUrl and supabaseAnonKey in js/config.js (publishable values only)",
    );
  }
  return { supabaseUrl: url.replace(/\/$/, ""), supabaseAnonKey: anonKey };
}

// ---------------------------------------------------------------------------
// Storage (localStorage survives reloads)
// ---------------------------------------------------------------------------

/** @returns {object|null} */
export function readStoredSession() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw === null ? null : JSON.parse(raw);
  } catch {
    return null;
  }
}

function writeStoredSession(session) {
  try {
    if (session === null) localStorage.removeItem(STORAGE_KEY);
    else localStorage.setItem(STORAGE_KEY, JSON.stringify(session));
  } catch {
    // Private-mode storage refusals: the session just does not persist.
  }
}

// ---------------------------------------------------------------------------
// The REST calls (Supabase Auth /auth/v1)
// ---------------------------------------------------------------------------

/**
 * @param {{supabaseUrl: string, supabaseAnonKey: string}} config
 * @param {string} path
 * @param {object|null} body
 */
async function authFetch(config, path, body) {
  const response = await fetch(`${config.supabaseUrl}/auth/v1/${path}`, {
    method: body === null ? "GET" : "POST",
    headers: {
      "content-type": "application/json",
      apikey: config.supabaseAnonKey,
      Authorization: `Bearer ${config.supabaseAnonKey}`,
    },
    body: body === null ? undefined : JSON.stringify(body),
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) {
    const message =
      payload.msg ?? payload.error_description ?? payload.error ??
      `auth request failed (${response.status})`;
    throw new Error(message);
  }
  return payload;
}

function toSession(payload) {
  if (payload.user === null || payload.user === undefined) {
    throw new Error("the auth response carried no user");
  }
  return {
    access_token: payload.access_token,
    refresh_token: payload.refresh_token,
    expires_at_ms: payload.expires_at * 1000,
    user_id: payload.user.id,
    email: payload.user.email ?? "",
  };
}

function persist(session) {
  writeStoredSession({
    access_token: session.access_token,
    refresh_token: session.refresh_token,
    expires_at: Math.floor(session.expires_at_ms / 1000),
    user: { id: session.user_id, email: session.email },
  });
}

/**
 * @param {{supabaseUrl: string, supabaseAnonKey: string}} config
 * @param {string} email
 * @param {string} password
 * @returns {Promise<object>} the session
 */
export async function signUp(config, email, password) {
  // Supabase may require email confirmation; when it does, this returns a
  // session only AFTER the user confirms — reported honestly instead of
  // faking a signed-in state.
  const payload = await authFetch(config, "signup", { email, password });
  if (payload.access_token === undefined || payload.access_token.length === 0) {
    throw new Error(
      "confirmation required: check your email for the confirmation link, then sign in",
    );
  }
  const session = toSession(payload);
  persist(session);
  return session;
}

export async function signIn(config, email, password) {
  const payload = await authFetch(config, "token?grant_type=password", { email, password });
  const session = toSession(payload);
  persist(session);
  return session;
}

export async function signOut(config, session) {
  try {
    await fetch(`${config.supabaseUrl}/auth/v1/logout?scope=global`, {
      method: "POST",
      headers: {
        apikey: config.supabaseAnonKey,
        Authorization: `Bearer ${session.access_token}`,
      },
    });
  } catch {
    // The local session is cleared regardless; a network failure during
    // logout must not trap the user.
  }
  writeStoredSession(null);
}

/**
 * Restores the session after a reload: expired tokens are refreshed
 * server-side; an unusable refresh is an honest signed-out state.
 * @returns {Promise<object|null>}
 */
export async function restoreSession(config) {
  const stored = readStoredSession();
  if (stored === null) return null;
  if (stored.expires_at * 1000 > Date.now() + REFRESH_SKEW_MS) {
    return {
      access_token: stored.access_token,
      refresh_token: stored.refresh_token,
      expires_at_ms: stored.expires_at * 1000,
      user_id: stored.user.id,
      email: stored.user.email ?? "",
    };
  }
  return refreshToken(config, stored.refresh_token);
}

/** Exchanges the refresh token for a fresh session (rotation-safe). */
export async function refreshToken(config, refresh_token) {
  try {
    const payload = await authFetch(config, "token?grant_type=refresh_token", { refresh_token });
    const session = toSession(payload);
    persist(session);
    return session;
  } catch {
    writeStoredSession(null);
    return null;
  }
}

export function clearLocalSession() {
  writeStoredSession(null);
}
