# The Native Worker — Deployment

Where does the C++ worker run in a "production-oriented" deployment? Read
this before promising anything: Vortyx does NOT auto-provision workers on
any cloud, and the control plane NEVER fakes execution when none is
connected.

## The deployment shape

```
[Vercel]  platform/web (static)  ── same-origin or CORS ──>  platform/api (functions)
                                                                    │  worker protocol
                                                                    │  (plain http/1.1 + token)
[any host you control]  vortyx_worker_agent ────────────────────────┘
                        (local simulated devices, real Phase 12 execution)
```

## Supported modes (all real, all documented)

1. **Local worker mode** — control plane and worker on one machine (the
   dev-server or a self-hosted Node process). The default development and
   verification path.
2. **Self-hosted worker mode** — the worker runs on a machine that can reach
   the API. The C++ core speaks **plain HTTP/1.1 only**; choose ONE:
   - run both inside a trusted network segment (VPN / private subnet), or
   - put a reverse proxy with TLS in front of the API and point the worker's
     `VORTYX_WORKER_ENDPOINT` at a plain-`http` listener it exposes on the
     trusted side.
   An `https://` endpoint is REFUSED by the agent at configuration time —
   the refusal is the security feature (never a silent downgrade).

## Not supported (and not claimed)

- Long-running C++ workers on Vercel. The control plane's functions own the
  lifecycle only; execution lives elsewhere by design.
- Automatic worker provisioning, autoscaling, or any cloud marketplace.
- GPU execution on a cloud provider: the current agent runs the local
  simulated devices (real compute, real Phase 12 scheduling, honest `cpu`
  backend reporting). Real hardware deployment is future work and will be
  documented when it exists.

## Operations

- **Scale**: run more `vortyx_worker_agent` processes (distinct
  `VORTYX_WORKER_ID`s). Claims are atomic; two agents can never hold one job.
- **Liveness**: each agent heartbeats its claim; the API reconciles expired
  leases to `failed("worker_lease_expired")` (triggered by claims and by
  `/api/internal/reconcile`; on Vercel, set `CRON_SECRET` and a cron entry to
  call it periodically — Vercel sends it as the Authorization bearer).
- **Cancellation**: a user/admin cancel sets `cancel_requested`; the agent's
  next heartbeat relays it into the executing record and the orchestrator
  cancels at the next wave boundary. A queued job cancels immediately in the
  control plane.
- **Secrets**: the worker token lives in the agent's environment and the
  API's server environment only. It is never a user identity and never
  grants control-plane reads — worker endpoints can claim/complete, nothing
  else.
