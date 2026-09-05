// DOM helpers (Phase 15 web console) — tiny, dependency-free.

/**
 * Creates one element. attrs.class / attrs.text / attrs.href are the common
 * ones; every other attribute is set verbatim.
 * @param {string} tag
 * @param {Record<string, string>=} attrs
 * @param {...(Node|string)} children
 * @returns {HTMLElement}
 */
export function el(tag, attrs, ...children) {
  const node = document.createElement(tag);
  if (attrs !== undefined) {
    for (const [key, value] of Object.entries(attrs)) {
      if (key === "class") node.className = value;
      else if (key === "text") node.textContent = value;
      else node.setAttribute(key, value);
    }
  }
  for (const child of children) {
    if (typeof child === "string") node.append(document.createTextNode(child));
    else if (child !== null && child !== undefined) node.append(child);
  }
  return node;
}

/** Removes every child. */
export function clear(node) {
  while (node.firstChild !== null) node.removeChild(node.firstChild);
}

/** Formats epoch milliseconds in the VIEWER's timezone (the API ships ms). */
export function formatTime(ms) {
  if (ms === null || ms === undefined) return "—";
  return new Date(Number(ms)).toLocaleString();
}

/** Formats bytes honestly (no invented precision). */
export function formatBytes(bytes) {
  const value = Number(bytes);
  if (!Number.isFinite(value) || value < 0) return "—";
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let index = 0;
  let amount = value;
  while (amount >= 1024 && index < units.length - 1) {
    amount /= 1024;
    index += 1;
  }
  const rounded = index === 0 ? amount : amount.toFixed(1);
  return `${rounded} ${units[index]}`;
}

/**
 * The job-status badge vocabulary — every state the API can report has an
 * explicit visual (queued/running/completed/failed/cancelled are never
 * conflated).
 */
export function statusBadge(status) {
  return el("span", { class: `badge status-${status}`, text: status });
}

// ---------------------------------------------------------------------------
// Shared state renderers (loading / empty / error) — the honest states every
// screen shows while it works, when it has nothing, and when it fails.
// ---------------------------------------------------------------------------

import { describeApiError, ApiError } from "./api.js";

/**
 * @param {HTMLElement} container
 * @param {unknown} error
 * @param {string=} retryHref
 */
export function renderError(container, error, retryHref) {
  const view = describeApiError(error);
  const box = el("div", { class: `state error kind-${view.kind}` });
  box.append(el("h2", { text: view.title }), el("p", { text: view.detail }));
  if (view.kind === "unauthorized") {
    const button = el("button", { text: "Sign in" });
    button.addEventListener("click", () => {
      location.hash = "#/login";
    });
    box.append(button);
  } else if (typeof retryHref === "string") {
    const button = el("button", { text: "Retry" });
    button.addEventListener("click", () => {
      location.hash = retryHref;
    });
    box.append(button);
  }
  clear(container);
  container.append(box);
}

export function renderLoading(container, label) {
  clear(container);
  container.append(el("div", { class: "state loading", text: label ?? "Loading…" }));
}

export function renderEmpty(container, title, detail) {
  clear(container);
  const box = el("div", { class: "state empty" });
  box.append(el("h2", { text: title }), el("p", { text: detail ?? "" }));
  container.append(box);
}
