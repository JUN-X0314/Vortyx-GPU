// Project detail — tabs: overview, members, jobs, artifacts, quota, audit.
// Every tab shows REAL server state; member management and quota editing
// are gated server-side (the UI hides what the role cannot do, and the
// server re-checks everything — hiding a button is never the authorization).

import { el, clear, formatBytes, formatTime, renderLoading, renderError, renderEmpty } from "../ui.js";

const TABS = ["overview", "members", "jobs", "artifacts", "quota", "audit"];

export async function renderProject(main, api, projectId, tab) {
  renderLoading(main, "Loading project…");
  let project;
  let role;
  try {
    const result = await api.get(`/api/projects/${projectId}`);
    project = result;
    role = (await api.get("/api/projects")).projects.find((p) => p.project_id === projectId)?.role;
  } catch (error) {
    renderError(main, error, "#/projects");
    return;
  }

  clear(main);
  const isAdmin = role === "admin" || role === "owner";
  const isOwner = role === "owner";

  const header = el("div", { class: "page-head" });
  header.append(
    el("h1", { text: project.name }),
    el("span", { class: `badge project-${project.status}`, text: project.status }),
    el("span", { class: "badge", text: `your role: ${role ?? "unknown"}` }),
  );
  if (isOwner && project.status === "active") {
    const archiveButton = el("button", { class: "danger ghost", text: "Archive project" });
    archiveButton.addEventListener("click", async () => {
      if (!window.confirm("Archive this project? Archived projects refuse new submissions.")) return;
      try {
        await api.post(`/api/projects/${projectId}/archive`, {});
        location.reload();
      } catch (error) {
        renderError(main, error);
      }
    });
    header.append(archiveButton);
  }
  main.append(header);

  // The tab bar.
  const tabBar = el("nav", { class: "tabs" });
  for (const t of TABS) {
    if (t === "audit" && !isAdmin) continue; // server enforces admin+ too
    const link = el("a", { href: `#/projects/${projectId}/${t}`, text: t, class: t === tab ? "active" : "" });
    tabBar.append(link);
  }
  main.append(tabBar);

  const body = el("section", { class: "panel" });
  main.append(body);
  try {
    if (tab === "overview") await overviewTab(body, api, project);
    else if (tab === "members") await membersTab(body, api, projectId, isAdmin);
    else if (tab === "jobs") await jobsTab(body, api, projectId, role);
    else if (tab === "artifacts") await artifactsTab(body, api, projectId, role, isAdmin);
    else if (tab === "quota") await quotaTab(body, api, projectId, isAdmin);
    else if (tab === "audit") await auditTab(body, api, projectId);
  } catch (error) {
    renderError(body, error);
  }
}

async function overviewTab(body, api, project) {
  body.append(
    el("h2", { text: "Overview" }),
    definitionList([
      ["Project ID", project.project_id],
      ["Owner", project.owner_user_id],
      ["Status", project.status],
      ["Created", formatTime(project.created_at_ms)],
      ["Updated", formatTime(project.updated_at_ms)],
    ]),
    el("p", { class: "muted", text: "Submit a job from the Jobs tab. Supported operations are vector_add, vector_multiply and vector_scale." }),
  );
}

async function membersTab(body, api, projectId, isAdmin) {
  body.append(el("h2", { text: "Members" }));
  const result = await api.get(`/api/projects/${projectId}/members`);
  const members = result.members ?? [];

  if (isAdmin) {
    const form = el("form", { class: "inline-form" });
    const userInput = el("input", { type: "text", name: "user_id", placeholder: "user id (UUID)", required: "required" });
    const roleSelect = el("select", { name: "role" });
    // The Owner role is NEVER grantable (the single-owner invariant) — the
    // select simply does not offer it, and the server refuses it anyway.
    for (const [value, label] of [["admin", "Admin"], ["member", "Member"], ["viewer", "Viewer"]]) {
      roleSelect.append(el("option", { value, text: label }));
    }
    const addButton = el("button", { class: "primary", text: "Add member" });
    const errorLine = el("p", { class: "form-error hidden" });
    form.append(userInput, roleSelect, addButton, errorLine);
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      errorLine.classList.add("hidden");
      addButton.disabled = true;
      try {
        await api.post(`/api/projects/${projectId}/members`, {
          user_id: userInput.value.trim(), role: roleSelect.value,
        });
        location.reload();
      } catch (error) {
        errorLine.textContent = error.message;
        errorLine.classList.remove("hidden");
      } finally {
        addButton.disabled = false;
      }
    });
    body.append(form);
  }

  if (members.length === 0) {
    renderEmpty(body, "No members", "Add members with their user id (shown on their Settings screen).");
    return;
  }
  const table = el("table", { class: "data" });
  const head = el("tr", {});
  for (const label of ["User", "Role", "Added"]) head.append(el("th", { text: label }));
  table.append(head);
  for (const member of members) {
    const row = el("tr", {});
    row.append(
      el("td", { text: member.user_id }),
      el("td", {}, el("span", { class: `badge role-${member.role}`, text: member.role })),
      el("td", { text: formatTime(member.created_at_ms) }),
    );
    if (isAdmin && member.role !== "owner") {
      const removeButton = el("button", { class: "ghost danger", text: "Remove" });
      removeButton.addEventListener("click", async () => {
        try {
          await api.del(`/api/projects/${projectId}/members/${member.user_id}`);
          location.reload();
        } catch (error) {
          renderError(body, error);
        }
      });
      row.append(el("td", {}, removeButton));
    }
    table.append(row);
  }
  body.append(table);
}

