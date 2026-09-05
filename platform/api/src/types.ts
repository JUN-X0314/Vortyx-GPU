// Data-model mirror of the C++ platform contract (Phase 11).
//
// The control-plane contract is implemented twice on purpose: once in C++
// (src/platform — the device-agent side and its executable specification)
// and once here in TypeScript (the Vercel-hosted API side). The types below
// mirror src/platform/{status,identity,metadata,job,store}.hpp field for
// field; tests on BOTH sides pin the same wire vocabulary so the two layers
// cannot drift.
//
// Timestamps are epoch milliseconds; `null` means "not set / not reported"
// — never a fabricated 0 (project-wide honesty rule).

/** Control-plane protocol version. Must equal the C++ kProtocolVersion. */
export const PROTOCOL_VERSION = "1";

/** Control-plane outcome vocabulary (mirror of vortyx::platform::Status). */
export type PlatformStatus =
  | "ok"
  | "invalid_input"
  | "unauthenticated"
  | "forbidden"
  | "not_found"
  | "conflict"
  | "internal";

/** Backend names the Phase 11 contract recognizes. */
export const KNOWN_BACKENDS = ["cpu", "vulkan"] as const;
export type BackendName = (typeof KNOWN_BACKENDS)[number];

/** Operation labels — exactly the Phase 10 ComputeOp workload labels. */
export const KNOWN_OPERATIONS = [
  "vector_add",
  "vector_multiply",
  "vector_scale",
] as const;
export type OperationLabel = (typeof KNOWN_OPERATIONS)[number];

export type DeviceStatus = "online" | "offline";

/** Self-reported node description (mirror of DeviceMetadata). */
export interface DeviceMetadata {
  protocol_version: string;
  software_version: string;
  operating_system: string;
  architecture: string;
  backends: string[];
  operations: string[];
  display_name: string;
}

/** One registered node (mirror of DeviceRecord; server fields are store-set). */
export interface DeviceRecord {
  device_id: string;
  owner_user_id: string;
  metadata: DeviceMetadata;
  status: DeviceStatus;
  last_seen_ms: number | null;
  created_at_ms: number | null;
}

/** Remote job lifecycle — deliberately NOT the local TaskQueue lifecycle. */
export type JobStatus =
  | "queued"
  | "running"
  | "completed"
  | "failed"
  | "cancelled";

/** Submission contract (mirror of JobEnvelope). NO data payload by design. */
export interface JobEnvelope {
  job_id: string;
  operation: OperationLabel;
  element_count: number;
  requested_backend: string; // "" = unspecified
  /** RESERVED transport field: no scheduling semantics in Phase 11. */
  priority: number;
  protocol_version: string;
  created_at_ms: number | null;
}

/** A job plus its control-plane state (mirror of JobRecord). */
export interface JobRecord {
  job: JobEnvelope;
  owner_user_id: string;
  submitted_by_device_id: string | null;
  status: JobStatus;
  error: string;
  created_at_ms: number | null;
  started_at_ms: number | null;
  completed_at_ms: number | null;
}

/** Execution outcome (mirror of ResultEnvelope). Metadata only. */
export interface ResultEnvelope {
  job_id: string;
  status: "completed" | "failed";
  backend: string; // "" = not reported
  error: string;
  result_element_count: number | null;
}

/** Upper bound of the control-plane contract (mirror of kMaxJobElementCount). */
export const MAX_JOB_ELEMENT_COUNT = 2147483647;
