// The API client (Phase 15 web console) — plain ES module (no build).
//
// ONE place knows how to talk to the Vortyx API: the base URL, the Bearer
// credential, the unified error body ({error:{code,message}}), the 401 ->
// refresh-and-retry path, and the session-lost handoff. Views never parse
// errors themselves — they branch on the mapped error views below.

import { refreshToken, SessionExpiredError } from "./auth.js";

export class ApiError extends Error {
  /**
   * @param {number} status
   * @param {string} code
   * @param {string} message
   */
  constructor(status, code, message) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.code = code;
  }
}

export class ApiClient {
  /**
   * @param {{baseUrl: string}} config
   * @param {object} authConfig the publishable Supabase config
   * @param {() => Promise<object|null>} getSession
   * @param {(session: object) => void} onSessionRefreshed
   * @param {() => void} onSessionLost
   */
  constructor(config, authConfig, getSession, onSessionRefreshed, onSessionLost) {
    this.config = config;
    this.authConfig = authConfig;
    this.getSession = getSession;
    this.onSessionRefreshed = onSessionRefreshed;
    this.onSessionLost = onSessionLost;
  }

  /**
   * @param {string} method
   * @param {string} path
   * @param {unknown} body
   * @param {boolean} retry
   * @returns {Promise<unknown>}
   */
  async request(method, path, body, retry = true) {
    const session = await this.getSession();
    if (session === null) throw new SessionExpiredError();
    const response = await fetch(`${this.config.baseUrl}${path}`, {
      method,
      headers: {
        "content-type": "application/json",
        Authorization: `Bearer ${session.access_token}`,
      },
      body: body === undefined ? undefined : JSON.stringify(body),
    });

    if (response.status === 401 && retry) {
      // The token may have just expired: one honest refresh + retry, then
      // the session is genuinely gone.
      let refreshed = null;
      try {
        refreshed = await refreshToken(this.authConfig, session.refresh_token);
      } catch {
        refreshed = null;
      }
      if (refreshed !== null) {
        this.onSessionRefreshed(refreshed);
        return this.request(method, path, body, false);
      }
      this.onSessionLost();
      throw new SessionExpiredError();
    }

    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      const error = payload.error;
      throw new ApiError(
        response.status,
        error?.code ?? "internal_error",
        error?.message ?? `request failed (${response.status})`,
      );
    }
    return payload;
  }

  get(path) {
    return this.request("GET", path, undefined);
  }
  post(path, body) {
    return this.request("POST", path, body);
  }
  put(path, body) {
    return this.request("PUT", path, body);
  }
  del(path) {
    return this.request("DELETE", path, undefined);
  }
}

// ---------------------------------------------------------------------------
// Error mapping for the views (one vocabulary for the whole console)
// ---------------------------------------------------------------------------

/**
 * @param {unknown} error
 * @returns {{kind: string, title: string, detail: string}}
 */
export function describeApiError(error) {
  if (error instanceof ApiError) {
    switch (error.code) {
      case "unauthenticated":
        return { kind: "unauthorized", title: "Sign in required", detail: "Your session is no longer valid. Sign in again to continue." };
      case "forbidden":
        return { kind: "forbidden", title: "Not allowed", detail: "Your role in this project does not permit this action." };
      case "not_found":
        return { kind: "not_found", title: "Not found", detail: "This resource does not exist or you cannot see it." };
      case "conflict":
        return { kind: "conflict", title: "Conflict", detail: error.message };
      case "quota_exceeded":
        return { kind: "quota", title: "Project quota reached", detail: error.message };
      case "rate_limit_exceeded":
        return { kind: "rate_limit", title: "Too many requests", detail: "Slow down for a moment and try again." };
      case "unavailable":
        return { kind: "unavailable", title: "Unavailable", detail: error.message };
      default:
        return { kind: "unknown", title: "Request failed", detail: error.message };
    }
  }
  if (error instanceof SessionExpiredError) {
    return { kind: "unauthorized", title: "Sign in required", detail: error.message };
  }
  return {
    kind: "network",
    title: "Network error",
    detail: error instanceof Error ? error.message : "the API could not be reached",
  };
}
