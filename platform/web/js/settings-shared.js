// Shared settings helpers (kept out of the view so tests can pin them).

import { signOut } from "./auth.js";

export function signOutAndLeave(authConfig, session) {
  signOut(authConfig, session).finally(() => {
    location.hash = "#/login";
  });
}
