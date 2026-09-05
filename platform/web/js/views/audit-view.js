// Audit — the caller's own audit events (the safe default scope, the same
// rule as the C++ service). Project-scoped audit lives on the project's
// Audit tab for admins/owners.

import { el, clear, formatTime, renderLoading, renderEmpty, renderError } from "../ui.js";
import { auditTable } from "./project-view.js";

export async function renderAudit(main, api) {
  renderLoading(main, "Loading audit…");
  let events;
  try {
    const result = await api.get("/api/audit?limit=100");
    events = result.events ?? [];
  } catch (error) {
    renderError(main, error, "#/audit");
    return;
  }

  clear(main);
  main.append(el("h1", { text: "Audit" }));
  main.append(el("p", { class: "muted", text: "Your own events (the safe default scope). Project admins see project-wide events on each project's Audit tab. Events carry metadata only — never secrets or payloads." }));

  if (events.length === 0) {
    renderEmpty(main, "No events", "You have not performed any audited actions yet.");
    return;
  }
  main.append(auditTable(events));
}
