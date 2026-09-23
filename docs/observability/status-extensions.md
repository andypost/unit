# `/status` additions (N4): design note

Status: design only, not implemented. Written against
`src/nxt_status.c` (the `/status` JSON builder, `nxt_status_get()`) as of
this branch. No source changes in this note -- see "Implementation notes"
for the follow-up shape.

## Why these three

`/status` today reports per-language modules, per-listener connection
counters, per-app request/process counters, and (when built with
`--otel`) exported/failed span counts (`src/nxt_status.c:163`, the
`telemetry` object gated on `report->otel_configured`). It has no
visibility into three things this stream's USDT probes and Day-1 work
now make easy to source cheaply:

Referenced line numbers below (`src/nxt_status.c:54`, `nxt_status_get()`'s
object-size arithmetic; `:163`, the `telemetry` gate) are current as of
this branch; re-check them if `nxt_status.c` has moved since.

1. **App-queue depth** -- how full the router-to-worker SHM ring
   (`nxt_app_queue_t`, `src/nxt_app_queue.h`) is, per app. A queue that
   is chronically near-full is a leading indicator of a worker that
   cannot keep up, well before requests start timing out.
2. **SHM (mmap) fill** -- how much of each `PORT_MMAP_DATA_SIZE` segment
   (`src/nxt_port_memory_int.h`) is actually allocated as chunks, and how
   many segments exist. This is the "are we about to hit `oosm`"
   (out-of-shared-memory) signal.
3. **Schedules counters** -- the DRUPAL-stream "schedules" hook near
   `nxt_router_conf_create` (owned by another in-flight stream on
   `nxt_router.c`) is expected to maintain its own counters; this design
   reserves their shape in `/status` without depending on that stream's
   internals landing first.

## JSON shape

Extending the existing per-application object
(`src/nxt_status.c`'s `apps_str` / `app_obj`, currently
`{processes: {...}, requests: {...}}` per app name) with a new
`queue` member, and adding a top-level `shared_memory` object:

```json
{
  "applications": {
    "my_python_app": {
      "processes": { "running": 2, "starting": 0, "idle": 1 },
      "requests": { "active": 3, "total": 41022 },
      "queue": {
        "depth": 6,
        "capacity": 254,
        "high_water": 40
      }
    }
  },
  "shared_memory": {
    "segments": 3,
    "segment_size": 10486784,
    "chunk_size": 16384,
    "chunks_total": 1920,
    "chunks_free": 512,
    "oosm_count": 0
  },
  "schedules": {
    "total": 12,
    "due": 1,
    "overdue": 0,
    "last_run_ms_ago": 4210
  }
}
```

`queue` and `schedules` are per-app-family objects that follow the same
"omit rather than zero" convention `telemetry` already uses
(`src/nxt_status.c:54`, the `4 + (report->otel_configured != 0)` sizing):
a build/config where the feature does not
apply (no app queue configured, no schedules configured) leaves the key
out entirely, so existing consumers that pattern-match `/status` see no
new required field and no ambiguous zero.

## Where each value comes from

| Field | Source | Notes |
|---|---|---|
| `applications.<app>.queue.depth` | `nxt_app_queue_t.queue` head/tail distance, via `nxt_app_nncq_t` (`src/nxt_nncq.h`), read where `nxt_app_queue_send()`/`nxt_app_queue_recv()` already touch it (`src/nxt_app_queue.h:81`, `:127` -- the same two lines carrying `NXT_USDT(queue__enqueue/dequeue, ...)` from this stream) | Cheapest as a running counter maintained alongside the two USDT call sites (increment on enqueue, decrement on dequeue) rather than re-derived from the ring's head/tail on every `/status` request, which would need a lock/atomic read racing live traffic. |
| `applications.<app>.queue.capacity` | `NXT_APP_QUEUE_SIZE` (`src/nxt_app_queue.h:15`) | Compile-time constant, not per-app; included per-app for convenience so a client does not need a second lookup. |
| `applications.<app>.queue.high_water` | New running max alongside `depth`, reset semantics TBD (e.g. reset on each `/status` read, like a counter snapshot, or free-running) | Needs a decision in implementation: free-running (never resets) answers "how bad has it ever been", reset-on-read answers "how bad since I last looked". Either is a one-line addition next to the existing depth counter. |
| `shared_memory.segments` | Count of `nxt_process_t.outgoing`/`incoming` mmap handles (`src/nxt_port_memory_int.h`, `nxt_port_mmap_handler_t`), summed across processes the router tracks | Existing `nxt_port_mmap_get_buf()` call site is already probed (`freeunit:mmap-chunk-get`, `src/nxt_router.c:7652`); segment *creation* is `freeunit:mmap-chunk-alloc` (`src/nxt_port_memory.c:318`) -- a running counter incremented there and decremented on the existing unmap path is the same pattern as the queue depth counter above. |
| `shared_memory.segment_size` / `chunk_size` | `PORT_MMAP_DATA_SIZE`, `PORT_MMAP_CHUNK_SIZE` (`src/nxt_port_memory_int.h:18-31`) | Compile-time constants (differ under `NXT_MMAP_TINY_CHUNK`); report as-built. |
| `shared_memory.chunks_total` / `chunks_free` | `nxt_port_mmap_header_t.free_map[]` (`src/nxt_port_memory_int.h:73`), a per-segment bitmap already walked by the chunk allocator | `chunks_free` needs a popcount over `free_map[]` per segment, summed; doable at `/status`-read time (bounded, `MAX_FREE_IDX` words per segment) without needing a new running counter, unlike the queue depth case where the ring's own head/tail is not safely readable cross-process without extra synchronization. |
| `shared_memory.oosm_count` | `nxt_port_mmap_header_t.oosm` (`nxt_atomic_t`, already flipped on out-of-shared-memory) | Sum (or max) across segments; already an atomic flag, so reading it for `/status` needs no new instrumentation. |
| `schedules.total` / `due` / `overdue` / `last_run_ms_ago` | The DRUPAL stream's "schedules" hook, expected near `nxt_router_conf_create` in `src/nxt_router.c` | Deliberately not designed further here: that stream owns the counters' actual field names and update points. This section only reserves the `/status` JSON shape (a `schedules` object, sibling to `applications`) so both streams can land independently without a `/status` merge conflict beyond the obvious one-line addition to `nxt_status_get()`. |

## Implementation notes (not done here)

- `nxt_status_get()`'s object-size arithmetic (`src/nxt_status.c:54`,
  `4 + (report->otel_configured != 0)`) would grow by up to two more
  conditional members (`shared_memory` always present if any segment
  exists; `schedules` present iff the schedules feature is configured),
  following the existing `telemetry` precedent exactly.
- The queue depth/high-water counters are new fields on whatever struct
  already backs `nxt_status_report_t` per app (or a new small struct
  reachable from it); populated where the USDT probes already sit, so
  landing this after (or alongside) the USDT probes is naturally a
  one-line addition per counter at each existing call site, not a new
  code path.
- `shared_memory.chunks_free`'s per-segment popcount is read-only and
  does not need a new lock: `free_map[]` is already read without
  additional synchronization by the allocator itself (it is designed to
  tolerate concurrent chunk claims), so `/status` can read the same
  bitmap the same way.
- None of this requires `--usdt`: the counters are ordinary router-side
  state, updated at the same call sites the USDT probes were added to,
  but independent of whether USDT tracing is compiled in. USDT and
  `/status` are two different consumers of the same "something happened
  here" call sites.
