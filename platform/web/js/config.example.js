// Vortyx web console — runtime configuration TEMPLATE.
//
// Copy this file to js/config.js and fill in the values. Both values are
// PUBLISHABLE BY DESIGN (the anon key is public; data safety comes from
// Row Level Security, never from key secrecy). A service-role key, a worker
// token or any other secret must NEVER be placed here.
//
// apiBaseUrl: "" = the console is served from the same origin as the API
// (the documented single-project Vercel setup or the local dev server).
// Otherwise use the API project's absolute origin (with CORS configured
// through VORTYX_ALLOWED_ORIGIN on the API).

window.VORTYX_CONFIG = {
  supabaseUrl: "https://YOUR-PROJECT.supabase.co",
  supabaseAnonKey: "YOUR-PUBLISHABLE-ANON-KEY",
  apiBaseUrl: "",
};
