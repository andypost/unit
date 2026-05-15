# FreeUnit Python — State & Roadmap

Python release cadence (3.13 no-GIL, 3.14 subinterpreters, annual October majors) is moving faster than most app-server stacks. This doc lands a snapshot of what Unit's Python module does today and a roadmap that keeps pace.

## Current state

The module is split across ~6.5 kLoC in `src/python/`:

| File | LoC | Responsibility |
|---|---|---|
| `nxt_python.c` | 920 | lifecycle, thread init/join, atexit, factory flag, target resolution |
| `nxt_python_wsgi.c` | 1413 | WSGI 1.0 protocol |
| `nxt_python_asgi.c` | 1576 | ASGI dispatcher, event loop setup |
| `nxt_python_asgi_http.c` | 689 | ASGI HTTP scope |
| `nxt_python_asgi_lifespan.c` | 659 | ASGI lifespan (startup/shutdown) |
| `nxt_python_asgi_websocket.c` | 1091 | ASGI WebSocket scope |
| `nxt_python_asgi_str.c` | 143 | interned ASGI string cache |

### Execution model

- **Per-worker thread pool**, not per-process only. Config: `"threads": N` (`nxt_python.c:236`, `:609`). Unlike PHP, Python already has a thread pool — each worker process runs N request-handler threads, each with its own `PyGILState` context. Scaling = processes × threads, throttled by the GIL.
- **WSGI or ASGI**, auto-detected per target (`nxt_python_asgi_check`). Targets can mix — each target is independently WSGI or ASGI.
- **Factory pattern** (`nxt_python.c:455`): when `factory: true`, the configured callable is invoked with no args at startup and must *return* the app. Standard for Django (`get_asgi_application()`), FastAPI factory, etc.
- **Event loop:** `asyncio.new_event_loop()` by default. `nxt_python_asgi_get_event_loop()` looks up named loop factories, so `uvloop`-importing code can win.
- **Lifespan:** full ASGI 3 lifespan with startup/shutdown events.
- **WebSocket:** ASGI WebSocket scope including subprotocol negotiation; no permessage-deflate.
- **Embed:** `python-config --embed` when available (3.8+), falls back to old `--ldflags`.
- **Version support:** claims 3.6+; actively exercised 3.10–3.13; Docker ships 3.12/3.13 and a `python3.14` variant.

### Known gaps

