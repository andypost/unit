# FreeUnit Roadmap

Consolidated technical roadmap for the FreeUnit fork. Groups work that's **shared across all language modules** so it's designed once, plus **core-daemon improvements** independent of any SAPI, plus **fork-governance items** that aren't code but define what "FreeUnit" means as a project distinct from upstream.

Per-language detail lives in:
- [unit-php.md](unit-php.md) — PHP SAPI, ZTS, persistent-worker, TrueAsync
- [unit-python.md](unit-python.md) — WSGI/ASGI, free-threaded 3.13t, subinterpreters
- [unit-ruby.md](unit-ruby.md) — Rack, threads, Fiber scheduler, Ractors
- [unit-cron.md](unit-cron.md) — scheduler/cron primitive (drush, Celery, Sidekiq)
- [unit-arm32.md](unit-arm32.md) — armv7 SIGBUS / alignment investigation

---

## Release status — 1.35.6 (2026-07, content-complete on `pre-1.35.6`)

_Dated note; revisit each release._ The **1.35.6** cycle is **content-complete
and fully merged to `pre-1.35.6`**. Only the release mechanics remain (see
_Security track_ below). It ships:

- **Security hardening** — the 14-vector audit remediation across the HTTP/
  WebSocket paths, the libunit shmem ABI, process isolation, TLS, the
  controller, and the language bindings (#85), plus the post-audit follow-ups,
  all merged: port-queue item-size cap (#88), `set_headers` control-char
  rejection (#90), kernel-PID IPC sender auth (#91), port-read OOM null-deref
  (#96), rootfs path normalization (#105), router→worker `fields[]` region +
  Java `InputStream.read` off/len bounds (#112), duplicate upstream
  Content-Length (#113), NUL/empty in config C-string values (#114), and mount
  onto the openat2-validated fd — closing #14's mount-destination TOCTOU (#115).
- **Engine** — symmetric idle/active connection tracking, prep for graceful
  drain, with `/status` connection accounting now exact on every close path
  (#111).
- **Lifecycle** — SIGQUIT graceful-quit plumbing (#107) + two-phase listener
  close (#108): the first shipped slices of the graceful-shutdown plan.
- **wasmtime 35 → 36.0.12** (#87) — clears the wasmtime sandbox-escape CVEs.
- **OpenTelemetry** 0.24 → 0.32 (semconv attributes, HTTP + gRPC transports).

**Post-audit hardening** — merged to `pre-1.35.6` as focused PRs:
- **#88** — bound the shared-memory port queue item size (stack-overflow fix).
- **#90** — reject control chars in `set_headers` names/values (response splitting).
- **#91** — authorize privileged IPC senders by kernel-validated PID (`SCM_CREDENTIALS`).
- **#95 / #105** — reject embedded-NUL and lexical-`/` rootfs and cgroup isolation paths.

**Wave-3 residual hardening** — merged to `pre-1.35.6`:
- **#112** — libunit request `fields[]` region + per-field sptr bounds and cached-index range-check; Java `InputStream.read()` off/len guard.
- **#113** — reject duplicate upstream `Content-Length` (response-smuggling desync).
- **#114** — reject empty / embedded-NUL config strings consumed as C strings (app options + pathname unix sockets).
- **#115** — mount rootfs destinations onto the openat2-validated fd (`/proc/self/fd`), completing #14's mount-destination vector.
- **#117 / #125** — close out **#116**: the same NUL/empty C-string guard extended to `access_log` path, TLS certificate names, and njs module paths (PR-A, #117), then to the templated `share`/`chroot` paths at request-resolution time (PR-B, #125); `index` intentionally excluded (static config value, no request-controlled component).
- **#123** — per-language negative test coverage for the #114/#117 C-string guards (java, njs, perl, php, tls, wasm-component), one case per module CI leg.
- **#129** — libunit Go-module port-fd **double-close race**: the Go SAPI dup'd the port fd then closed the original while libunit still owned it, and `nxt_unit_add_port()` ran its callback outside `lib->mutex` — a concurrent `port_release()` could close an already-closed (or reused) fd. Root-caused and fixed (Go keeps its own dup; an extra `port_use`/`port_release` brackets the callback). This closes the intermittent go-isolation `close(N): Bad file descriptor` CI flake ("Wave 2a").
- **#128** — permanent libunit close()-provenance diagnostic (names the failing call site and the fd's prior closer via a lock-published, wraparound-safe ticket table) so any future double-close self-locates.

Perf validated (A/B/C bench, `f2cd42e2` → wave → +#111): the whole wave costs
≈ +3 % router CPU/request only on the synthetic `return 200` hot path and
**+0.1 % on the PHP/IPC app path**; #111 itself is unmeasurable. No latency-tail
or error-rate change. Trade accepted for two CVE-class fixes.

### Security track — handled; feature work must not re-open it

**Security for 1.35.6 is owned end-to-end by the maintainer/security track and
is effectively done.** Agents doing feature or refactor work should treat the
audit + hardening as **complete** and must **not re-audit, re-open, or block on
these items** — the live working state is private (`plan-finish-vectors.md`),
not here. The only actions left, all security-track owned:

- Cut the release: `pre-1.35.6` → `master` (merge, **not** fast-forward — master
  carries CI-only commits), tag **1.35.6**.
- Publish the two draft GHSAs + request CVEs **at the tag** — `GHSA-768w-qrh2-jjvh`
  (control-socket peer-auth + cgroup TOCTOU, High) and `GHSA-g6v8-r577-76jm`
  (WebSocket OOB / cross-frame disclosure, High) — then backport #14/#18 to
  **1.34.x LTS**. This advances **G2 (security disclosure process)** below.
- **[#116](https://github.com/freeunitorg/freeunit/issues/116) — done this cycle:**
  the config C-string NUL/empty guards from #114 were extended to `access_log`
  path, TLS certificate names, and njs module paths (PR-A, #117) and to the
  templated `share`/`chroot` paths at request-resolution time (PR-B, #125).
  `index` was intentionally left as a plain static config value. The issue can
  be closed once PR-B lands in a tagged release.
- Deferred, non-blocking: `andypost/unit#26` (Trivy + cgroup NUL); **wasmtime → 44**
  for the last `rustls-webpki` CVE, blocked until wasmtime moves off rustls 0.22
  (first release on webpki 0.103 is 44.0.0; see [unit-wasm.md](unit-wasm.md) W1);
  **[#130](https://github.com/freeunitorg/freeunit/issues/130)** — `pkg/docker`
  `template.Dockerfile` `dpkg-query`/`set -e` pipeline can silently truncate
  `/requirements.apt` (template-wide, packaging hygiene; see G5).

_Working state / resume notes live in `plan-finish-vectors.md` (maintainer-private, not committed)._

### After 1.35.6 — where the next work goes (non-security)

With the security wave closed, the next cycle is **feature / platform work**.
Pick from here (in rough priority), not from the security list above:

1. **Graceful shutdown / reload — finish the series.** #107/#108/#111 shipped the
   signal split, listener drain, and the `active_connections` engine primitive.
   Next are the server-initiated connection drain and `POST /reload` endpoint —
   phases 5–7 of [plan-graceful-shutdown.md](plan-graceful-shutdown.md). Highest-
   leverage because it unblocks PHP P6 / Python P7 / Ruby P7 reload.
2. **"Design once" cross-cutting primitives** — status API, preload/warmup,
   graceful reload, per-target env/venv (the table below) in the router /
   controller / libunit layer so SAPIs stay thin.
3. **Per-language modules** — PHP (ZTS pool, persistent worker), Python
   (free-threaded 3.13t, subinterpreters), Ruby (Fiber scheduler, YJIT); see the
   per-language docs.
4. **Scheduler / `/run` endpoint** — [plan-run.md](plan-run.md).

The 12-month timeline at the bottom sequences these across the Core / PHP /
Python / Ruby / Governance streams.

---

## The guiding insight

Reading the three language roadmaps side-by-side, the same feature keeps appearing four or five times:

| Capability | PHP | Python | Ruby | Cron |
|---|---|---|---|---|
| Preload / warmup | P3 | P2 | P3 | — |
| Status API | P2 | P3 | P4 | — |
| Graceful code reload | P6 | P7 | P7 | (yes) |
| Persistent worker mode | P4 | P10 | P12 | — |
| Scheduler integration | P7 | P9 | P10 | all |
| Per-target env/venv | P5 | P4 | P8 | — |
| OpenTelemetry spans | cross-cut | cross-cut | cross-cut | cross-cut |

If these ship as three slightly-different implementations, the fork accumulates tech debt faster than it pays it down. **Design them once in the router / controller / libunit layer; SAPIs implement thin hooks.** That is the single most important engineering decision in this roadmap.

---

## Cross-cutting platform work

These items live in the **core daemon** (`src/nxt_router.c`, `src/nxt_controller.c`, `src/nxt_unit.{c,h}`, `src/nxt_conf_validation.c`), not in any single SAPI. Each enables the corresponding per-language item.

### X1. Unified preload/warmup contract in libunit

- New libunit callback: `nxt_unit_preload_handler_t(nxt_unit_ctx_t*, nxt_unit_preload_t*)` invoked after interpreter init, before the worker signals READY.
- Config: `"preload"` accepting `true` (language-specific auto-preload), a script path, or a list of module names/paths.
- Fork-after-preload on Linux so all workers share COW pages.
- **Enables:** PHP P3, Python P2, Ruby P3.
- **Effort:** ~1 week.

### X2. Unified status API schema

- `/status/applications/<name>/<lang>` namespace with a schema shared across languages:
  ```json
  {
    "requests": { "total": N, "active": N, "errors": N, "p50_ms": N, "p99_ms": N },
    "workers":  [{ "pid": N, "rss_kb": N, "uptime_s": N, "state": "…" }],
    "runtime":  { /* language-specific: opcache, GC, YJIT, interpreters */ }
  }
  ```
- Language modules fill only the `runtime` subtree.
- **Enables:** PHP P2, Python P3, Ruby P4.
- **Effort:** ~1 week for the schema + controller plumbing; each SAPI ~3 days.

### X3. Graceful reload endpoint

- `POST /control/applications/<name>/reload` → spawn a new generation with fresh code/state, drain old workers after `graceful_timeout`, flip routing atomically.
- Watch-file convention: `reload_on_touch: "tmp/restart.txt"` (Rails-native, also useful for PHP deploys).
- Integrates with OpenTelemetry to annotate the reload boundary as a span event.
- **Prerequisite contract (merged):** PR #54 / issue #28 made the TLS write path return a fatal error on peer-initiated half-close instead of busy-looping. The drain phase of X3 depends on this — without it, asking the old generation to stop accepting and finish in-flight requests could pin a router worker at 100% CPU on any TLS connection the client tore down mid-response.
- **Open work:** generalise the same contract to non-TLS writes (`nxt_h1proto.c` write loop, port-socket writes — see `unit-todos.md` Pattern D′) and add the server-initiated drain primitive in `nxt_main_process.c` / `nxt_event_engine.c` (`unit-todos.md` Pattern D).
- **Detailed plan:** see [plan-graceful-shutdown.md](plan-graceful-shutdown.md) — 7-phase delivery (signal split → listener drain → write-path D′ → engine teardown → connection drain → reload endpoint → per-language hooks) with mermaid diagrams, file:line edits, and per-phase tests. Effort revised to **~7–8 weeks total** (the ~2 weeks below is just the endpoint phase).
- **Enables:** PHP P6, Python P7, Ruby P7.
- **Effort:** ~2 weeks for the endpoint phase alone; ~7–8 weeks for the full lifecycle (see plan).

### X4. Persistent-worker contract

- libunit callback: `nxt_unit_request_loop_t` that lets a SAPI take full control of the per-worker request loop instead of handing back to C between requests. Semantic: "call me with requests until I return."
- Required state-reset hook between requests.
- **Enables:** PHP P4 (FrankenPHP-style), Python P10 (uvloop-native), Ruby P12 (Fiber-native).
- **Effort:** ~3 weeks (ABI-level change, needs careful design review).

### X5. Scheduler primitive

- See [unit-cron.md](unit-cron.md) in full. Two-phase:
  1. `POST /control/applications/<name>/run` with argv override (1 week).
  2. `"schedules"` config section with cron/interval syntax (3–4 weeks).
- Language-specific `preset:` sugar (`drupal`, `django`, `laravel`, `rails`) resolves `cmd` idiomatically.
- **Enables:** PHP P7, Python P9, Ruby P10.

### X6. Per-target env / path / venv overrides

- Today: `options` (PHP), `path` (Python), `hooks` (Ruby) are app-global. Move them into targets.
- Unified schema: every target accepts `env: {…}`, `working_directory`, and a language-specific block.
- **Enables:** PHP P5, Python P4, Ruby P8.
- **Effort:** ~1 week (mostly schema + config validator).

### X7. OpenTelemetry span conventions

- Standard span names: `unit.request`, `unit.scheduler.run`, `unit.worker.lifecycle`, `unit.reload`.
- Standard attributes: `unit.app`, `unit.target`, `unit.worker.pid`, `unit.language`, `unit.language.version`.
- Language-specific spans nest under these (e.g. `python.gc`, `php.opcache.miss`).
- Documented in `unit-docs/source/howto/observability.rst` (doesn't exist yet — write it).
- **Effort:** ~1 week.

### X8. Metrics endpoint (Prometheus)

- `/metrics` on the control socket exposes counters/histograms derived from X2 status data.
- `unit_requests_total{app,language,status}`, `unit_worker_memory_bytes{app,pid}`, `unit_scheduler_runs_total{app,schedule,result}`, `unit_reload_total{app}`.
- **Effort:** ~1 week.

---

## Core daemon — platform hardening

Independent of language modules. Most of these are overdue or acknowledged bugs.

### D0. io_uring event engine  *(in flight — most-developed core work)*

A new `io_uring`-based event engine alongside epoll, developed on
`andypost/unit` and destined for upstream. Unlike the mostly-aspirational D1–D10
below, this is real in-flight code under review, staged for low risk:

- **Stage 1 — poll-mode readiness bridge** (`src/nxt_io_uring_engine.c`, ~1.4k LOC):
  io_uring drives readiness with an epoll-compatible delivery contract, so the
  rest of the daemon is unchanged. Registration is gated on a **runtime probe
  with automatic epoll fallback** (also on runtime engine-creation failure);
  `--io-uring` / `--io-uring-default` configure options; signals via signalfd;
  multishot-poll re-arm hardened to honor epoll's edge-like delivery.
- **Stage 2 — completion-mode + feature tiers**: resolves capability **tiers**
  (`ACCEPT` multishot for listeners, `RECV` tier, completion-mode conn recv
  deferred behind a probe), `NXT_IO_URING_FORCE_TIER` override, epoll-degrade
  path shared and double-failure hardened, **seccomp kill-switch**, dead
  machinery dropped. Engine-contract + design-notes doc included.
- **A/B bench harness** (`tools/bench-engines.sh` + report): build-agnostic
  epoll↔io_uring comparison; detects the active engine from stderr, owns the
  unitd process tree (setsid/pgid kill ladder) for clean sweeps. _Run serially
  in a quiet environment for trustworthy numbers._
- **Why:** removes per-event syscall overhead and epoll's readiness→syscall
  round-trips; the payoff scales with connection count and is the natural next
  step after the symmetric idle/active connection accounting (#111) and the
  engine-teardown fixes (#98).
- **Status / path:** on branches (`feat/io-uring-stage1`, `feat/io-uring-stage2`,
  `bench/io-engine-harness`), multi-angle reviewed; **land via `andypost/unit`
  → upstream**. Fallback-to-epoll keeps it zero-risk to ship disabled-by-default.
- **Effort:** Stage 1 review+merge ~1 week; Stage 2 ~2–3 weeks; completion-mode
  recv path is the deepest remaining piece.

### D1. 32-bit ARM alignment fixes (armv7/armhf)

See [unit-arm32.md](unit-arm32.md). Active CI failure today. Three-stage fix:
- Static asserts on struct offsets that must be 8-byte aligned.
- `nxt_aligned(8)` + padding on `nxt_port_mmap_header_t`, `nxt_port_queue_t`, `nxt_thread_time_t`.
- Bump allocator minimum alignment to 8 on 32-bit targets.
- Also fixes nginx/unit#1600 deadlock in `nxt_event_engine_destroy()`.
- **Effort:** ~1–2 weeks.

### D2. HTTP/2 support in router

- Upstream Unit has never shipped HTTP/2 termination. `src/nxt_h1proto.c` is HTTP/1.1-only.
- Blocks HTTP/3, blocks 103 Early Hints, blocks gRPC, blocks modern observability-path improvements.
- Pragmatic path: adopt `nghttp2` as a dep; write `src/nxt_h2proto.c` alongside `nxt_h1proto.c`; route by ALPN in the TLS handshake.
- Big-ticket, multi-month. Highest single impact item in the fork.
- **Effort:** ~3 months for minimal HTTP/2. HTTP/3 (QUIC) is a separate year of work — likely out of scope.

### D3. HTTP request/response body streaming improvements

- Audit `src/nxt_h1proto.c` and the libunit body path for unnecessary buffering. Large uploads / server-sent events still hit pathological cases.
- Related to recent commit history: `tests: add edge cases for multipart upload`.
- **Effort:** ~2 weeks.

### D4. TLS modernization

- TLS 1.3 is fine. Audit:
  - Session ticket rotation defaults.
  - OCSP stapling (not currently supported).
  - ECH / Encrypted Client Hello (future).
  - Post-quantum KEMs via OpenSSL 3.x providers (X25519MLKEM768 is already widely deployed at CDN edge).
- Error-path correctness on writes already addressed by PR #54 / issue #28 (see `unit-todos.md` TLS section); treat it as the contract any new TLS-adjacent code must respect.
- **Effort:** ~2 weeks for OCSP stapling; rest is ongoing.

### D5. Config validation / error messages

- `nxt_conf_validation.c` errors are frequently unhelpful ("invalid configuration"). Add JSON Pointer paths and suggestions.
- **Effort:** ~1 week, high user-visible value.

### D6. Control API: JSON Patch / JSON Merge Patch

- Currently users PUT entire subtrees. RFC 6902 Patch / RFC 7396 Merge Patch would massively improve automation (CI/CD, Terraform providers).
- **Effort:** ~2 weeks.

### D7. Control API authentication

- The control socket is all-or-nothing (file permissions). No per-endpoint ACLs, no auth tokens for non-Unix-socket control.
- Proposal: token-based auth for a TCP control listener, scoped to endpoint patterns. Disabled by default.
- **Effort:** ~3 weeks.

### D8. Structured logging

- `unit.log` is free-form text. Add `log_format: "json"` option with stable field names (`ts`, `level`, `pid`, `app`, `msg`, `request_id`).
- **Effort:** ~1 week.

### D9. systemd socket activation

- Full socket activation (`LISTEN_FDS`, `sd_notify` READY=1/RELOADING=1) would make Unit a first-class systemd citizen.
- **Effort:** ~1 week.

### D10. Fuzzing coverage

- `fuzzing/` exists but coverage is thin. Extend OSS-Fuzz integration; at minimum the HTTP parser, JSON parser, and route matcher.
- **Effort:** ~1 week initial + ongoing.

---

## Fork governance / project-level items

Not code, but define the fork. These determine whether FreeUnit is a drive-by patchset or a sustainable LTS project.

### G1. Supported-versions matrix

**Mostly delivered as machine-enforced data (#124), not just a doc.**
- **`pkg/eol.json`** is now the single source of truth for supported runtime
  (PHP/Python/Ruby/Node/Perl/Go/Java) and OS (Alpine/Debian/RHEL/Ubuntu/…)
  versions with their EOL dates; the Docker matrix, `build-test.yml` legs, and
  `EOL.md` all derive from it.
- **`EOL.md`** is the generated human-readable matrix.
- **`unit-eol-check`** (`pkg/eol/`, Rust) validates `eol.json` against the
  endoflife.date API and **CI hard-fails** any PR that ships a runtime/OS past
  EOL + grace (the expiry gate); a weekly job reports upcoming EOLs.
- Java 25 (LTS) + 26 runtimes added on this basis (#126 / #127).
- **Remaining:** the human-facing `SUPPORT.md` policy prose (security-fix
  windows, LTS support length) — ~1 day; the enforcement mechanism now exists.

### G2. Security disclosure process

- `SECURITY.md` exists; verify it states a clear embargo window, PGP key, and a first-response SLA.
- Set up private GitHub Security Advisories.
- Register FreeUnit CVE numbering authority or document the path via MITRE.
- **Effort:** ~1 day once policy is agreed.

### G3. Release cadence

- Upstream Unit released roughly every few months. For an LTS fork:
  - **Security releases** — within 7 days of upstream-embargo lift.
  - **Minor releases** — every 8–12 weeks with new features.
  - **LTS branches** — one at a time, 2-year support window.
- Document in `RELEASE-PROCESS.md`.

### G4. Public CI matrix

- **In progress:** the runtime build/test matrix (Go/Java/Node/PHP/Python/Ruby)
  is now **data-driven from `pkg/eol.json`** (G1) — e.g. the Java legs cover
  17/21/25/26, and `build-test.yml` re-runs when `eol.json` changes (#127).
  Per-language config-validation negatives run in every matching leg (#123).
- Still to do:
  - Full PHP × Python × Ruby × Node × OS × arch expansion.
  - armv7 as a first-class CI target (once D1 lands).
  - Nightly builds against upstream PHP/Python/Ruby HEAD so regressions surface fast.
- **Effort:** ~2 weeks initial + ongoing maintenance.

### G5. Package distribution

- Today: Docker images in GHCR, generated from `pkg/docker/template.Dockerfile`
  with the version list read from `pkg/eol.json` (G1). Expand:
  - APK packages for Alpine (community repo inclusion).
  - DEB packages for Debian/Ubuntu (PPA or apt repo on `apt.freeunit.org`).
  - RPM packages for RHEL/Fedora/Rocky/Alma.
  - Homebrew tap for macOS (dev use).
- Known packaging bug: **[#130](https://github.com/freeunitorg/freeunit/issues/130)**
  — the template's `dpkg-query -S … | uniq` runtime-dep discovery can silently
  truncate `/requirements.apt` under `set -e` (fix in the template + regenerate
  all Dockerfiles).
- **Effort:** ~4 weeks initial; packaging automation in `pkg/`.

### G6. Documentation site

- `unit-docs/` repo (Sphinx) is separate and deployed to freeunit.org.
- Gaps: no developer/architecture docs (the questions future-Claude asked in CLAUDE.md creation). Write:
  - `unit-docs/source/dev/architecture.rst` (processes, ports, shared memory, event loop).
  - `unit-docs/source/dev/sapi.rst` (how to write a language module).
  - `unit-docs/source/dev/libunit.rst` (ABI reference).
- **Effort:** ~2 weeks.

### G7. Migration docs from alternatives

- Concrete step-by-step migration guides: from PHP-FPM, from Passenger, from gunicorn/uwsgi, from Puma, from Apache+mod_php. These are the highest-ROI user-acquisition content for a fork.
- **Effort:** ~1 week per guide.

### G8. Upstream patch triage

- Upstream (nginx/unit) is archived but the git history and outstanding PRs have value. Document what's been cherry-picked, what's been rejected and why, what's pending.
- Maintain `CHERRY_PICKS.md` or similar. Prevents re-litigating decisions.
- **Effort:** ongoing.

### G9. Contributor pipeline

- `CONTRIBUTING.md` is minimal. Add a "good first issue" list, document the review process, pick a DCO vs CLA policy.
- Monthly community call? Quarterly? Probably not needed yet; revisit when contributor count > 10.
- **Effort:** 2 days.

### G10. Naming / rebranding hygiene

- Source still uses `nxt_` prefix, `NGINX Unit` strings in logs, `NGX_*` in docs. Decide per-case:
  - `nxt_` C prefix — keep (would break every patch).
  - Log strings / `Server:` header — rebrand over a deprecation window.
  - Man pages / docs — rebrand freely.
- Don't pretend this isn't an NGINX fork; **do** make it clear FreeUnit is the active project.
- **Effort:** ~1 week scan + 2 weeks rolling changes.

---

## Consolidated timeline

Grouped to show parallelizable streams. Rows are calendar months from "today."

| Month | Core / platform | Cross-cutting | PHP | Python | Ruby | Governance |
|---|---|---|---|---|---|---|
| 1 | D1 armv7 fix, D5 config errors | X1 preload, X2 status schema | P3 preload | P2 preload, P3 status | P2 multiarch, P3 preload, P4 status | G1 support matrix, G2 sec policy |
| 2 | D9 systemd, D8 structured log | X3 reload, X6 per-target env | P1 ZTS threads, P2 status | P1 free-threaded 3.13t | P1 threads | G4 CI matrix |
| 3 | D4 TLS (OCSP) | X5 scheduler phase 1 (run endpoint) | P5 per-target ini | P4 venv-aware | P8 Bundler, P9 YJIT | G3 release cadence, G6 arch docs |
| 4 | D6 JSON Patch | X5 scheduler phase 2 (cron) | P6 graceful reload, P7 scheduler | P7 reload, P9 scheduler | P7 reload, P10 scheduler | G5 packaging |
| 5 | D3 body streaming | X4 persistent-worker contract | P4 persistent worker (Octane) | P5 subinterpreters | P5 Fiber scheduler | G7 migration guides |
| 6 | D2 HTTP/2 (start) | X7 OTel conventions, X8 metrics | P8 Fibers bridge | P8 ASGI extensions | P6 Ractors | G8 upstream triage |
| 7–9 | D2 HTTP/2 (ship) | | P10 CI matrix | P10 unit-native loop | P11 Rack 4 audit | G10 rebranding |
| 10–12 | D7 control auth, D10 fuzzing | | P11 WASM PHP spike | P11 CPython-WASI spike | P13 ruby-wasm spike | G9 contributor pipeline |

---

## What "done" looks like in 12 months

If this roadmap lands:

- **Multi-core scaling** in one process for every supported language: ZTS threads (PHP), free-threaded / subinterpreters (Python), threads / Ractors (Ruby).
- **Persistent-worker mode** available for every language — FrankenPHP, Octane, Falcon-class performance without framework-specific sidecars.
- **HTTP/2** in the router.
- **Scheduler** replaces host cron + docker exec for every language.
- **Zero-downtime deploys** via graceful reload for every language, including `tmp/restart.txt` for Rails.
- **armv7 CI green**, distribution packages for all three major Linux package managers, migration guides from every major alternative.
- **Observability:** Prometheus metrics, OpenTelemetry spans, structured JSON logs, unified status API.

That's the positioning: the last NGINX Unit you'll ever need, and the first server that takes Python 3.13+, Ruby 3.x, and PHP 8.5 seriously at the same time.

---

## How to use this roadmap

- **Contributors:** pick any `X*` (cross-cutting) or `D*` (daemon) item as a standalone PR. Language items (`P*` in sub-docs) depend on their `X*` parent — coordinate.
- **Users:** the table above lets you see when a feature you need is expected. Open an issue to bump priority.
- **Maintainers:** revisit quarterly. Mark items DONE / DROPPED / RESCHEDULED with dated notes. Don't let this document rot.
