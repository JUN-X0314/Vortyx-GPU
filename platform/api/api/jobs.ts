// GET  /api/jobs — list the authenticated user's jobs.
// POST /api/jobs — submit a job (idempotent by job_id: identical replay ->
// created:false with the existing record; different payload -> 409).
// Other methods: 405.
import { createApiHandler } from "../src/vercel.ts";

export default createApiHandler();
