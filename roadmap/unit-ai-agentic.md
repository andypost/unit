# FreeUnit for AI-Agentic Workloads — Roadmap

Complementary track to [unit-roadmap.md](unit-roadmap.md) and [unit-wasm.md](unit-wasm.md). It does **not** introduce new primitives. It reprioritizes already-scoped items (`X*`, `D*`, `W*`) into a predictable quarterly schedule aimed at a concrete class of workload: **AI agents and their tools**.

---

## Why a separate document

Two roadmap documents already coexist in this directory and they tell different stories:

1. **Public roadmap** ([unit-roadmap.md](unit-roadmap.md), [unit-maintainer.md](unit-maintainer.md)) — feature-oriented, LTS-driven, 12-month. *Design shared primitives once (preload, status, reload, persistent-worker, scheduler, OTel, metrics); let PHP / Python / Ruby SAPIs plug in.* Positioning: "the last NGINX Unit you'll ever need."
2. **Telegram-derived roadmap** ([maintainer-from-telegram.md](maintainer-from-telegram.md)) — substrate-oriented, from the maintainer's voice messages. *The polyglot "many apps in one Unit" idea conflicts with Docker-era packaging; the real unique bet is hidden async at the runtime level + WASM as the ultimate isolation substrate, aimed at serverless/platform vendors.*

### The delta

| Axis | Public roadmap | Telegram roadmap |
|---|---|---|
| Unit of shipping | Features across 3 SAPIs | A runtime substrate |
| Primary audience | PHP/Python/Ruby operators, LTS users | Serverless, multi-tenant, platform providers |
| Language emphasis | Parity across PHP + Python + Ruby | PHP is the only "meaningful" innovation target; skeptical of value for Go/Rust/Node |
| Async story | Per-language (ZTS, free-threaded, Fibers) | Hidden async at runtime level; app code stays synchronous |
| WASM role | One track among many (W1–W15) | The "ultimate" direction — shared event loop, memory reset, strong isolation |
| Performance goal | Lower latency + parity features | Throughput over latency; "process more, not faster" |
| Big open question | Sequencing | *Where should FreeUnit actually go?* (unresolved by design) |

Neither roadmap directly addresses **AI-agentic workloads** — the fastest-growing class of server-side work in 2026. Agents execute untrusted tool calls, run short-lived sandboxed code, pull model components on demand, and need introspection, replay, capability gating, and tight cold-start latencies. This document is the bridge: it surfaces the delta so future contributors stop relitigating it, and it proposes a parallel AI-agentic stream that leans into the substrate bets from the Telegram vision while reusing the primitives already committed in the public roadmap.

---

## Positioning

> FreeUnit as an **AI-agent runtime**: one host process that can safely run trusted native code (PHP / Python / Ruby / Node / Go), untrusted WASM agent tools with capability-gated access, scheduled agent jobs, and co-located inference — with one control plane, one observability surface, one distribution channel (OCI), and µs-scale per-request isolation.
>
> FreeUnit does **not** compete with LangChain / Temporal / Ray at the orchestration layer; it replaces the `docker exec + cron + sidecar-inference + nginx + fpm` stack underneath them.

This framing is compatible with the Telegram vision's serverless/platform-provider audience — AI-agent hosts are a subset of that audience — and compatible with the public roadmap's LTS posture, because every item it requires is already scoped there.

---

## Unit's unique value for AI-agentic usage

Each AI-agentic need below maps to an existing primitive; nothing in this roadmap is net-new scope.

| AI-agentic need | Unit primitive it maps to | Source |
|---|---|---|
| Run untrusted agent-generated code | WASI 0.2 component backend, per-request isolation | [unit-wasm.md](unit-wasm.md) §Current state #2 |
| µs-scale cold start for tool calls | Component instantiation via wasmtime | [unit-wasm.md](unit-wasm.md) §Why this matters |
| Capability-gated filesystem / net / clock | WASI `access:` config | `src/wasm-wasi-component/src/lib.rs` |
| Live reconfiguration by a controller agent | RESTful JSON control API + JSON Patch | [unit-roadmap.md](unit-roadmap.md) D6 |
| Recurring agent jobs (polling, retries) | Scheduler primitive + WASM scheduler tasks | [unit-roadmap.md](unit-roadmap.md) X5, [unit-wasm.md](unit-wasm.md) W8 |
| Co-locate inference with HTTP handlers | `wasi-nn` import | [unit-wasm.md](unit-wasm.md) W6 |
| Composable guardrails (auth, rate-limit, PII filter) | Component composition at config time | [unit-wasm.md](unit-wasm.md) W11 |
| Pull agent tools on demand | OCI component distribution | [unit-wasm.md](unit-wasm.md) W14 |
| Outbound HTTP through one observable pool | `wasi-http` server mode | [unit-wasm.md](unit-wasm.md) W15 |
| Replayable traces of agent actions | OTel span conventions + structured logs | [unit-roadmap.md](unit-roadmap.md) X7, D8 |
| Supply-chain trust for agent bundles | Signed components + SLSA attestation | [unit-wasm.md](unit-wasm.md) W13 |
| Per-tenant KV / SQLite without bundling drivers | `wasi-keyvalue` / `wasi-sqlite` | [unit-wasm.md](unit-wasm.md) W7 |
| Long-lived Python agent loop holding model state | Persistent-worker contract | [unit-roadmap.md](unit-roadmap.md) X4 |
| Memory reset between agent invocations | WASM linear-memory per-instance | [maintainer-from-telegram.md](maintainer-from-telegram.md) §6 |

