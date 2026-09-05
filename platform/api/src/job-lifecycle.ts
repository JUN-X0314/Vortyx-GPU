// Job lifecycle mirror (Phase 11) — the documented transition table.
//
// The control-plane lifecycle (queued -> running -> completed/failed/
// cancelled) is deliberately NOT the local TaskQueue's TaskState lifecycle
// (which has no Cancelled — see src/core/queue/task_queue.hpp). The table
// below is the same pure function the C++ store applies
// (job_status_transition_valid in src/platform/job.cpp); both layers and the
// RLS-adjacent API checks agree, and tests pin it on both sides.

import type { JobStatus } from "./types.ts";

const TERMINAL: ReadonlySet<JobStatus> = new Set(["completed", "failed", "cancelled"]);

const TRANSITIONS: Readonly<Record<"queued" | "running", readonly JobStatus[]>> = {
  queued: ["running", "cancelled"],
  running: ["completed", "failed", "cancelled"],
};

export function isTerminal(status: JobStatus): boolean {
  return TERMINAL.has(status);
}

export function transitionValid(from: JobStatus, to: JobStatus): boolean {
  if (TERMINAL.has(from)) return false;
  if (from !== "queued" && from !== "running") return false;
  return TRANSITIONS[from].includes(to);
}