async function jobsTab(body, api, projectId, role) {
  const head = el("div", { class: "tab-head" });
  head.append(el("h2", { text: "Jobs" }));
  if (role === "member" || role === "admin" || role === "owner") {
    head.append(el("a", { class: "button primary", href: `#/projects/${projectId}/submit`, text: "Submit job" }));
  }
  body.append(head);

  const result = await api.get(`/api/projects/${projectId}/jobs?limit=50`);
  const jobs = result.jobs ?? [];
  if (jobs.length === 0) {
    renderEmpty(body, "No jobs", "No job has been submitted to this project yet.");
    return;
  }
  const table = el("table", { class: "data" });
  const header = el("tr", {});
  for (const label of ["Job", "Operation", "Elements", "Shards", "Status", "Submitted", "Result"]) {
    header.append(el("th", { text: label }));
  }
  table.append(header);
  for (const job of jobs) {
    const row = el("tr", {});
    row.append(
      el("td", {}, el("a", { href: `#/jobs/${job.job_id}`, text: job.job_id.slice(0, 13) })),
      el("td", { text: job.operation }),
      el("td", { text: String(job.element_count) }),
      el("td", { text: String(job.requested_shard_count) }),
      el("td", {}, el("span", { class: `badge status-${job.status}`, text: job.status })),
      el("td", { text: formatTime(job.submitted_at_ms) }),
      el("td", {
        text: job.result_element_count === null ? "—" : `${job.result_element_count} elements`,
      }),
    );
    table.append(row);
  }
  body.append(table);
  if (result.next_offset !== null) {
    body.append(el("p", { class: "muted", text: "More jobs exist — the list is paginated by the API." }));
  }
}

async function artifactsTab(body, api, projectId, role, isAdmin) {
  body.append(el("h2", { text: "Artifacts" }));
  body.append(el("p", { class: "muted", text: "Artifact METADATA only — payload bytes are not stored by the control plane." }));

  if (role === "member" || isAdmin) {
    const form = el("form", { class: "inline-form" });
    const nameInput = el("input", { type: "text", name: "name", placeholder: "artifact name", required: "required", maxlength: "128" });
    const sizeInput = el("input", { type: "number", name: "declared_byte_size", placeholder: "declared size (bytes)", min: "0", required: "required" });
    const registerButton = el("button", { class: "primary", text: "Register" });
    const errorLine = el("p", { class: "form-error hidden" });
    form.append(nameInput, sizeInput, registerButton, errorLine);
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      errorLine.classList.add("hidden");
      registerButton.disabled = true;
      try {
        await api.post(`/api/projects/${projectId}/artifacts`, {
          name: nameInput.value.trim(), declared_byte_size: Number(sizeInput.value),
        });
        location.reload();
      } catch (error) {
        errorLine.textContent = error.message;
        errorLine.classList.remove("hidden");
      } finally {
        registerButton.disabled = false;
      }
    });
    body.append(form);
  }

  const result = await api.get(`/api/projects/${projectId}/artifacts`);
  const artifacts = result.artifacts ?? [];
  if (artifacts.length === 0) {
    renderEmpty(body, "No artifacts", "No artifact metadata registered in this project.");
    return;
  }
  const table = el("table", { class: "data" });
  const head = el("tr", {});
  for (const label of ["Name", "Declared size", "Created by", "Created"]) head.append(el("th", { text: label }));
  table.append(head);
  for (const artifact of artifacts) {
    const row = el("tr", {});
    row.append(
      el("td", { text: artifact.name }),
      el("td", { text: formatBytes(artifact.declared_byte_size) }),
      el("td", { text: artifact.created_by }),
      el("td", { text: formatTime(artifact.created_at_ms) }),
    );
    if (isAdmin || artifact.created_by !== "") {
      const delButton = el("button", { class: "ghost danger", text: "Delete" });
      delButton.addEventListener("click", async () => {
        try {
          await api.del(`/api/artifacts/${artifact.artifact_id}`);
          location.reload();
        } catch (error) {
          renderError(body, error);
        }
      });
      row.append(el("td", {}, delButton));
    }
    table.append(row);
  }
  body.append(table);
}