---

## Predictable schedule

Quarterly milestones with one rule: **each quarter delivers at least one demo-able AI-agentic capability end-to-end, even if deeper items slip.** Items reference existing IDs in other roadmap docs — track them there, not here.

### Q1 — Foundation

**Exit criterion:** a Python- or Rust-compiled WASM component can handle HTTP requests with streaming bodies, declared filesystem capabilities, and OTel spans.

- `W1` wasmtime version reconciliation (35 → 43) — [unit-wasm.md](unit-wasm.md)
- `W2` async body streaming for component backend — co-designed with `D3` body streaming
- `W3` HTTP trailers (needed for gRPC agent transports)
- `X7` OTel span conventions (`unit.request`, `unit.worker.lifecycle`)
- `D8` structured JSON logs with `request_id`
- `D5` better config validation errors (agents generate config — errors must be machine-actionable)

### Q2 — Agent tool-call substrate

**Exit criterion:** an agent can register a set of WASM tools via JSON API, execute them per-request or on a schedule, and query status / metrics.

- `X5` scheduler primitive, phases 1 + 2
- `W8` WASM scheduler tasks (µs cold-start makes sub-minute cron cheap)
- `X2` unified status API with a WASM `runtime` subtree
- `X8` Prometheus metrics endpoint
- `D6` JSON Patch / Merge Patch on the control API (agents mutate config incrementally, not via full PUT)
- `D7` control API token auth (required before *any* controller-agent story is safe on a network socket)

### Q3 — Capability + supply-chain trust

**Exit criterion:** a signed, OCI-distributed WASM tool bundle can be pulled, verified, and composed with guardrail components at config time.

- `W7` `wasi-keyvalue` / `wasi-sqlite` (per-tenant agent memory without bundled drivers)
- `W11` component composition at config time (guardrails as discrete components: auth, rate-limit, PII filter)
- `W13` signed components + SLSA attestation (supply-chain trust for third-party agent tools)
- `W14` OCI component distribution (`component: "oci://ghcr.io/org/foo:v1"`)
- `X3` graceful reload so pulled components hot-swap cleanly

### Q4 — Inference + outbound + presets

**Exit criterion:** a single FreeUnit instance hosts a Python/PHP app, a WASM agent-tool bundle, a small local inference task via `wasi-nn`, and a `wasi-http` outbound pool — all visible in one status / metrics / OTel surface.

- `W6` `wasi-nn` (CPU/GPU inference inline with HTTP handlers)
- `W15` `wasi-http` server mode for outbound (agent HTTP calls go through Unit's connection pool / TLS / observability)
- `W12` language presets (PHP-wasm / CPython-WASI / ruby.wasm) — scoped as *agent-tool targets*, not as replacements for native SAPIs
- `X4` persistent-worker contract — lets a long-lived Python agent loop hold model state across requests

---

## Predictability guarantees

1. **No new IDs.** This roadmap only references `X*` / `D*` / `W*` items already scoped in [unit-roadmap.md](unit-roadmap.md) and [unit-wasm.md](unit-wasm.md). If an item isn't there, it isn't committed here.
2. **Quarter-shaped milestones with exit criteria.** Individual items may slip; the quarter ships when its exit criterion is demonstrable end-to-end.
3. **Parallel, not displacing.** PHP / Python / Ruby tracks proceed as planned. The AI-agentic stream reuses their shared-primitive outputs (`X1`, `X2`, `X3`, `X4`, `X5`, `X7`, `X8`).
4. **No framework opinions.** FreeUnit is the runtime below agent orchestrators (LangChain, LlamaIndex, Temporal, Ray, Haystack). It doesn't adopt any of them into the daemon.

---

## Out of scope

- Agent orchestration itself (graphs, planners, memory stores, tool-call routing) — FreeUnit is the runtime below those.
- Changes to native SAPI priorities in [unit-php.md](unit-php.md), [unit-python.md](unit-python.md), [unit-ruby.md](unit-ruby.md).
- New primitives not already in [unit-roadmap.md](unit-roadmap.md) or [unit-wasm.md](unit-wasm.md).
- Vendor-specific model integrations (OpenAI, Anthropic, etc.). `wasi-nn` + `wasi-http` cover the substrate; anything model-specific belongs in a component, not in the daemon.

---

## How this fits the two existing roadmaps

- From the **public roadmap**: this document is a prioritization lens, not a commitment surface. It pulls a specific subset of `X*` / `D*` / `W*` items forward because they compound for AI-agent hosts.
- From the **Telegram roadmap**: this document makes the substrate bet concrete without forcing a pivot away from the polyglot SAPI story. The AI-agent audience is the narrowest *useful* interpretation of "serverless / platform providers" — narrow enough to ship against, broad enough to matter.
