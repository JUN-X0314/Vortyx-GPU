// Dashboard — REAL numbers from the API only: the caller's project count,
// their jobs by status, and their projects. Hardware telemetry that Vortyx
// does not measure is NEVER invented here (no fake utilization, no fake
// VRAM: unavailable means not shown).

import { el, clear, statusBadge, renderLoading, renderEmpty } from "../ui.js";

export async function renderDashboard(main, api) {
  renderLoading(main, "Loading dashboard…");
  const [projectsResult, jobsResult] = await Promise.all([
    api.get("/api/projects"),
    api.get("/api/jobs?limit=100"),
  ]);
  const projects = projectsResult.projects ?? [];
  const jobs = jobsResult.jobs ?? [];

  clear(main);
  main.append(el("h1", { text: "Dashboard" }));

  // The summary cards — every number is an aggregate of real records.
  const byStatus = { queued: 0, running: 0, completed: 0, failed: 0, cancelled: 0 };
  for (const job of jobs) byStatus[job.status] = (byStatus[job.status] ?? 0) + 1;

  const cards = el("div", { class: "cards" });
  cards.append(
    metricCard("Projects", String(projects.length), "#/projects"),
    metricCard("Active jobs", String(byStatus.queued + byStatus.running), "#/projects"),
    metricCard("Completed", String(byStatus.completed)),
    metricCard("Failed", String(byStatus.failed)),
  );
  main.append(cards);

  // Execution health: what the API actually knows — the queue/running
  // states. There is no device telemetry in this response and none is
  // claimed; the Devices screen shows the real registry instead.
  const recent = el("section", { class: "panel" });
  recent.append(el("h2", { text: "Recent jobs" }));
  if (jobs.length === 0) {
    recent.append(el("p", { class: "muted", text: "No jobs yet. Open a project and submit the first job." }));
  } else {
    const table = jobTable(jobs.slice(0, 8));
    recent.append(table);
  }
  main.append(recent);

  const projectPanel = el("section", { class: "panel" });
  projectPanel.append(el("h2", { text: "Your projects" }));
  if (projects.length === 0) {
    projectPanel.append(el("p", { class: "muted", text: "No projects yet — create one from the Projects screen." }));
  } else {
    const list = el("ul", { class: "rows" });
    for (const project of projects.slice(0, 6)) {
      const row = el("li", {});
      row.append(el("a", { href: `#/projects/${project.project_id}`, text: project.name }),
        el("span", { class: `badge project-${project.status}`, text: project.status }));
      list.append(row);
    }
    projectPanel.append(list);
  }
  main.append(projectPanel);
}

function metricCard(label, value, href) {
  const card = el("div", { class: "card metric" });
  const content = [el("span", { class: "metric-label", text: label }), el("strong", { class: "metric-value", text: value })];
  if (href !== undefined) {
    const link = el("a", { href, class: "card-link" });
    link.append(...content);
    card.append(link);
  } else {
    card.append(...content);
  }
  return card;
}

export function jobTable(jobs) {
  const table = el("table", { class: "data" });
  const head = el("tr", {});
  for (const label of ["Job", "Operation", "Status", "Submitted", "Result"]) {
    head.append(el("th", { text: label }));
  }
  table.append(head);
  for (const job of jobs) {
    const row = el("tr", {});
    row.append(
      el("td", {}, el("a", { href: `#/jobs/${job.job_id}`, text: job.job_id.slice(0, 13) })),
      el("td", { text: job.operation }),
      el("td", {}, statusBadge(job.status)),
      el("td", { text: new Date(job.submitted_at_ms).toLocaleString() }),
      el("td", {
        text: job.result_element_count === null || job.result_element_count === undefined
          ? "—"
          : `${job.result_element_count} elements`,
      }),
    );
    table.append(row);
  }
  return table;
}
