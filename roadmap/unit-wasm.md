# FreeUnit WebAssembly — State & Roadmap

WebAssembly is the most strategically interesting capability in Unit's tree. It's the one feature where FreeUnit can lead rather than catch up — most app servers still treat WASM as a curiosity; Unit already ships two WASM backends, one of them on the WASI 0.2 component model. The runtime ecosystem (Wasmtime, Wasmer, WasmEdge) is moving fast, WASI Preview 3 is coming, and the language-target list keeps growing (PHP-wasm, CPython-WASI, ruby.wasm, Go, Rust, C#, Swift). This doc captures the current state and argues for an aggressive roadmap.

## Current state

Unit ships **two** independent WASM backends, with different philosophies:

### 1. Core WASM SAPI — `src/wasm/`

| File | LoC | Purpose |
|---|---|---|
| `src/wasm/nxt_wasm.h` | 143 | Shared types: request/response structs, hook enum, ops vtable |
| `src/wasm/nxt_wasm.c` | 315 | Hook dispatch, request/response marshalling, config |
| `src/wasm/nxt_rt_wasmtime.c` | 439 | Wasmtime C API backend |

- **Custom Unit ABI.** Guest modules must export specific functions: `nxt_wasm_malloc`, `nxt_wasm_free`, `nxt_wasm_request_handler`, plus optional lifecycle hooks (`module_init`, `module_end`, `request_init`, `request_end`, `response_end`) — see `nxt_wasm_fh_e` enum in `nxt_wasm.h:90`.
- **Request layout** is a packed C struct (`nxt_wasm_request_t`) copied into a linear-memory arena at a fixed offset, fields addressed by `(off, len)` pairs.
- **Runtime:** Wasmtime via its C API (`libwasmtime.so`), abstracted behind a `nxt_wasm_operations_t` vtable (`init`, `destroy`, `exec_request`, `exec_hook`) so alternative runtimes could plug in — but only Wasmtime is implemented.
- **Memory:** 32 MiB linear memory, 64 KiB pages.
- **TLS flag** is passed but body streaming is not.
- **Config:** `type: "wasm"`, `module: "…/foo.wasm"`, optional `access: { filesystem: [...] }` for WASI dir mappings.

This backend is what was originally merged upstream. It's functional but requires guests to implement Unit-specific exports — not portable.

### 2. WASI Component Model backend — `src/wasm-wasi-component/`

| File | LoC | Purpose |
|---|---|---|
| `src/wasm-wasi-component/src/lib.rs` | 610 | Full implementation |
| `src/wasm-wasi-component/Cargo.toml` | 33 | Rust crate — built as cdylib |
| `build.rs` + `wrapper.h` | — | bindgen glue to libunit's C ABI |

- **Standards-based.** Uses the WASI 0.2 HTTP interface (`wasi:http/incoming-handler`) — any component that implements the interface just works. No Unit-specific ABI required.
- **Runtime:** wasmtime 35.0.0 (+ `component-model` + `cranelift`), `wasmtime-wasi 35`, `wasmtime-wasi-http 35`.
- **Crate type:** `cdylib` — loaded by libunit as a dynamic module.
- **Async pipeline:** Rust/Tokio internally, but the libunit body API is sync — body streaming awkward (see TODOs below).
- **Config:** `type: "wasm-wasi-component"`, `component: "…/foo.wasm"`, `access: { filesystem: [...] }`.

### Runtime stack

- **Wasmtime version** — `Cargo.toml` pins 35.0.0; upstream `pkg/contrib/src/wasmtime/version` was recently bumped to 43.0.1 in commit `925d6626` for the Docker image. **Version skew** between the Rust crate and the packaged C library is real; should be reconciled.
- **WebAssembly standards implemented:** WASI 0.2 (aka "Preview 2"). WASI 0.3 (async-native) is in draft; WASI Preview 1 is legacy, used by the core SAPI implicitly.
- **Docker:** `ghcr.io/freeunitorg/freeunit:latest-wasm` ships both backends.
- **CI:** no dedicated wasm CI matrix beyond the Docker build workflow.

### Known TODOs / gaps (from `unit-todos.md`)

All in `src/wasm-wasi-component/src/lib.rs`:
- `:382` — convert request/response body into a Stream to become async
- `:389` — partial reads not supported
- `:390` — how to make this async at the nxt level (libunit body API is sync)
- `:439` — HTTP trailers: what to do with them?
- `:450` — `unsafe` block with an unresolved safety question
- `:523` — handle failure when read `amt` is negative
- `:65` — dead-code question
- `test/test_wasm-wasi-component.py` exists but coverage is thin

### Why this matters

1. **Language-agnostic SAPI.** Every future language — PHP-wasm (`php-wasm`), CPython-WASI, ruby.wasm, Tenko for Node, .NET 9's WASI target, Swift, Java via TeaVM — lands "for free" the moment the component model absorbs them. Each new language today requires a new `src/<lang>/` module plus a bespoke `auto/modules/<lang>` script plus Docker variants plus CI. WASM collapses that into "point at a `.wasm` file."
2. **Per-request isolation nearly free.** Component instantiation is microseconds; linear-memory isolation is hardware-fast. Unit's process/cgroup/namespace isolation is orders of magnitude heavier. For untrusted code (multi-tenant SaaS, plugin systems), WASM is the only practical answer.
3. **Capability-based security.** WASI only grants what config explicitly maps: no filesystem, no network, no clock unless allowed. This is stronger than seccomp, and declarative in config.
4. **Deterministic performance.** No GC, no JIT warmup after a few requests, no opcache to prime — AOT-compiled bytecode runs at native-adjacent speed from request one.
5. **Edge / CDN deployment posture.** Fastly (Compute@Edge), Cloudflare (Workers), Shopify (Oxygen), Fermyon (Spin) — the entire edge-compute industry bet on WASM. FreeUnit can serve the same workloads at origin, not just edge, and be the natural migration target.

---

## Roadmap

### Near term (1–3 months)

**W1. Reconcile wasmtime versions across the tree.**
- Bump `Cargo.toml` from wasmtime 35 → 43.0.1 to match the packaged C library.
- Test across the supported OS matrix; cranelift ABI changes between majors occasionally.
- **Effort:** 3 days if no API breakage; up to 1 week if cranelift-codegen changed.

**W2. Async body streaming for the component backend.**
- Drains the largest TODO cluster (`lib.rs:382,389,390`). Requires libunit body API to go async, which is the same change `unit-roadmap.md` D3 needs for HTTP/2 work — **co-design these two**.
- Add backpressure: today a large upload into a slow wasm component buffers unboundedly.
- **Wins:** streaming uploads, SSE/chunked responses, long-polling — all currently broken or pathological.
- **Effort:** ~3–4 weeks including the libunit ABI change (coordinated with D3).

**W3. HTTP trailers.**
- Plumb `wasi:http` trailers through the component handler (`lib.rs:439`) — currently dropped.
- Needed for gRPC-over-HTTP and for any well-behaved trailer-using client.
- **Effort:** ~1 week.

**W4. CI matrix for WASM.**
- Add a `{wasmtime 35, wasmtime 43, wasmtime head} × {core SAPI, component backend} × {linux/amd64, linux/arm64}` matrix.
- Run the WASI component-conformance test suite (Bytecode Alliance has one).
- **Effort:** ~1 week.

### Mid term (3–6 months)

**W5. Runtime abstraction — Wasmer and WasmEdge backends.**
- The core SAPI already has a vtable (`nxt_wasm_operations_t`) for alternative runtimes. Nothing fills the slot. Add:
  - `nxt_rt_wasmer.c` against Wasmer's C API.
  - `nxt_rt_wasmedge.c` against WasmEdge's C API.
- `./configure wasm --runtime=wasmer` selects the backend.
- **Wins:** runtime-choice by workload (WasmEdge is tuned for AI inference via wasi-nn; Wasmer has faster AOT); hedge against Wasmtime licensing/vendor changes.
- **Effort:** ~3 weeks each. Ship one first, evaluate demand before the second.

**W6. wasi-nn support — GPU / ML inference.**
- Map wasi-nn imports so components can call CPU/GPU inference runtimes (ONNX, PyTorch, TensorFlow Lite) via config-declared backends.
- **Wins:** a single FreeUnit instance can host web + AI inference; kills the "sidecar an inference server" pattern for small models.
- **Effort:** ~4 weeks. Build-feature-gate behind `--wasm-wasi-nn`.

**W7. wasi-keyvalue / wasi-sqlite.**
- Mount a KV store or embedded SQLite as WASI imports, configured per-app.
- Declarative storage for wasm components without bundling a DB driver.
- **Effort:** ~2 weeks.

**W8. WASM scheduler tasks.**
- Integrates with the scheduler primitive from `unit-cron.md`. `type: "wasm-wasi-component"` schedules run as single-invocation component instantiations.
- **Cold-start win is massive:** µs instantiation means "every-second" crons are cheap; language-neutral so ops scripts can be written in Rust or Go without per-language baggage.
- **Effort:** minimal once the scheduler primitive lands — mostly config plumbing.

**W9. Deprecate or consolidate the core WASM SAPI.**
- `src/wasm/` uses a custom ABI that nobody writes guests for outside of Unit docs. The component backend is strictly better for new users.
- Options:
  1. Hard-deprecate core SAPI after one release cycle. Keep component backend only.
  2. Reimplement core-SAPI semantics as a thin shim on top of wasmtime-wasi-http (same runtime, different entry ABI).
- Option 2 keeps backwards compat without the maintenance cost of a separate backend.
- **Effort:** ~1 week for deprecation notice; ~3 weeks for the shim.

### Long term (6–12 months)

**W10. WASI Preview 3 adoption (async-native).**
- Preview 3 makes async a first-class part of the ABI. Eliminates W2's friction: streaming body flows naturally.
- Track Bytecode Alliance timeline; land support behind a feature flag when Wasmtime ships stable P3.
- **Effort:** ~6 weeks once upstream stabilizes.

**W11. Component composition at config time.**
- Config: `"components": [{ "path": "auth.wasm", "exports": "wasi:http" }, { "path": "app.wasm", "imports_from": "auth" }]` — Unit composes them into one runtime graph at load.
- Unlocks plugin architectures (auth middleware, rate-limit middleware, observability middleware) as discrete components.
- **Wins:** the "middleware as microservice, without the microservice tax" play.
- **Effort:** ~6 weeks.

**W12. Language presets: PHP-wasm, CPython-WASI, ruby.wasm.**
- Pre-packaged WASI components that embed PHP/Python/Ruby and evaluate a user-supplied script.
- Config: `type: "php-wasm"`, `script: "./index.php"` → Unit downloads/caches the PHP-wasm runtime component and runs the user's code inside it.
- Replaces bespoke `src/php/`, `src/python/`, `src/ruby/` for *new* apps that want strict isolation. Doesn't kill the native SAPIs — they stay faster for trusted code.
- **Wins:** multi-tenant platform story becomes trivial; each tenant's code runs in its own component with capability-gated WASI.
- **Effort:** ~4 weeks per language, plus upstream coordination with PHP-wasm/CPython WASI maintainers.

**W13. Signed components + attestation.**
- Verify signatures on `.wasm` before loading (Sigstore, cosign).
- Attestation: emit SLSA provenance for each instantiated component into OpenTelemetry spans.
- **Wins:** supply-chain security story that native SAPIs can't match.
- **Effort:** ~3 weeks.

**W14. Component registry / OCI distribution.**
- Support `component: "oci://ghcr.io/org/foo:v1"` in config — Unit pulls, verifies, caches the component from an OCI registry (Wasm is OCI-distributable per the WebAssembly OCI Artifact Spec).
- Makes deployment look like `kubectl apply` for wasm: declarative, versioned, pull-by-digest.
- **Effort:** ~4 weeks.

**W15. wasi-http server mode (vs current client-handler mode).**
- Expose Unit itself as a wasi-http host that components can `wasi:http/outgoing-handler` against — lets components make outbound HTTP through Unit's own connection pool / TLS stack / observability.
- **Effort:** ~3 weeks.

---

## Short roadmap table

| # | Item | Effort | Ship window |
|---|------|--------|-------------|
| W1 | Reconcile wasmtime versions (35→43) | 3d–1w | Near |
| W2 | Async body streaming (with D3) | 3–4w | Near |
| W3 | HTTP trailers | 1w | Near |
| W4 | CI matrix + component conformance | 1w | Near |
| W5 | Runtime abstraction (Wasmer, WasmEdge) | 3w each | Mid |
| W6 | wasi-nn (GPU/ML inference) | 4w | Mid |
| W7 | wasi-keyvalue / wasi-sqlite | 2w | Mid |
| W8 | WASM scheduler tasks | trivial after cron | Mid |
| W9 | Deprecate/consolidate core SAPI | 1–3w | Mid |
| W10 | WASI Preview 3 | 6w (upstream-gated) | Long |
| W11 | Component composition | 6w | Long |
| W12 | PHP-wasm / CPython-WASI / ruby.wasm presets | 4w each | Long |
| W13 | Signed components + attestation | 3w | Long |
| W14 | OCI component distribution | 4w | Long |
| W15 | wasi-http server mode | 3w | Long |

**Headline bets:** W2+W8 (async streaming + wasm scheduler) unblock near-term workload coverage; W11+W12 (component composition + language presets) are the positioning bets that make FreeUnit the obvious host for the next decade of polyglot workloads.

---

## Why wasm over native SAPI, eventually

The pattern across the language-specific roadmaps (`unit-php.md`, `unit-python.md`, `unit-ruby.md`) is the same set of features shipped three times: threads, persistent workers, preload, status, graceful reload, scheduler. Each native SAPI carries a perpetual maintenance tax — PHP version guards, Python version guards, Ruby multiarch bugs, C-extension compatibility warnings, ABI drift with libphp / libpython / libruby.

The WASM component model gets FreeUnit out of that tax for *new* applications:

- One host implementation instead of 8 language modules.
- One security model (capability-based WASI) instead of 8 isolation stories (PHP open_basedir, Python sys.path sandbox, Ruby tainting, Node permission model, etc.).
- One distribution channel (OCI wasm) instead of 8 packaging stories (Composer, pip, bundler, npm, CPAN, go modules, maven, cargo).
- One observability shape (span per component invocation) instead of 8 language-specific probes.

The native SAPIs remain the **fast path for trusted code on one language per app**. WASM is the **default path for multi-tenant, polyglot, or supply-chain-sensitive workloads** — and that superset is growing. Position accordingly.

---

## Integration with other roadmap docs

- `unit-roadmap.md` D3 (body streaming) is a **prerequisite** for W2.
- `unit-roadmap.md` X5 (scheduler primitive) is a **prerequisite** for W8.
- `unit-roadmap.md` X2 (unified status API) should absorb wasm stats (components loaded, instantiation count, avg duration, linear memory high-water) into the `runtime` subtree.
- `unit-roadmap.md` G5 (package distribution) naturally extends to W14 (OCI component distribution).
- `unit-todos.md` pattern F is exactly W2 — same fix, same PR.
