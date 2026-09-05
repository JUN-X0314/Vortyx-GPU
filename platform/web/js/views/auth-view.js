// Auth screens (login + signup) — real Supabase Auth REST calls.

import { signIn, signUp, SessionExpiredError } from "../auth.js";
import { el, clear } from "../ui.js";

function authLayout(title, form, notice) {
  const root = document.getElementById("app");
  clear(root);
  const wrap = el("div", { class: "auth" });
  const card = el("div", { class: "auth-card" });
  card.append(el("h1", { class: "brand big", text: "Vortyx" }), el("h2", { text: title }));
  if (notice !== null && notice !== undefined && notice.length > 0) {
    card.append(el("p", { class: "notice", text: notice }));
  }
  card.append(form);
  root.append(wrap);
  return card;
}

function errorSlot() {
  const slot = el("p", { class: "form-error hidden" });
  return {
    node: slot,
    show(message) {
      slot.textContent = message;
      slot.classList.remove("hidden");
    },
  };
}

function field(labelText, input) {
  const wrap = el("label", { class: "field" });
  wrap.append(el("span", { text: labelText }), input);
  return wrap;
}

function submitButton(label) {
  const button = el("button", { type: "submit", class: "primary", text: label });
  return button;
}

export function renderLogin(root, authConfig, onSignedIn) {
  const email = el("input", { type: "email", name: "email", required: "required", autocomplete: "username" });
  const password = el("input", { type: "password", name: "password", required: "required", autocomplete: "current-password" });
  const errors = errorSlot();
  const button = submitButton("Sign in");

  const form = el("form", { class: "auth-form" });
  form.append(field("Email", email), field("Password", password), errors.node, button);

  const switcher = el("p", { class: "switch" });
  const link = el("a", { href: "#/signup", text: "Create an account" });
  switcher.append("No account yet? ", link);

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    errors.node.classList.add("hidden");
    button.disabled = true;
    try {
      const session = await signIn(authConfig, email.value.trim(), password.value);
      onSignedIn(session);
    } catch (error) {
      errors.show(error instanceof SessionExpiredError ? error.message : error.message);
    } finally {
      button.disabled = false;
    }
  });

  const card = authLayout("Sign in", form);
  card.append(switcher);
}

export function renderSignup(root, authConfig, onSignedIn) {
  const email = el("input", { type: "email", name: "email", required: "required", autocomplete: "username" });
  const password = el("input", { type: "password", name: "password", required: "required", minlength: "8", autocomplete: "new-password" });
  const errors = errorSlot();
  const button = submitButton("Create account");

  const form = el("form", { class: "auth-form" });
  form.append(
    field("Email", email),
    field("Password (8+ characters)", password),
    errors.node,
    button,
  );

  const switcher = el("p", { class: "switch" });
  switcher.append("Already registered? ", el("a", { href: "#/login", text: "Sign in" }));

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    errors.node.classList.add("hidden");
    button.disabled = true;
    try {
      const session = await signUp(authConfig, email.value.trim(), password.value);
      onSignedIn(session);
    } catch (error) {
      errors.show(error.message);
    } finally {
      button.disabled = false;
    }
  });

  const card = authLayout("Create your account", form);
  card.append(switcher);
}
