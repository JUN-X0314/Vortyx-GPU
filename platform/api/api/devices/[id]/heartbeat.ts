// PATCH /api/devices/:id/heartbeat — mark an OWN device as heard-from
// (status online, server last_seen stamp). Missing or foreign id -> 404.
import { createApiHandler } from "../../../src/vercel.ts";

export default createApiHandler();
