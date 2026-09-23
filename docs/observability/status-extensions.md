# `/status` additions (N4)

Status: **`schedules` shipped** on this branch (`stream/obs-d3`).
**`queue` and `shared_memory` deferred** -- see "Deferred: per-app queue
depth and SHM fill" below for why, and what would need to change to pick
them up later.

This note was originally a design-only document (see git history for that
version); it is now updated to describe what actually landed, against
`src/nxt_status.c` (the `/status` JSON builder, `nxt_status_get()`),
`src/nxt_router.c`'s `nxt_router_status_handler()` (the router-side
collector), and `src/nxt_router_schedule.c` (the schedules feature itself,
ADR 0004) as of this branch.

## Why "schedules"

`/status` already reports per-language modules, per-listener connection
counters, per-app request/process counters, and (when built with
`--otel`) exported/failed span counts. It had no visibility into whether
the "schedules" feature (ADR 0004: periodic internal requests to an
application) is doing its job -- whether a schedule is actually firing on
time, how often it is skipped or times out, and what its last run
answered. Every one of those numbers is already tracked, in
`nxt_router_schedule_state_t` (`src/nxt_router_schedule.c`): `runs`,
`skipped`, `failed`, `timed_out`, `running`, `last_status`,
`last_duration`. `/status` only had to expose them.

## JSON shape (as shipped)

A new top-level `schedules` object, sibling to `applications`, keyed by
schedule name:

```json
{
  "connections": { "accepted": 1067, "active": 13, "idle": 4, "closed": 1050 },
  "requests": { "total": 1307 },
  "applications": { "...": "..." },
  "schedules": {
    "cron": {
      "runs": 42,
      "skipped": 1,
      "failed": 0,
      "timed_out": 0,
      "running": 0,
      "last_status": 200,
      "last_duration_ms": 8,
      "last_start": 1732300042
    }
  }
}
```

Field meanings:

| Field | Meaning |
|---|---|
| `runs` | Runs dispatched to a worker so far (a run that never got dispatched, e.g. no worker engine, does not increment this; it increments `failed` instead). |
| `skipped` | Runs skipped because the previous one was still running and `overlap` is `"skip"`. |
| `failed` | Runs that could not start, or that finished with no valid response (status 0, >= 400, or a discarded/oversized response). |
| `timed_out` | Runs that hit the schedule's own `timeout` (ADR 0004 section 7.2), distinct from `failed`. |
| `running` | `1` if a run is in flight right now, `0` otherwise. |
| `last_status` | The HTTP status of the most recently *finished* run; `0` before any run has finished. |
| `last_duration_ms` | The most recently finished run's duration, in milliseconds. |
| `last_start` | Wall-clock start of the most recently *dispatched* run, whole seconds since the Epoch; `0` before any run has started. Lags `running` while that run is still in flight (`last_status`/`last_duration_ms` describe the run *before* it). |

Following the existing `telemetry` precedent (`nxt_status_get()`'s object
size arithmetic, `4 + (report->otel_configured != 0) + ...`), the whole
`schedules` object is **omitted rather than emptied** when no schedule is
configured -- so a build/config with no schedules reports exactly what it
did before this change, and a client pattern-matching `/status` sees no
new required key and no ambiguous empty object.

A schedule that was just removed from the configuration but whose last
run has not finished yet is still reported: its name stays a key in
`schedules` (with `running: 1`, most likely) until that run's result
comes back, matching how the router itself keeps that state alive.

## Where it comes from

- `src/nxt_status.h` gained `nxt_status_schedule_t` (one schedule's
  counters, mirroring the existing `nxt_status_app_t`) and two new fields
  on `nxt_status_report_t`: `schedules_count`, and a comment explaining
  that the `nxt_status_schedule_t` array sits *after* the last
  `nxt_status_app_t` in `apps[]` -- C allows only one flexible array
  member per struct, and the whole report is one contiguous buffer copied
  across the router/controller port, so both the router (writer) and
  `nxt_status_get()` (reader) compute that second array's address the
  same way: `nxt_status_report_schedules(report)`, a small inline helper
  next to the struct.
- `src/nxt_router_schedule.c`/`.h` gained a `/status`-only accessor,
  `nxt_router_schedules_status_each()`, that hands each schedule's
  counters to a callback -- the states queue
  (`nxt_router_schedule_states`) is private to that file, so this is the
  only way `nxt_router.c` can read it. No lock: both this walk and the
  counters themselves are touched only on the main engine, exactly like
  the existing `nxt_router_schedule_state_find()` walk.
