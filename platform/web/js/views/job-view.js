// Job detail — the REAL lifecycle: status, attempt, shard summary, error,
// result metadata, cancel (when the role permits; the server decides).

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
