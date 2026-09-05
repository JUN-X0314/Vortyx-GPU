// GET /api/health — liveness + configuration readiness. No authentication.
import { createApiHandler } from "../src/vercel.ts";

export default createApiHandler();