async function quotaTab(body, api, projectId, isAdmin) {
  body.append(el("h2", { text: "Quota & usage" }));
  const [quota, usage] = await Promise.all([
    api.get(`/api/projects/${projectId}/quota`),
    api.get(`/api/projects/${projectId}/usage`),
  ]);
  body.append(
    el("h3", { text: "Live usage (derived from in-flight jobs)" }),
    definitionList([
      ["Active jobs", `${usage.active_jobs}`],
      ["Requested shards", `${usage.running_shards}`],
      ["Reserved memory", formatBytes(usage.reserved_memory_bytes)],
    ]),
    el("h3", { text: "Policy" }),
    definitionList([
      ["Max concurrent jobs", `${quota.max_concurrent_jobs}`],
      ["Max running shards", `${quota.max_running_shards}`],
      ["Max reserved memory", formatBytes(quota.max_memory_bytes)],
    ]),
  );

  if (isAdmin) {
    const form = el("form", { class: "stack-form" });
    const jobsInput = el("input", { type: "number", name: "max_concurrent_jobs", value: String(quota.max_concurrent_jobs), min: "0", required: "required" });
    const shardsInput = el("input", { type: "number", name: "max_running_shards", value: String(quota.max_running_shards), min: "0", required: "required" });
    const memoryInput = el("input", { type: "number", name: "max_memory_bytes", value: String(quota.max_memory_bytes), min: "0", required: "required" });
    const saveButton = el("button", { class: "primary", text: "Save policy" });
    const errorLine = el("p", { class: "form-error hidden" });
    form.append(
      labeledField("Max concurrent jobs", jobsInput),
      labeledField("Max running shards", shardsInput),
      labeledField("Max reserved memory (bytes)", memoryInput),
      saveButton,
      errorLine,
    );
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      errorLine.classList.add("hidden");
      saveButton.disabled = true;
      try {
        await api.put(`/api/projects/${projectId}/quota`, {
          max_concurrent_jobs: Number(jobsInput.value),
          max_running_shards: Number(shardsInput.value),
          max_memory_bytes: Number(memoryInput.value),
        });
        location.reload();
      } catch (error) {
        errorLine.textContent = error.message;
        errorLine.classList.remove("hidden");
      } finally {
        saveButton.disabled = false;
      }
    });
    body.append(el("h3", { text: "Change policy (Admin+)" }), form);
  }
}

async function auditTab(body, api, projectId) {
  body.append(el("h2", { text: "Project audit" }));
  const result = await api.get(`/api/projects/${projectId}/audit?limit=100`);
  const events = result.events ?? [];
  if (events.length === 0) {
    renderEmpty(body, "No audit events", "This project has no recorded events yet.");
    return;
  }
  body.append(auditTable(events));
}

function auditTable(events) {
  const table = el("table", { class: "data" });
  const head = el("tr", {});
  for (const label of ["When", "Actor", "Action", "Outcome", "Reason", "Job"]) {
    head.append(el("th", { text: label }));
  }
  table.append(head);
  for (const event of events) {
    const row = el("tr", {});
    row.append(
      el("td", { text: formatTime(event.timestamp_ms) }),
      el("td", { text: event.actor_user_id ?? "system" }),
      el("td", { text: event.action }),
      el("td", {}, el("span", { class: `badge outcome-${event.outcome}`, text: event.outcome })),
      el("td", { text: event.reason_code === "" ? "—" : event.reason_code }),
      el("td", { text: event.job_id === null ? "—" : event.job_id.slice(0, 13) }),
    );
    table.append(row);
  }
  return table;
}

export { auditTable };

function definitionList(pairs) {
  const list = el("dl", { class: "defs" });
  for (const [label, value] of pairs) {
    list.append(el("dt", { text: label }), el("dd", { text: String(value) }));
  }
  return list;
}

function labeledField(labelText, input) {
  const wrap = el("label", { class: "field" });
  wrap.append(el("span", { text: labelText }), input);
  return wrap;
}
