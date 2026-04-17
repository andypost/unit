# FreeUnit Roadmap

Technical roadmap documents for the FreeUnit fork. Start with [**unit-roadmap.md**](unit-roadmap.md) — it's the hub document that consolidates cross-cutting work and links everything else.

## Documents

| File | Purpose |
|---|---|
| [unit-roadmap.md](unit-roadmap.md) | **Start here.** Cross-cutting platform work, core daemon items, governance, 12-month timeline. |
| [unit-php.md](unit-php.md) | PHP module state and roadmap — ZTS worker pool, persistent worker, TrueAsync, version support. |
| [unit-python.md](unit-python.md) | Python module — free-threaded 3.13t, subinterpreters (PEP 684/734), ASGI/WSGI, venv, preload. |
| [unit-ruby.md](unit-ruby.md) | Ruby module — thread pool, Ractors, Fiber scheduler, YJIT, Bundler, Rack 3. |
| [unit-cron.md](unit-cron.md) | Scheduler/cron primitive for drush, Celery, Sidekiq, rake, artisan, manage.py. |
| [unit-arm32.md](unit-arm32.md) | 32-bit ARM (armv7/armhf) SIGBUS / alignment investigation and fix plan. |
| [unit-maintainer.md](unit-maintainer.md) | Maintainer-facing synthesis of the roadmap — priorities, sequencing rules, near-term backlog, and governance guidance. |
| [unit-todos.md](unit-todos.md) | Inventory of ~90 `TODO`/`FIXME`/`XXX`/`HACK`/`BUG` markers across the codebase, grouped by subsystem. |
| [unit-wasm.md](unit-wasm.md) | WASM-бэкенды (Wasmtime core SAPI + WASI 0.2 component model), async body streaming, multi-runtime abstraction (Wasmer/WasmEdge), wasi-nn, WASI P3, language presets (PHP-wasm/CPython-WASI/ruby.wasm), OCI distribution. |
| [plan-run.md](plan-run.md) | `/run` endpoint & scheduler implementation plan — control API extension, cron engine, WASM task execution, overlap policies, OTel integration, mermaid diagrams. |

## Scope

These are **planning documents**, not commitments. They capture the design space and a prioritization that matches the fork's stated mission (LTS maintenance, PHP 8.4/8.5, Python 3.13+, Ruby 3.x). Items move between timelines as contributors pick them up.

The hub doc includes a 12-month consolidated timeline with parallel streams for Core / Cross-cutting / PHP / Python / Ruby / Governance work. Revisit quarterly; mark items DONE / DROPPED / RESCHEDULED with dated notes.
