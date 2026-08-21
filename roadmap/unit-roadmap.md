# FreeUnit Roadmap

Consolidated technical roadmap for the FreeUnit fork. Groups work that's **shared across all language modules** so it's designed once, plus **core-daemon improvements** independent of any SAPI, plus **fork-governance items** that aren't code but define what "FreeUnit" means as a project distinct from upstream.

Per-language detail lives in:
- [unit-php.md](unit-php.md) — PHP SAPI, ZTS, persistent-worker, TrueAsync
- [unit-python.md](unit-python.md) — WSGI/ASGI, free-threaded 3.13t, subinterpreters
- [unit-ruby.md](unit-ruby.md) — Rack, threads, Fiber scheduler, Ractors
- [unit-cron.md](unit-cron.md) — scheduler/cron primitive (drush, Celery, Sidekiq)
- [unit-arm32.md](unit-arm32.md) — armv7 SIGBUS / alignment investigation
- [unit-ai-agentic.md](unit-ai-agentic.md) — AI-agentic workloads track; reuses `X*`/`D*`/`W*` items on a quarterly schedule, adds no new primitives

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
- **Enables:** PHP P6, Python P7, Ruby P7.
- **Effort:** ~2 weeks.

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

Published policy document (`SUPPORT.md`) stating:
- Which Unit versions receive security fixes and for how long.
- Which PHP/Python/Ruby/Node/Perl/Go/Java minors are supported (and their EOL dates).
- OS support (Alpine, Debian, RHEL, Ubuntu — versions).
- **Effort:** 1 day of writing.

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

- Today: one GitHub Actions workflow. Expand to:
  - All supported PHP × Python × Ruby × Node × OS × arch combinations as a matrix.
  - armv7 as a first-class CI target (once D1 lands).
  - Nightly builds against upstream PHP/Python/Ruby HEAD so regressions surface fast.
- **Effort:** ~2 weeks initial + ongoing maintenance.

### G5. Package distribution

- Today: Docker images in GHCR. Expand:
  - APK packages for Alpine (community repo inclusion).
  - DEB packages for Debian/Ubuntu (PPA or apt repo on `apt.freeunit.org`).
  - RPM packages for RHEL/Fedora/Rocky/Alma.
  - Homebrew tap for macOS (dev use).
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

Grouped to show parallelizable streams. Rows are calendar months from "today." An additional AI-agentic stream ([unit-ai-agentic.md](unit-ai-agentic.md)) runs in parallel; it reprioritizes a subset of the `X*` / `D*` / `W*` items below into quarterly milestones and adds no new work.

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
