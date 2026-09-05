// GET /api/jobs/:id — one own job (any state). Missing or foreign id -> 404.
import { createApiHandler } from "../../src/vercel.ts";

export default createApiHandler();
