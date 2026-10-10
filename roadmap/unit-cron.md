# FreeUnit Scheduler — Design Brainstorm

Running periodic CLI tasks (drush cron, Laravel `artisan schedule:run`, Rails `rake`, Django `manage.py`, Celery beat-style jobs) **inside** Unit — sharing the same runtime, isolation jail, rootfs, cgroups, user, and OpenTelemetry pipeline as the web app.

Host cron + docker exec is the status quo. Its problems: duplicated isolation config, no unified logging/metrics, drift between web and cron environments, awkward credential surface.

---

## Target UX

```json
{
  "applications": {
    "drupal": {
      "type": "php",
      "root": "/var/www/drupal",
      "user": "www-data",
      "isolation": {
        "namespaces": { "mount": true, "pid": true, "network": false },
        "rootfs": "/var/www/drupal"
      },
      "schedules": [
        {
          "name": "cron",
          "cmd": ["vendor/bin/drush", "cron"],
          "every": "5m",
          "on_overlap": "skip",
          "timeout": "5m"
        },
        {
          "name": "queue-default",
          "cmd": ["vendor/bin/drush", "queue:run", "default"],
          "every": "1m",
          "on_overlap": "skip"
        },
        {
          "name": "nightly-purge",
          "cmd": ["vendor/bin/drush", "cache:rebuild"],
          "at": "0 3 * * *",
          "timeout": "30m"
        }
      ]
    }
  }
}
```

One-shot invocation via control API / unitctl:

```console
$ unitctl run drupal/cron
$ curl -X POST --unix-socket /var/run/control.unit.sock \
       http://localhost/control/applications/drupal/run \
       -d '{"cmd": ["vendor/bin/drush", "updb", "-y"]}'
```

Status API:

```console
$ curl --unix-socket … /status/applications/drupal/schedules
{
  "cron": {
    "last_run": "2026-04-15T14:05:03Z",
    "last_exit": 0,
    "last_duration_ms": 842,
    "next_run": "2026-04-15T14:10:03Z",
    "runs_total": 2881,
    "failures_total": 3
  }
}
```

---

## Implementation angles

### 1. New `type: "*-scheduler"` process type

Add a scheduler-flavored SAPI next to existing ones (`auto/modules/php-scheduler`). Each fire spawns a fresh worker that embeds the runtime, runs argv, exits.

- **Pros:** clean separation; full reuse of isolation/cgroups/rootfs; leak-proof (fresh process per run like traditional cron).
- **Cons:** duplicates SAPI init per language; cold-start cost on each fire (PHP especially — opcache lost). Not great for every-minute jobs.

### 2. Scheduler as controller/router feature (recommended)

New top-level config entry (`"schedulers"` table). Controller owns cron state; on fire, sends `NXT_PORT_MSG_RUN_TASK` to an existing app worker carrying argv + working-dir + env delta. libunit grows a callback:

```c
typedef int (*nxt_unit_task_handler_t)(nxt_unit_ctx_t *ctx,
                                       nxt_unit_task_t *task);
```

SAPIs implement it by invoking the language's "run script with argv" path (PHP: `php_execute_script` with overridden SAPI request; Python: reuse interpreter, exec entry point; Ruby: `rb_load_protect`).

- **Pros:** one scheduler across all languages; warm interpreter reuse (fast); triggers usable from webhooks or manual; extends naturally to "worker pool for background jobs."
- **Cons:** libunit ABI bump — rollout coordination with every SAPI; in-process side effects risk (leaked globals, opcache poisoning, file-descriptor drift).

### 3. Sidecar invoking normal app via synthetic HTTP

