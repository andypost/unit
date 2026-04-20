# FreeUnit PHP — State & Roadmap

## Current state

The PHP module is a single 2.6 kLoC translation unit, `src/nxt_php_sapi.c`, plus the configure script `auto/modules/php`. It links against `libphp.so` (or `.a` with `--lib-static`) via `php-config`, implements the `sapi_module_struct` contract, and runs *embedded* — the PHP runtime lives inside each Unit app worker process.

### Execution model

- **One request per process at a time.** Worker processes are preforked (`processes: { max, spare, idle_timeout }` in config); scaling is horizontal across processes, not threads. Comparable to PHP-FPM's static/dynamic/ondemand pools.
- **Targets** (`src/nxt_php_sapi.c` — `nxt_php_target_t`): a single app can expose multiple script/root/index tuples, selected via the route `pass` string.
- **Options** (`options.file`, `options.user`, `options.admin`): php.ini path override plus per-app ini entries injected at `ZEND_INI_SYSTEM` / `ZEND_INI_USER` scope during `nxt_php_setup`.
- **Isolation:** inherits Unit's full isolation block — namespaces, rootfs, cgroups v2, user/uid, capabilities, seccomp.
- **Version support:** PHP 5.4 → 8.5. Versioned behind `PHP_VERSION_ID` guards (`NXT_PHP7`, `NXT_PHP8`). Fork primary motivation is PHP 8.4/8.5 (upstream stopped at 8.3 era).

### ZTS today

ZTS (Zend Thread Safety) is **detected and initialized**, but **not exploited**:

- `auto/modules/php` probes the `ZTS` macro with a feature test.
- `nxt_php_sapi.c:422-431` calls `php_tsrm_startup()` (PHP ≥7.4) or `tsrm_startup() + ts_resource()` for older versions.
- `nxt_php_sapi.c:401` keeps a `static void ***tsrm_ls` for < 7.4.

That's it. Unit still runs one request per worker process; the TSRM infrastructure is paid for but not used to run concurrent requests on threads. A ZTS build today only buys you the ability to link against a ZTS-compiled `libphp.so` (sometimes the only one available in a distro).

### TrueAsync (opt-in, experimental)

See `true-async.md`. Enabled by `async: true` + `entrypoint`. Uses the Zend Async API (`zend_async_event_t`, `zend_async_scope`) to drive multiple concurrent requests inside one process via coroutines/Fibers, scoped superglobals, `register_handler()` callback. Build-gated by `NXT_PHP_TRUEASYNC` (`zend_async_event_t` feature probe). Requires a PHP build that ships the TrueAsync API — not mainstream PHP yet. Single-process concurrency, cooperatively scheduled; does not use threads.

### Build knobs

`auto/modules/php` defines, based on feature probes:
- `NXT_ZEND_SIGNAL_STARTUP` — call `zend_signal_startup()` to work around php#71041.
- `NXT_PHP_TRUEASYNC` — TrueAsync API available.
- `NXT_PHP_PRE_REQUEST_INIT` — `sapi_module_struct.pre_request_init` field present (newer PHP).
- `--lib-static`, `--lib-path`, `--config`, `--module` — pick the libphp, its path, the resulting `.unit.so` name.

### Known gaps

1. **No thread-per-request mode.** ZTS is loaded but idle. Concurrency only via more processes.
2. **Cold bootstrap every request.** No persistent worker mode (FrankenPHP-style) outside of TrueAsync.
3. **Opcache not shared across processes.** Each worker fills its own opcache; preloading is primed by Unit via the `preload` config key (P3 shipped).
4. **No JIT tuning defaults.** Users set `opcache.jit_buffer_size` etc. via `options` — Unit does nothing to help.
5. **No per-target php.ini.** `options` are app-global, not target-scoped.
6. **No status surface.** Unit doesn't expose opcache stats, interned-strings memory, accelerator hit rate, or per-request timing. PHP-FPM's `pm.status_path` equivalent is missing for PHP specifically.
7. **No graceful code-reload.** Deploying new code requires a full app restart (or `max_requests` churn) — no `SIGUSR2`-style drain-and-swap.
8. **PHP 8.5 Fibers/async runtime adoption** is confined to TrueAsync, which relies on a forked PHP. Mainline Fibers (PHP 8.1+) aren't specifically integrated with Unit's event engine.
9. **CLI/scheduler path absent.** No primitive to run drush/artisan inside the same jail (see `unit-cron.md`).
10. **Test matrix is thin.** `test/test_php_*.py` exists but coverage across NTS/ZTS/debug/JIT-on/JIT-off builds is limited.

