// POST /api/devices/register — register a NEW device for the authenticated
// user. Duplicate id -> 409. Idempotency key: none (registration is
// create-only; clients generate a fresh UUID per node install).
import { createApiHandler } from "../src/vercel.ts";

export default createApiHandler();
