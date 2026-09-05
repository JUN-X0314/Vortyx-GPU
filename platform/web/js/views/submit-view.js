// Job submit — only the operations the backend actually supports
// (vector_add | vector_multiply | vector_scale). The client validates for
// convenience; the SERVER validates for real.

import { el, clear, renderLoading, renderError } from "../ui.js";

const OPERATIONS = ["vector_add", "vector_multiply", "vector_scale"];
const BACKENDS = [["", "Any available"], ["cpu", "CPU"], ["vulkan", "Vulkan (if available)"]];
const MAX_ELEMENT_COUNT = 2147483647;
const MAX_SHARDS = 64;

function randomJobId() {
  const hex = () => Math.floor(Math.random() * 0xffff).toString(16).padStart(4, "0");
  return `job-${hex()}${hex()}-${hex()}-4${hex().slice(1)}-8${hex().slice(1)}-${hex()}${hex()}${hex()}`.slice(0, 36);
}

export async function renderJobSubmit(main, api, projectId) {
  renderLoading(main, "Loading…");
  let project;
  try {
    project = await api.get(`/api/projects/${projectId}`);
  } catch (error) {
    renderError(main, error, "#/projects");
    return;
  }

  clear(main);
  main.append(el("h1", { text: `Submit job — ${project.name}` }));

  const panel = el("section", { class: "panel" });
  const form = el("form", { class: "stack-form" });

  const jobIdInput = el("input", { type: "text", name: "job_id", value: randomJobId(), required: "required", pattern: "[A-Za-z0-9._-]{1,128}" });
  const opSelect = el("select", { name: "operation" });
  for (const op of OPERATIONS) opSelect.append(el("option", { value: op, text: op }));
  const elementInput = el("input", { type: "number", name: "element_count", value: "10000", min: "1", max: String(MAX_ELEMENT_COUNT), required: "required" });
  const shardInput = el("input", { type: "number", name: "requested_shard_count", value: "1", min: "1", max: String(MAX_SHARDS), required: "required" });
  const backendSelect = el("select", { name: "requested_backend" });
  for (const [value, label] of BACKENDS) backendSelect.append(el("option", { value, text: label }));

  const submitButton = el("button", { type: "submit", class: "primary", text: "Submit job" });
  const errorLine = el("p", { class: "form-error hidden" });

  const labeled = (labelText, input, hint) => {
    const wrap = el("label", { class: "field" });
    wrap.append(el("span", { text: labelText }), input);
    if (hint !== undefined) wrap.append(el("small", { class: "hint", text: hint }));
    return wrap;
  };

  form.append(
    labeled("Job ID (the idempotency key — resubmitting the same id replays)", jobIdInput),
    labeled("Operation", opSelect),
    labeled("Element count", elementInput, "The data-parallel domain size; the payload itself never leaves the worker."),
    labeled("Requested shard count", shardInput, "1 = single device; more lets the Phase 12 scheduler spread the work."),
    labeled("Backend preference", backendSelect),
    submitButton,
    errorLine,
  );

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    errorLine.classList.add("hidden");

    // Client-side convenience validation (the server re-validates for real).
    const elementCount = Number(elementInput.value);
    if (!Number.isInteger(elementCount) || elementCount < 1 || elementCount > MAX_ELEMENT_COUNT) {
      errorLine.textContent = `element_count must be 1..${MAX_ELEMENT_COUNT}`;
      errorLine.classList.remove("hidden");
      return;
    }

    submitButton.disabled = true;
    try {
      const created = await api.post(`/api/projects/${projectId}/jobs`, {
        job_id: jobIdInput.value.trim(),
        operation: opSelect.value,
        element_count: elementCount,
        requested_shard_count: Number(shardInput.value),
        requested_backend: backendSelect.value,
      });
      // Reflect the REAL server state: a replay reports created=false.
      if (created.created === false) {
        errorLine.classList.remove("hidden");
        errorLine.textContent = "Replayed an existing submission with this id (no new job created).";
        setTimeout(() => {
          location.hash = `#/jobs/${created.job_id}`;
        }, 1200);
      } else {
        location.hash = `#/jobs/${created.job_id}`;
      }
    } catch (error) {
      errorLine.textContent = error.message;
      errorLine.classList.remove("hidden");
    } finally {
      submitButton.disabled = false;
    }
  });

  panel.append(form);
  main.append(panel);
  main.append(
    el("p", { class: "muted", text: "Jobs are queued in the control plane and claimed by a native worker. When no worker is connected, jobs stay queued — the status below is always the server's truth." }),
  );
}