---

## Roadmap

Ordered by **shipping value ÷ implementation risk**, not strict dependency order. Each item is scoped to land standalone.

### Near term (1–3 months)

**P1. ZTS worker-pool mode — thread-per-request.**
- New config knob: `"threads": N` alongside `processes`. When `threads > 1`, require a ZTS build; otherwise fail loudly at startup.
- Each worker process runs `N` request-handler threads. libunit already uses one event context; extend per-thread context creation, map each thread to a TSRM resource via `ts_resource(0)` on thread start.
- Router balances across (process × thread) endpoints. Response path stays per-request; no shared mutable state beyond opcache/interned strings (already thread-safe under ZTS).
- **Wins:** dramatic memory reduction (one opcache per process instead of per request-serving unit), lower p99 under burst, faster cold start on new connections, competitive with `mpm_worker` + mod_php.
- **Risks:** non-thread-safe extensions (ext/mysqli with some drivers, xdebug, legacy). Document a known-bad list, add a startup check that iterates `EG(modules)` and warns.
- **Effort:** ~2–3 weeks. Most work is test coverage and extension compatibility triage, not the dispatch plumbing.

**P2. Status API for PHP.**
- `/status/applications/<name>/php` returns: opcache stats (hits, misses, cached scripts, memory used/free, interned strings), JIT state, request counters (total, active, rejected), last GC run, per-worker memory high-water-mark.
- Implementation: one SAPI internal call per worker that scrapes `opcache_get_status()` equivalents from C (`accel_shared_globals`, `ZCSG` macros) without needing a PHP function call.
- **Wins:** removes the "is opcache actually hot" mystery; feeds Prometheus.
- **Effort:** ~1 week.

**P3. Preload/warm-up hook.**
- Config: `"preload": "/path/to/preload.php"` mapped to `opcache.preload` automatically, executed during `nxt_php_setup` before first request.
- Extend to an explicit `"warmup": ["/script1.php", "/script2.php"]` that eagerly compiles without executing (via `opcache_compile_file`).
- **Wins:** deterministic p99 on the first request after reload; large frameworks (Symfony, Laravel) see huge cold-start savings.
- **Effort:** ~3 days.

### Mid term (3–6 months)