- `src/nxt_router_schedule.c` also gained one new field,
  `nxt_router_schedule_state_t.last_start`, set from `nxt_realtime()` at
  the point a run is actually dispatched (`nxt_router_schedule_start()`,
  right after `state->running = 1`) -- everything else already existed.
- `src/nxt_router.c`'s `nxt_router_status_handler()` builds the
  `schedules` part of the report the same two-pass way it already builds
  `apps`: one pass to size the buffer (`nxt_router_schedules_status_count()`
  plus a size-summing callback), one to fill it (a second callback that
  copies each name into the same reverse-growing name area the app names
  already use, then writes the offset-relocated `nxt_status_schedule_t`).
- `src/nxt_status.c`'s `nxt_status_get()` turns that array into the
  `schedules` JSON object, one member per schedule, using
  `nxt_conf_set_member_dup` for the (possibly reconfiguration-surviving)
  name the same way `applications` already does.

None of this touches a struct on `tools/perf/layout-check.sh`'s SHM list;
`nxt_status_report_t` is a router-to-controller port message, not a
shared-memory mapping, so it was never checked by that gate, and this
change does not add it to one either.

## Deferred: per-app queue depth and SHM fill

The original design (see git history) proposed `applications.<app>.queue`
(depth/capacity/high-water) and a top-level `shared_memory` object. Both
are **not implemented** on this branch. What was actually feasible turned
out narrower than the design assumed, and the parts that are feasible
were judged not worth the risk of touching shared code on a day the CODE
stream is actively editing `nxt_router_prepare_msg` and the port/app-queue
sources:

- **App-queue depth is lock-free-readable, but per-*port*, not
  per-*app*.** `nxt_app_queue_t` (`src/nxt_app_queue.h`) wraps
  `nxt_app_nncq_t` (`src/nxt_app_nncq.h`), whose `head`/`tail` counters
  already have public accessors (`nxt_app_nncq_head()`/`_tail()`) and can
  be read with a plain (non-locking) load -- `tail - head` is a
  reasonable approximate depth, the same class of read the queue's own
  enqueue/dequeue paths already do. The problem is reaching a given app's
  queue(s) at all: an app can have several ports (`nxt_app_t.ports`,
  `src/nxt_router.h`), and that queue is explicitly protected by
  `nxt_app_t.mutex` ("Protects ports queue"). Summing depth across an
  app's ports from the `/status` path would mean taking that mutex from a
  new call site on the router's status-request path -- exactly the "new
  locking" this stream was told to avoid, and a lock also taken from
  worker-engine code the CODE stream is editing today. Reading a single
  queue's depth is cheap; enumerating which queues belong to which app
  safely is not, without either that lock or a new lock-free index this
  stream has no mandate to add mid-stream.
- **SHM (mmap) fill** (`nxt_port_mmap_header_t.free_map[]` popcount,
  `oosm` flag) needed no new synchronization by the original design's own
  reasoning, but reaching it means walking `nxt_process_t`'s mmap handler
  list, which is exactly the data structure the CODE stream owns today
  (`nxt_port*.c`). Adding a `/status` read path into it mid-stream risks
  a merge conflict or a subtle race with in-flight edits to the same
  structures, for a feature this stream was told to skip rather than risk
  when in doubt.

Both remain sourceable without a shared-memory layout change and without
new locking *for the depth read itself* -- the missing piece is safe
enumeration, not safe reading. A follow-up stream, landing after the CODE
stream's port/app-queue work and free to add either a lock-free per-app
running total (updated at the existing enqueue/dequeue call sites, as the
original design proposed) or to take `nxt_app_t.mutex` deliberately and
measure the cost, can pick this up; nothing this stream shipped forecloses
it.

## Dashboard demo JSON shape

There is no dashboard demo JSON fixture in this repository as of this
branch (`docs/observability/` has this file plus `usdt.md` and
`usdt-plan.md`, neither of which carries a `/status` JSON shape). If one
is added later, it should use the field names in this document --
`schedules.<name>.{runs,skipped,failed,timed_out,running,last_status,
last_duration_ms,last_start}` -- not the earlier design draft's
`schedules.{total,due,overdue,last_run_ms_ago}`, which never shipped.
