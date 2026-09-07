// GET /api/platform/info — what this control plane speaks (protocol version,
// operation vocabulary, backend names). No authentication.
import { createApiHandler } from "../../src/vercel.ts";

export default createApiHandler();