**P4. Persistent worker mode (FrankenPHP-style).**
- Config: `"worker": "/path/to/worker.php"`. Script runs once, then a callback (set via a Unit-provided PHP extension function) is invoked per request with `$request`, returning `$response`. No re-init between requests.
- Different from TrueAsync: serial, not coroutine-based — works with any PHP 8.1+, no TrueAsync API needed. Stackable with P1 threads.
- Must reset opcodes/objects between requests (follow FrankenPHP's `frankenphp_handle_request` reset recipe).
- **Wins:** Laravel Octane-class performance without Swoole/RoadRunner. Major positioning win for FreeUnit.
- **Effort:** ~4–6 weeks. State-reset correctness is the hard part.

**P5. Per-target php.ini and environment.**
- Allow `options`, `admin`, `user` inside a target definition, overriding the app-global ones.
- Per-target `working_directory`, `chdir`, environment delta.
- **Wins:** multisite Drupal / Symfony multi-app deployments stop needing separate Unit applications.
- **Effort:** ~1 week.

**P6. Graceful code reload (hot swap).**
- New control endpoint: `POST /control/applications/<name>/reload`. Spawns a new generation of workers with fresh opcache, drains existing ones after `graceful_timeout`.
- Integrates with OpenTelemetry to annotate the reload boundary.
- **Wins:** deploy without a request-draining load-balancer dance.
- **Effort:** ~2 weeks, some of it overlap with scheduler reload work in `unit-cron.md`.

**P7. Scheduler integration (`drush`, `artisan`).**
- Lands the Phase-1 primitive from `unit-cron.md` (`POST /control/applications/<name>/run`).
- PHP-specific sugar: `preset: "drupal"` / `preset: "laravel"` auto-resolves drush/artisan paths, applies `--uri` / `APP_URL` overrides from the first listener.
- **Effort:** see `unit-cron.md`.

### Long term (6–12 months)

**P8. Native Fibers ↔ Unit event loop bridge.**
- Without requiring the TrueAsync fork. Expose a Unit-provided PHP extension that schedules Fibers on Unit's event engine (epoll/kqueue). Makes `react/async` or `amphp` cooperate natively with Unit I/O.
- Distinct from P4 (serial persistent worker) and TrueAsync (coroutine scope) — this is "plain Fibers with a real event loop underneath."
- **Effort:** ~2 months. Requires careful design of the libunit-to-PHP scheduler handshake.

**P9. JIT-aware tuning defaults.**
- At startup, inspect CPU flags and set `opcache.jit`, `opcache.jit_buffer_size`, `opcache.jit_prof_threshold` to sensible values if the user hasn't. Warn when JIT is requested but build doesn't support it (e.g. musl aarch64 historically).
- Per-target JIT buffer isolation would require PHP upstream changes — leave out.
- **Effort:** ~1 week.

**P10. CI matrix expansion.**
- GitHub Actions matrix: `{PHP 8.1, 8.2, 8.3, 8.4, 8.5} × {NTS, ZTS} × {JIT on, JIT off} × {debug, release}`.
- Targeted regression tests for each ZTS feature once P1 lands.
- **Effort:** ongoing; initial setup ~1 week.

**P11. WASM-compiled PHP target.**
- Not this repo's job long-term, but Unit already has `wasm-wasi-component` support (`src/wasm-wasi-component/`). Running `php-wasm` as a component would offer per-request isolation without process cost. Exploratory.
- **Effort:** spike, 2–3 weeks.

---

## Cross-cutting concerns

### Version policy
Keep PHP 5.4+ build compatibility as long as the `#if PHP_VERSION_ID` guards don't become unbearable. Formally test 8.1+. Drop 5.x from CI matrix (untested ≠ unsupported code removal — keep guards).

### Extension compatibility
Publish a **known-bad-under-ZTS** extension list in user docs once P1 lands. Detect and warn at startup.

### Configuration ergonomics
The config surface for PHP is growing (processes, threads, preload, worker, targets, options, async, entrypoint, schedules). Propose a consolidated `"php"` section in config schema docs (not code — just documentation grouping) to reduce config-file cognitive load.

### Observability
Every roadmap item should update the status API under `/status/applications/<name>/php` and emit OpenTelemetry spans for request-path and lifecycle events. Consistency matters more than feature completeness here.

### Backport policy for the LTS fork
- **Security fixes:** backport aggressively to the current `main`.
- **ZTS mode (P1):** land only on `main` once stable; do not backport to LTS branches — it's a semantic change.
- **Persistent worker (P4):** opt-in; safe to backport because disabled by default.

---

## Short roadmap table

| # | Item | Effort | Ship window |
|---|------|--------|-------------|
| P1 | ZTS thread-per-request worker pool | 2–3w | Near |
| P2 | PHP status API (opcache/JIT/counters) | 1w | Near |
| P3 | Preload/warmup hook | 3d | Shipped |
| P4 | Persistent worker mode (Octane-style) | 4–6w | Mid |
| P5 | Per-target ini / env | 1w | Mid |
| P6 | Graceful code reload | 2w | Mid |
| P7 | Scheduler integration (drush/artisan) | see unit-cron | Mid |
| P8 | Fibers ↔ event-loop bridge | ~2m | Long |
| P9 | JIT-aware defaults | 1w | Long |
| P10 | Expanded CI matrix | 1w + ongoing | Long |
| P11 | WASM PHP target spike | 2–3w | Long |

**Headline bets:** P1 (ZTS worker pool) and P4 (persistent worker) are the two changes that would most clearly differentiate FreeUnit from PHP-FPM and justify the fork's existence beyond "keeps working on PHP 8.5." Ship those and the PHP story writes itself.
