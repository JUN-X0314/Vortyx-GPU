// POST /api/jobs/:id/cancel — owner cancellation of a queued/running job.
// Already-terminal -> 422 (illegal transition). Missing or foreign -> 404.
import { createApiHandler } from "../../../src/vercel.ts";

export default createApiHandler();
