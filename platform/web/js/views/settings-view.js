// Settings — what the console honestly knows about the account: the
// verified user id (what other admins paste to add you as a member), the
// email, and the session state. No fictional preferences.

import { el, clear } from "../ui.js";
import { signOutAndLeave } from "../settings-shared.js";

export async function renderSettings(main, api, session, authConfig) {
  clear(main);
  main.append(el("h1", { text: "Settings" }));

  const account = el("section", { class: "panel" });
  account.append(el("h2", { text: "Account" }));
  const list = el("dl", { class: "defs" });
  list.append(
    el("dt", { text: "Email" }),
    el("dd", { text: session.email === "" ? "—" : session.email }),
    el("dt", { text: "User ID" }),
    el("dd", { class: "mono", text: session.user_id }),
    el("dt", { text: "Session" }),
    el("dd", { text: `expires ${new Date(session.expires_at_ms).toLocaleString()}` }),
  );
  account.append(list);
  account.append(
    el("p", { class: "muted", text: "Give this User ID to a project admin so they can add you to a project. There is no user directory — the console never lets one user enumerate others." }),
  );

  const signoutButton = el("button", { class: "danger", text: "Sign out" });
  signoutButton.addEventListener("click", () => signOutAndLeave(authConfig, session));
  account.append(signoutButton);

  const about = el("section", { class: "panel" });
  about.append(el("h2", { text: "About this console" }));
  about.append(
    el("p", { class: "muted", text: "The console renders server state only. Execution is performed by native Vortyx workers running the real Phase 12 distributed stack; hardware telemetry that is not measured is never displayed." }),
  );

  main.append(account, about);
}
