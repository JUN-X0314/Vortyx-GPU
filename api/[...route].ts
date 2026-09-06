// Vortyx Platform — single Vercel catch-all adapter for the existing API router.
//
// The implementation remains in platform/api/src/vercel.ts. This file is
// deployment glue only: it makes the repository's canonical API handlers
// reachable from the same Vercel project that serves platform/web.
import { createApiHandler } from "../platform/api/src/vercel.ts";

export default createApiHandler();
