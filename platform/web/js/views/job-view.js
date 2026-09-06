// Job detail — the REAL lifecycle: status, attempt, shard summary, error,
// result metadata, execution plan (when the API provides one), cancel
// (when the role permits; the server decides).

import { el, clear, formatTime, statusBadge, renderLoading, renderError } from "../ui.js";

export async function renderJobDetail(main, api, jobId) {
  renderLoading(main, "Loading job…");
  let job;
  try {
    job = await api.get(`/api/service/jobs/${jobId}`);
  } catch (error) {
    renderError(main, error, "#/projects");
    return;
  }

  clear(main);
  const header = el("div", { class: "page-head" });
  header.append(el("h1", { class: "mono", text: job.job_id }), statusBadge(job.status));
  const canCancel =
    (job.status === "queued" || job.status === "running") &&
    (job.submitted_by !== "" ); // visibility already proves the caller can see it; server re-checks role
  if (canCancel) {
    const cancelButton = el("button", { class: "danger ghost", text: "Cancel job" });
    const errorLine = el("p", { class: "form-error hidden" });
    cancelButton.addEventListener("click", async () => {
      cancelButton.disabled = true;
      try {
        await api.post(`/api/service/jobs/${job.job_id}/cancel`, {});
        await renderJobDetail(main, api, jobId); // re-fetch the server truth
      } catch (error) {
        // The honest race outcome: a completed job refuses cancellation.
        errorLine.textContent = error.message;
        errorLine.classList.remove("hidden");
        cancelButton.disabled = false;
      }
    });
    header.append(cancelButton, errorLine);
  }
  main.append(header);

  const panel = el("section", { class: "panel" });
  panel.append(el("h2", { text: "Submission" }));
  panel.append(
    defs([
      ["Project", job.project_id],
      ["Submitted by", job.submitted_by],
      ["Operation", job.operation],
      ["Element count", String(job.element_count)],
      ["Requested shards", String(job.requested_shard_count)],
      ["Backend preference", job.requested_backend === "" ? "any" : job.requested_backend],
      ["Submitted", formatTime(job.submitted_at_ms)],
      ["Attempt", String(job.attempt)],
      ["Cancel requested", job.cancel_requested ? "yes" : "no"],
    ]),
  );
  main.append(panel);

  const execution = el("section", { class: "panel" });
  execution.append(el("h2", { text: "Execution" }));
  const rows = [
    ["Status", job.status],
    ["Terminal at", job.terminal_at_ms === null ? "not terminal yet" : formatTime(job.terminal_at_ms)],
    ["Shards (total / succeeded / failed)",
      job.total_shards === null ? "not reported yet" : `${job.total_shards} / ${job.succeeded_shards} / ${job.failed_shards}`],
    ["Result", job.result_element_count === null ? "—" : `${job.result_element_count} elements via ${job.result_backend ?? "unknown backend"}`],
    ["Error", job.error === "" ? "—" : job.error],
  ];
  execution.append(defs(rows));
  main.append(execution);

  // Phase 16: the execution plan — ONLY what the API actually provides.
  // A null plan renders an explicit "not available" state; a present plan
  // renders its own recorded fields verbatim. The UI never invents a
  // value the API did not send, and every string lands via textContent
  // (no HTML injection from any API field).
  const plan = el("section", { class: "panel" });
  plan.append(el("h2", { text: "Execution plan" }));
  const planRows = planSummaryRows(job.plan);
  if (planRows !== null) {
    plan.append(defs(planRows));
  } else {
    plan.append(
      el("p", {
        class: "muted",
        text: "Not available: this job was not planned by the Adaptive Compute Fabric (planning is opt-in on the control plane).",
      }),
    );
  }
  main.append(plan);

  // The honest waiting-state note (never a fake terminal).
  if (job.status === "queued") {
    main.append(el("p", { class: "muted", text: "This job is queued in the control plane. A native worker claims it when one is connected — until then it stays queued." }));
  } else if (job.status === "running") {
    main.append(el("p", { class: "muted", text: "A worker holds the lease and the Phase 12 scheduler is executing the shards. Refresh to see the current state." }));
  }
}

function defs(pairs) {
  const list = el("dl", { class: "defs" });
  for (const [label, value] of pairs) {
    list.append(el("dt", { text: label }), el("dd", { text: String(value) }));
  }
  return list;
}

// The plan section's [label, value] rows — a PURE function so the logic
// (what the console shows for a plan, and what it refuses to invent) is
// testable without a DOM. Returns null when there is no plan object — the
// caller renders the explicit "not available" state. Every value is a
// field the API actually sent, or an em-dash placeholder: nothing here
// fabricates a version, a device or a reason.
export function planSummaryRows(plan) {
  if (!plan || typeof plan !== "object") return null;
  const devices = Array.isArray(plan.devices) ? plan.devices : [];
  return [
    ["Plan version", plan.plan_version === null || plan.plan_version === undefined ? "—" : String(plan.plan_version)],
    ["Planner", plan.planner || "—"],
    ["Planner version", plan.planner_version || "—"],
    ["Devices", devices.length > 0 ? devices.join(", ") : "none recorded"],
    ["Reason", plan.reason || "—"],
  ];
}