1. **No awareness of Python 3.13 free-threaded build (PEP 703).** Threads are serialized by GIL even when the interpreter could run without one. `nxt_python_init_threads` uses `PyGILState_Ensure` unconditionally.
2. **No subinterpreter mode (PEP 684 / PEP 734).** Each worker gets one interpreter; 3.12+ can host multiple with independent GILs and 3.13 exposes them via `concurrent.interpreters`. Not used.
3. **No preload/warmup.** Modules import on first request of the first thread. Django app graph / SQLAlchemy models / Pydantic schemas compile lazily, so p99 is bad right after spawn.
4. **No status surface.** Unit doesn't expose request counters, GC stats, memory high-water, interpreter count, thread states. Operators have no Unit-side answer to "why is Python slow right now."
5. **No per-target virtualenv.** `path` config tweaks `sys.path` but doesn't mimic `venv/bin/activate` — no `VIRTUAL_ENV`, no `site.main()` from the venv. Users hack it with absolute imports.
6. **No graceful code reload.** Same deploy-requires-restart problem as PHP.
7. **WSGI concurrency is threads-only.** No async-WSGI bridge (asgiref's `WsgiToAsgi` / Django 4.1 async views live only on ASGI).
8. **CI matrix is shallow.** Unclear coverage for 3.13t (free-threaded) or 3.14 alphas.
9. **No scheduler primitive.** Celery beat / `manage.py` / Django management commands still run under host cron or a sidecar.
10. **Factory callables can't take args.** `factory: true` is boolean — no way to pass settings to the factory.

---

## Roadmap

### Near term (1–3 months)

**P1. Free-threaded Python 3.13t support (PEP 703).**
- Detect at build time: `Py_GIL_DISABLED` macro probe in `auto/modules/python`.
- At runtime: check `PySys_GetXOptions()` or `Py_IsGILEnabled()` (3.13+) and, when GIL is disabled, switch the thread pool into **true-parallel mode**: drop the `PyGILState_Ensure`/`Release` round-trips, use `PyThreadState_Swap` more aggressively, avoid the single-lock bottleneck in the request dispatcher.
- Document the C-extension compatibility bomb: not all extensions are free-thread safe. Add a startup warning listing loaded modules not marked `Py_mod_gil = Py_MOD_GIL_NOT_USED`.
- **Wins:** genuine N-core scaling in one process; roughly matches gunicorn's free-threaded mode.
- **Effort:** ~2 weeks. Most of it is test matrix and extension compat triage.

**P2. Preload/warmup hook.**
- Config: `"preload": ["my_app.settings", "my_app.models"]` — imported before worker accepts requests. Modeled on gunicorn's `--preload`.
- Or `"preload_script": "path/to/warm.py"` for arbitrary code.
- Forks **after** import so all workers share COW pages — memory win on Linux.
- **Wins:** cold-start cliff removed; deterministic p99 on first few requests.
- **Effort:** ~3 days.

**P3. Status API for Python.**
- `/status/applications/<name>/python`: interpreter count, per-thread state (running/idle/gc), request counters, last GC stats (`gc.get_stats()`), tracemalloc high-water if enabled, import count.
- OpenTelemetry span per request: `python.request` with `{protocol, factory, target, thread_id}`.
- **Wins:** removes the Python debugging black box.
- **Effort:** ~1 week.

**P4. Virtualenv-aware launcher.**
- If `path` points inside a venv (`pyvenv.cfg` present), resolve site-packages, set `VIRTUAL_ENV`, call `site.main()` so `sys.path` matches `./venv/bin/python`.
- Support `uv` project layouts (`.venv/` discovered automatically).
- **Wins:** "works like the way I tested it" ergonomics.
- **Effort:** ~1 week.

### Mid term (3–6 months)

**P5. Subinterpreter worker mode (PEP 684/734).**
- Config: `"interpreters": N` (per process). Each interpreter gets its own GIL (3.12+) and state, runs on a dedicated thread.
- Router balances requests across (process × interpreter) pairs.
- Memory cost is higher than threads but gives real parallelism on pre-3.13 Python without free-threading.
- **Risk:** C extensions that aren't per-interpreter safe crash or leak. Same compatibility warning as P1 but differently shaped.
- **Wins:** a scaling knob that works on stable Python 3.12 today.
- **Effort:** ~3–4 weeks.

**P6. Parameterized factory.**
- Change `factory: true` to accept `factory: {args: [...], kwargs: {...}}`. Backwards compatible.
- Wire a few ergonomic builtins: `{env}`, `{listener}`, `{app_name}`.
- **Effort:** ~3 days.

**P7. Graceful code reload.**
- `POST /control/applications/<name>/reload` → spawn a new generation of workers, drain old ones after `graceful_timeout`. Same pattern as the PHP roadmap (P6 there).
- Combine with P2 so new workers arrive pre-warmed.
- **Effort:** ~2 weeks.

**P8. ASGI extensions adoption.**
- WebSocket permessage-deflate (RFC 7692). Server push (HTTP/2 — coordinated with Unit router roadmap).
- Early-data / 0-RTT handling on the ASGI scope when Unit terminates TLS 1.3.
- ASGI HTTP trailers.
- **Effort:** ~2 weeks.

**P9. Scheduler integration (`manage.py`, Celery).**
- Uses the scheduler primitive from `unit-cron.md`. Python-specific preset `preset: "django"` auto-resolves `manage.py` and sets `DJANGO_SETTINGS_MODULE` from the app env.
- Celery beat integration via scheduler: no separate beat process needed.
- **Effort:** see `unit-cron.md`.

### Long term (6–12 months)

**P10. uvloop / winloop / raw epoll integration.**
- Let Unit's own event engine drive the ASGI coroutine scheduler directly without a Python asyncio selector layer. Expose a C-level coroutine stepper. Similar to how TrueAsync does it for PHP.
- Huge throughput win for async-heavy workloads; small loss for blocking-WSGI-via-asgiref.
- Gated behind `"event_loop": "unit"` to opt in.
- **Effort:** ~2 months. Hardest to ship correctly.

**P11. Pyodide / CPython-WASI target.**
- Unit has `wasm-wasi-component` support. Once CPython-on-WASI stabilizes (3.13+ has basic support, 3.14 improves), allow `type: "python-wasm"` for per-request interpreter isolation. Exploratory.
- **Effort:** spike, 3–4 weeks.

**P12. Django-aware deploy orchestration.**
- Optional hooks: `migrate_on_start` (run `migrate --check` and bail if pending), `collectstatic_on_start`. Keeps Unit ignorant of frameworks by default but provides batteries for the 80% case.
- **Effort:** ~1 week once P9 is in place.

**P13. CI matrix expansion.**
- `{3.10, 3.11, 3.12, 3.13, 3.13t, 3.14} × {WSGI, ASGI} × {threads 1, threads 8} × {GIL, no-GIL}`.
- Per-release alpha testing with a daily CI job against `python:3-rc` images.
- **Effort:** ~1 week setup, ongoing.

---

## Short roadmap table

| # | Item | Effort | Ship window |
|---|------|--------|-------------|
| P1 | Free-threaded 3.13t mode | 2w | Near |
| P2 | Preload / warmup | 3d | Near |
| P3 | Python status API | 1w | Near |
| P4 | Venv-aware launcher | 1w | Near |
| P5 | Subinterpreter worker pool | 3–4w | Mid |
| P6 | Parameterized factory | 3d | Mid |
| P7 | Graceful code reload | 2w | Mid |
| P8 | ASGI extensions (permessage-deflate, trailers, HTTP/2 push) | 2w | Mid |
| P9 | Scheduler integration (Celery, manage.py) | see unit-cron | Mid |
| P10 | Unit-native event loop | ~2m | Long |
| P11 | CPython-WASI target spike | 3–4w | Long |
| P12 | Django lifecycle hooks | 1w | Long |
| P13 | CI matrix expansion | 1w + ongoing | Long |

**Headline bets:** P1 (free-threaded 3.13t) and P5 (subinterpreters) are the two items that unlock multi-core Python without the "just run more processes" tax. Ship those and FreeUnit becomes the obvious Python server for Python 3.13+.
