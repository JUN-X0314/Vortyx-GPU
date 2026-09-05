// The Vortyx Platform Web Console — boot + hash router (plain ES module).
//
// Screens: login, signup, dashboard, projects, project (overview/members/
// jobs/artifacts/quota/audit), job detail, job submit, devices, audit,
// settings. Every screen renders REAL server state through the API client;
// loading / empty / error / unauthorized states are explicit — stale data
// is never dressed up as success, and the console never invents a number
// the API did not send.

import * as auth from "./auth.js";
import { ApiClient } from "./api.js";
import { renderLogin, renderSignup } from "./views/auth-view.js";
import { renderDashboard } from "./views/dashboard-view.js";
import { renderProjects } from "./views/projects-view.js";
import { renderProject } from "./views/project-view.js";
import { renderJobSubmit } from "./views/submit-view.js";
import { renderJobDetail } from "./views/job-view.js";
import { renderDevices } from "./views/devices-view.js";
import { renderAudit } from "./views/audit-view.js";
import { renderSettings } from "./views/settings-view.js";
import { el, clear, renderError, renderLoading, renderEmpty } from "./ui.js";

/** @type {object|null} the live session (restored on boot) */
let session = null;
/** @type {auth.AuthConfig|null} */
let authConfig = null;
/** @type {ApiClient|null} */
let api = null;

const apiConfig = {
  baseUrl: (globalThis.VORTYX_CONFIG && globalThis.VORTYX_CONFIG.apiBaseUrl) || "",
};

function onSessionLost() {
  session = null;
  location.hash = "#/login";
}

function bootApi() {
  api = new ApiClient(
    apiConfig,
    authConfig,
    async () => session,
    (refreshed) => {
      session = refreshed;
    },
    onSessionLost,
  );
}

// ---------------------------------------------------------------------------
// The authenticated app shell (top navigation + content area)
// ---------------------------------------------------------------------------

const NAV_ITEMS = [
  ["#/dashboard", "Dashboard"],
  ["#/projects", "Projects"],
  ["#/devices", "Devices"],
  ["#/audit", "Audit"],
  ["#/settings", "Settings"],
];

function shell(activeHash) {
  const root = document.getElementById("app");
  clear(root);
  const nav = el("nav", { class: "topnav" });
  const brand = el("a", { class: "brand", href: "#/dashboard", text: "Vortyx" });
  nav.append(brand);
  for (const [href, label] of NAV_ITEMS) {
    const link = el("a", { href, text: label });
    if (activeHash.startsWith(href)) link.classList.add("active");
    nav.append(link);
  }
  const who = el("span", { class: "who", text: session !== null ? session.email : "" });
  const signout = el("button", { class: "ghost", text: "Sign out" });
  signout.addEventListener("click", async () => {
    if (session !== null) await auth.signOut(authConfig, session);
    session = null;
    location.hash = "#/login";
  });
  nav.append(el("div", { class: "spacer" }), who, signout);
  const main = el("main", { class: "content", id: "content" });
  root.append(nav, main);
  return main;
}

export { el };

// ---------------------------------------------------------------------------
// The router
// ---------------------------------------------------------------------------

function parseHash() {
  const hash = location.hash === "" ? "#/dashboard" : location.hash;
  return hash.replace(/^#/, "");
}

async function route() {
  if (authConfig === null) {
    try {
      authConfig = auth.loadConfig();
    } catch (error) {
      const root = document.getElementById("app");
      clear(root);
      const box = el("div", { class: "state error" });
      box.append(el("h2", { text: "Configuration required" }), el("p", { text: error.message }));
      root.append(box);
      return;
    }
    bootApi();
  }

  if (session === null) {
    session = await auth.restoreSession(authConfig);
  }

  const path = parseHash();
  const publicRoutes = ["/login", "/signup"];
  if (session === null && !publicRoutes.includes(path)) {
    location.hash = "#/login";
    return;
  }
  if (session !== null && publicRoutes.includes(path)) {
    location.hash = "#/dashboard";
    return;
  }

  if (path === "/login") {
    renderLogin(document.getElementById("app"), authConfig, (s) => {
      session = s;
      location.hash = "#/dashboard";
    });
    return;
  }
  if (path === "/signup") {
    renderSignup(document.getElementById("app"), authConfig, (s) => {
      session = s;
      location.hash = "#/dashboard";
    });
    return;
  }

  // Everything below is the authenticated shell.
  const main = shell(path);
  const projectMatch = /^\/projects\/([^/]+)(\/(members|jobs|artifacts|quota|audit))?$/.exec(path);
  const submitMatch = /^\/projects\/([^/]+)\/submit$/.exec(path);
  const jobMatch = /^\/jobs\/([^/]+)$/.exec(path);

  try {
    if (path === "/dashboard") {
      await renderDashboard(main, api);
    } else if (path === "/projects") {
      await renderProjects(main, api);
    } else if (submitMatch !== null) {
      await renderJobSubmit(main, api, submitMatch[1]);
    } else if (projectMatch !== null) {
      await renderProject(main, api, projectMatch[1], projectMatch[3] ?? "overview");
    } else if (jobMatch !== null) {
      await renderJobDetail(main, api, jobMatch[1]);
    } else if (path === "/devices") {
      await renderDevices(main, api);
    } else if (path === "/audit") {
      await renderAudit(main, api);
    } else if (path === "/settings") {
      await renderSettings(main, api, session, authConfig);
    } else {
      renderEmpty(main, "Not found", "This screen does not exist.");
    }
  } catch (error) {
    renderError(main, error);
  }
}

window.addEventListener("hashchange", route);
route();
