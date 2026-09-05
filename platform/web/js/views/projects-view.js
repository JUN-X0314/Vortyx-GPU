// Projects screen — the caller's REAL project list + creation.

import { el, clear, renderLoading, renderEmpty, renderError } from "../ui.js";

export async function renderProjects(main, api) {
  renderLoading(main, "Loading projects…");
  let projects;
  try {
    const result = await api.get("/api/projects");
    projects = result.projects ?? [];
  } catch (error) {
    renderError(main, error, "#/projects");
    return;
  }

  clear(main);
  main.append(el("h1", { text: "Projects" }));

  const createPanel = el("section", { class: "panel" });
  const nameInput = el("input", { type: "text", name: "name", placeholder: "New project name", maxlength: "128", required: "required" });
  const createButton = el("button", { class: "primary", text: "Create project" });
  const errorLine = el("p", { class: "form-error hidden" });
  const form = el("form", { class: "inline-form" });
  form.append(nameInput, createButton, errorLine);
  createPanel.append(el("h2", { text: "Create a project" }), form);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    errorLine.classList.add("hidden");
    createButton.disabled = true;
    try {
      const created = await api.post("/api/projects", { name: nameInput.value.trim() });
      location.hash = `#/projects/${created.project_id}`;
    } catch (error) {
      errorLine.textContent = error.message;
      errorLine.classList.remove("hidden");
    } finally {
      createButton.disabled = false;
    }
  });
  main.append(createPanel);

  const listPanel = el("section", { class: "panel" });
  listPanel.append(el("h2", { text: "Projects you can access" }));
  if (projects.length === 0) {
    listPanel.append(el("p", { class: "muted", text: "No projects yet. Create one above — you become its owner." }));
    main.append(listPanel);
    return;
  }
  const table = el("table", { class: "data" });
  const head = el("tr", {});
  for (const label of ["Name", "Status", "Role", "Created"]) {
    head.append(el("th", { text: label }));
  }
  table.append(head);
  for (const project of projects) {
    const row = el("tr", {});
    row.append(
      el("td", {}, el("a", { href: `#/projects/${project.project_id}`, text: project.name })),
      el("td", {}, el("span", { class: `badge project-${project.status}`, text: project.status })),
      el("td", { text: project.role ?? "—" }),
      el("td", { text: new Date(project.created_at_ms).toLocaleString() }),
    );
    table.append(row);
  }
  listPanel.append(table);
  main.append(listPanel);
}
