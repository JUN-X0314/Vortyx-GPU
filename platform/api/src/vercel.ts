// Vercel adapter (Phase 11) — turns the framework-independent router into
// Vercel Node.js function handlers.
//
// Every file in api/ is a 3-line adapter importing createApiHandler() from
// here; nothing else in the project imports this module. The handler shapes
// below are STRUCTURAL (no @vercel/node dependency): Vercel's runtime passes
// objects with exactly these members, and the local dev server mimics them,
// so the same code runs in both places.
//
// Lifecycle note (honest): the platform instance is cached per module — on
// a warm lambda all requests share it, on a cold start it is rebuilt. With
// the memory store that means state is per-lambda-instance and EPHEMERAL;
// memory mode exists for local development and tests, and a real deployment
// uses VORTYX_STORE=supabase (stateless adapter + PostgreSQL durability).

import { handlePlatformRequest } from "./router.ts";
import { readConfig, resolvePlatform, type ResolvedPlatform } from "./config.ts";

export interface ApiRequestLike {
  method?: string;
  url?: string;
  headers: Record<string, string | string[] | undefined>;
  body?: unknown;
}

export interface ApiResponseLike {
  status(code: number): ApiResponseLike;
  json(body: unknown): void;
}

let platformCache: Promise<ResolvedPlatform> | null = null;

function platform(): Promise<ResolvedPlatform> {
  if (platformCache === null) {
    platformCache = resolvePlatform(readConfig(process.env)).catch((error) => {
      platformCache = null; // allow retry after a fix/redeploy
      throw error;
    });
  }
  return platformCache;
}

function pathOf(rawUrl: string | undefined): string {
  if (rawUrl === undefined || rawUrl.length === 0) return "/";
  try {
    return new URL(rawUrl, "http://localhost").pathname;
  } catch {
    return rawUrl;
  }
}

function headerValue(headers: Record<string, string | string[] | undefined>, name: string): string | undefined {
  const value = headers[name];
  if (value === undefined) return undefined;
  return Array.isArray(value) ? value[0] : value;
}

export function createApiHandler(): (req: ApiRequestLike, res: ApiResponseLike) => Promise<void> {
  return async (req, res) => {
    let resolved: ResolvedPlatform;
    try {
      resolved = await platform();
    } catch (error) {
      const message = error instanceof Error ? error.message : "platform misconfiguration";
      res.status(500).json({ error: { code: "config_error", message } });
      return;
    }
    const response = await handlePlatformRequest(
      {
        method: req.method ?? "GET",
        path: pathOf(req.url),
        body: req.body,
        authorization: headerValue(req.headers, "authorization"),
      },
      {
        store: resolved.store,
        verifier: resolved.verifier,
        storeKind: resolved.storeKind,
        softwareVersion: SOFTWARE_VERSION,
      },
    );
    res.status(response.status).json(response.body);
  };
}

// The API reports the Vortyx version it was built from. Single source of
// truth on the C++ side is src/core/version.hpp; this constant mirrors it
// for the /api/platform/info payload (checked consistent in docs).
const SOFTWARE_VERSION = "0.14.0";