Dedicated `scheduler` process (like `discovery`) reads cron table, on fire sends a synthetic HTTP request to a conventional internal route (`/__cron/cron` or Drupal's existing `/cron/<key>`).

- **Pros:** smallest diff; zero SAPI changes; works immediately for anything with an HTTP cron endpoint.
- **Cons:** requires app cooperation; auth surface (private listener or HMAC); stdout/stderr capture is indirect; not a fit for tasks without HTTP handlers (raw drush subcommands).

### 4. External-trigger primitive first

Add `POST /control/applications/<name>/run` with `{cmd, env, cwd}` that spawns a one-shot worker reusing the app's isolation. Users wire host cron / systemd timers externally.

- **Pros:** trivial; composable; immediately useful; ships as a single PR.
- **Cons:** punts scheduling; fragments the UX (two places to configure).

---

## Recommended path: (4) → (2), with (1) as the fallback engine

**Phase 1 — `unitctl run` primitive.** Land option 4 first. One week of work. Useful on its own. Validates the "spawn-one-shot-with-override" plumbing.

**Phase 2 — In-process scheduler.** Add the `schedules` array, cron parser, timer wiring, status API. Scheduler uses the Phase-1 primitive under the hood — either via an in-process call or by POSTing to it. SAPIs gain the task callback gradually; the scheduler falls back to option-1 (fresh process) for any SAPI that doesn't implement it yet.

**Phase 3 — Observability and lifecycle polish.** Overlap policies, retry backoff, OTel spans, structured log tags, failure alerting hooks.

This sequencing matches how Unit historically grew features (primitive in router → control API surface → config sugar).

---

## Cross-cutting design decisions

### Scheduling

- **Formats:** support cron-syntax (`"at": "*/5 * * * *"`), interval shorthand (`"every": "30s" | "5m" | "1h"`), and anchors (`"at": "@daily"`, `"@hourly"`, `"@reboot"`). The shorthand is drastically harder to misread than raw cron and should be the documented default.
- **Time source:** event-engine timer wheel (`nxt_timer_t`), not SIGALRM. Compute next-fire from wall clock so long GC pauses / sleep-wake don't accumulate drift. Missed intervals while the daemon was down: configurable `"catchup": "none" | "one" | "all"` (systemd-timer semantics).
- **Timezone:** per-schedule `"tz": "Europe/Amsterdam"`, default UTC. Critical for `@daily` at 3 AM.
- **Parser:** add minimal `src/nxt_cron.c` (Vixie-cron subset, ~300 LoC) or vendor a BSD-licensed one. Avoid full extended-cron syntax.

### Concurrency / overlap

- **`on_overlap`:** `skip` (default — prior run still going → drop this fire; log once), `queue` (enqueue, with `max_queue`), `parallel` (allow, with `max_concurrent`), `cancel_previous` (SIGTERM then SIGKILL the old one).
- **Per-schedule lock:** in-memory lock guarded by the event engine, not filesystem — simpler and sufficient since all fires go through the controller.
- **Timeouts:** `timeout` sends SIGTERM, after `grace_period` (default 10s) SIGKILL. Exit code reported as `timeout`.

### Output and observability

- **stdout/stderr capture:** stream line-buffered into Unit's error log with `app=<name> schedule=<task> run_id=<uuid>` tags. Optional `"log": "/var/log/unit/drupal-cron.log"` per-task file.
- **Status API:** `/status/applications/<app>/schedules/<name>` with last-N runs (ring buffer, N=20), start/end/exit/duration/stdout-preview. Cheap to serve, huge debugging value.
- **OpenTelemetry:** emit a span `scheduler.run` per execution with attrs `app`, `schedule`, `exit_code`, `duration_ms`, `overlap_skipped`. Auto-link to the existing OTel context if `--otel` is built.
- **Metrics (future):** Prometheus counters `unit_scheduler_runs_total{app,schedule,result}`, histogram `unit_scheduler_duration_seconds`.

### Security & isolation

- Scheduled tasks **inherit the app's entire isolation block** — same namespaces, rootfs, cgroups, user, capability set, seccomp filters. This is the actual selling point: host cron can't easily replicate a chrooted PHP-FPM jail; Unit already has one.
- Env delta is additive: app env + schedule-specific overrides. Never exposes the control socket to the task.
- `"run"` control endpoint requires the same auth as other mutating control APIs. Rate-limit default: 10/min/app.
- Drush `--uri` and `--root` get auto-populated from app config unless overridden — eliminates the classic drush-from-cron footgun (wrong URI → wrong multisite).

### Lifecycle & reconfigure

- On config reload: preserve running tasks; diff schedules; cancel removed ones at next idle; recompute next-fire for kept ones (keep jitter stable by deriving from `hash(app+name)`).
- On `SIGQUIT` graceful shutdown: allow in-flight scheduled runs up to `graceful_timeout`, then SIGTERM.
- App restart (`NXT_PORT_MSG_APP_RESTART`): treated same as reload — running schedules complete against the old process, new fires go to new workers.

### Failure handling

- **Retry:** `"retry": { "attempts": 3, "backoff": "exponential", "max_delay": "10m" }`. Default: no retry (cron-native expectation).
- **Dead-letter / alerting hook:** `"on_failure": { "exec": ["/usr/local/bin/pager"], "after_consecutive": 3 }` — lets operators wire PagerDuty/Slack without Unit itself talking to them.
- **Backpressure:** if the app's worker pool is saturated, option-2 scheduler must not deadlock — either block with timeout and report `skipped_saturation`, or fall back to spawning a fresh one-shot process.

---

## Drush-specific sugar

Ship an opt-in template that knows Drupal's conventions:

```json
{
  "type": "php",
  "preset": "drupal",
  "root": "/var/www/drupal",
  "schedules": {
    "drush:cron": { "every": "5m" },
    "drush:queue:run": { "args": ["default"], "every": "1m" },
    "drush:cache:rebuild": { "at": "0 3 * * *" }
  }
}
```

The `drush:*` preset resolves `cmd` to `["vendor/bin/drush", "--root=/var/www/drupal", "--uri=<first-listener>", "<subcommand>"]` and enforces `on_overlap: skip`. Similar presets: `artisan:*`, `rake:*`, `manage:*`.

---

## Open questions

1. **Per-app scheduler vs. global?** Global scheduler process simplifies clock/drift math but centralizes a failure domain; per-app keeps isolation pure but multiplies timers. Global + per-app dispatcher seems right.
2. **Should `"run"` block on completion?** Probably return `run_id` immediately, offer `/control/runs/<id>` for polling and `/control/runs/<id>/stream` for SSE-style log tail. Don't build long-poll into the base control API.
3. **WebAssembly task type?** `cmd` for WASI components would be a natural fit — tiny cold-start, no SAPI coupling. Possibly the *cleanest* first target for option 1, before PHP.
4. **Interaction with `processes: { max: 1 }` apps?** Single-worker apps mean the scheduled task blocks web traffic. Document clearly; recommend a separate "worker pool" app instance sharing rootfs.
5. **Distributed leader election?** If two Unit instances run the same config behind a load balancer, both fire the same cron. MVP: document "pick one host." Long-term: optional `"leader_election": { "backend": "file" | "redis" }`.

---

## Minimal file layout if we land this

```
src/nxt_scheduler.c            # engine: timer wheel + dispatch
src/nxt_scheduler.h
src/nxt_cron.c                 # cron syntax parser + next-fire math
src/nxt_cron.h
src/nxt_controller.c           # + /control/applications/*/run
src/nxt_conf_validation.c      # + "schedules" schema
src/nxt_status.c               # + /status/applications/*/schedules
src/nxt_unit.c / nxt_unit.h    # + nxt_unit_task_handler_t (Phase 2)
src/nxt_php_sapi.c             # + task callback impl (Phase 2)
test/test_scheduler.py         # drush-style fixtures
unit-docs/source/howto/
  scheduler.rst                # user-facing docs
```

Estimated effort: Phase 1 ~1 week, Phase 2 ~3–4 weeks including one SAPI, Phase 3 ~2 weeks. Parser + timer wiring is small; the bulk is SAPI integration and test coverage across languages.
