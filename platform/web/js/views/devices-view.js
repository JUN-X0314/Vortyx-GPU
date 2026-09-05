// Devices — the REAL device registry (the caller's own devices from the
// Phase 11 surface). No GPU telemetry exists in the control plane and none
// is invented: what you see is registration state, self-reported capacity
// configuration and liveness.

import { el, clear, formatTime, renderLoading, renderError, renderEmpty } from "../ui.js";

export async function renderDevices(main, api) {
  renderLoading(main, "Loading devices…");
  let devices;
  try {
    const result = await api.get("/api/devices");
    devices = result.devices ?? [];
  } catch (error) {
    renderError(main, error, "#/devices");
    return;
  }

  clear(main);
  main.append(el("h1", { text: "Devices" }));
  main.append(el("p", { class: "muted", text: "Your registered Vortyx nodes. Hardware utilization is not measured by the control plane and is therefore not shown — capacity values are the device's self-reported configuration, never a measurement." }));

  if (devices.length === 0) {
    renderEmpty(main, "No devices", "No Vortyx node has been registered under your account.");
    return;
  }

  const table = el("table", { class: "data" });
  const head = el("tr", {});
  for (const label of ["Device", "Name", "Status", "Backends", "Operations", "Last seen"]) {
    head.append(el("th", { text: label }));
  }
  table.append(head);
  for (const device of devices) {
    const row = el("tr", {});
    row.append(
      el("td", { class: "mono", text: device.device_id }),
      el("td", { text: device.metadata.display_name }),
      el("td", {}, el("span", { class: `badge status-${device.status}`, text: device.status })),
      el("td", { text: device.metadata.backends.join(", ") || "—" }),
      el("td", { text: device.metadata.operations.join(", ") || "—" }),
      el("td", { text: formatTime(device.last_seen_ms) }),
    );
    table.append(row);
  }
  main.append(table);
}
