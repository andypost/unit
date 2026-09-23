# Schedules

`schedules` is a top-level configuration object that makes FreeUnit issue
periodic requests to an application by itself, with no client and no sidecar
process involved. The typical use is a cron trigger for a PHP application
such as Drupal, in place of `automated_cron`, a host cron job running
`curl`, or a worker loop inside the application.

The design rationale is in [ADR 0004](adr/0004-schedules.md); this page is
the operator-facing reference.

## Configuration

```json
{
    "schedules": {
        "drupal-cron": {
            "pass": "applications/drupal/index",
            "uri": "/cron/SECRET_KEY",
            "interval": 300,
            "jitter": 15,
            "timeout": 240,
            "overlap": "skip",
            "headers": {
                "Host": "example.org"
            },
            "run_on_start": false
        }
    }
}
```

`schedules` is an object keyed by schedule name (1 to 128 printable ASCII
bytes). Each schedule has these fields:

| field          | type    | required | default    | notes |
|----------------|---------|----------|------------|-------|
| `pass`         | string  | yes      | —          | `applications/<app>` or `applications/<app>/<target>`. No variables. A route (`proxy`/an upstream) is rejected: v1 only runs schedules against an application. |
| `uri`          | string  | yes      | —          | Request target, `/path?query`, sent as a `GET`. 1–4096 bytes, must start with `/`, no whitespace, control bytes or `#`. |
| `interval`     | integer | yes      | —          | Seconds between runs, measured from one scheduled start to the next. 1 to 2147483 (about 24.8 days; timers are 32-bit millisecond values). |
| `jitter`       | integer | no       | `0`        | Up to this many seconds, uniformly random, added to each wait. `0` to `interval`; `interval + jitter` must not exceed 2147483. |
| `timeout`      | integer | no       | `interval` | Seconds before an unfinished run is abandoned. 1 to 2147483. See "Timeout" below. |
| `overlap`      | string  | no       | `"skip"`   | `"skip"` or `"queue"`. See "Overlap" below. |
| `headers`      | object  | no       | `{}`       | Extra request headers, string to string. `Host` also sets `server_name`. |
| `run_on_start` | boolean | no       | `false`    | Run about 1 s after the configuration is applied (plus jitter) instead of waiting a full interval, the first time the schedule appears. |

`GET` and `HTTP/1.1` are the only method and version in v1.

### Reserved headers

`headers` cannot set `Content-Length`, `Transfer-Encoding`, `Connection`,
`Upgrade`, `Keep-Alive`, `TE`, `Expect`, or any `Sec-WebSocket-*` name: the
run has no body and no connection, so these have nothing to act on. Header
values may not contain control characters, and all headers together are
limited to 8192 bytes.

### Drupal example

Drupal's cron endpoint is `/cron/{cron_key}`, where `cron_key` comes from
`state.get('system.cron_key')`. Drupal's `trusted_host_patterns` setting
checks the `Host` header, and rejects requests whose `Host` it does not
trust; a schedule run with no `headers.Host` set falls back to
`server_name` `localhost`, which will usually fail that check. Set
`headers.Host` to a host `trusted_host_patterns` accepts:

```json
"schedules": {
    "drupal-cron": {
        "pass": "applications/drupal/index",
        "uri": "/cron/SECRET_KEY",
        "interval": 300,
        "jitter": 15,
        "timeout": 240,
        "overlap": "skip",
        "headers": { "Host": "example.org" }
    }
}
```

Then remove `automated_cron` from Drupal's cron settings, since the
schedule replaces it.

## Semantics

### Interval and jitter

Each run is due `interval` seconds after the previous scheduled start (not
after the previous run *finished*), plus a uniformly random amount between
`0` and `jitter` seconds, recomputed on every wait. `run_on_start` makes the
first run happen about 1 second after the configuration that introduces the
schedule is applied, instead of waiting a full interval; every later run
follows the normal `interval`/`jitter` schedule.

### Overlap: skip or queue

If a run is still in flight when the next one comes due:

- `"skip"` (the default): the new run is dropped and a warning is logged.
  There is never more than one run of a given schedule in flight.
- `"queue"`: at most one run waits. It starts as soon as the in-flight run
  finishes. Further runs that come due while one is already queued coalesce
  into that single queued run — they do not pile up.

### Timeout

Each schedule has its own `timeout`, independent of the application's
`limits.timeout`. When a run's `timeout` expires:

- if the request is still queued for a process, it is retracted;
- if a worker has claimed it but not yet started, it is left alone;
- if a worker is actively running it, that worker is *abandoned*: it is
  not stopped, but it is also not returned to the idle pool, and its answer
  (if any) is discarded;
- the schedule then answers itself with a synthetic `503` for logging
  purposes and is free to run again at its next interval.

## Caveats

- **A timeout does not stop the application.** `timeout` only frees the
  *schedule* to run again; it never signals or kills the worker process
  running the overdue request. A 10-minute cron job with a 4-minute
  `timeout` keeps running in the background for the full 10 minutes, and
  that worker still counts against the application's `processes` capacity
  for as long as it runs and does not answer. Keep the schedule `timeout`
  at or below the application's `limits.timeout`, and give the application
  enough spare `processes` that a long-running schedule does not starve
  ordinary visitor traffic.

- **The access log contains the full URI, including any secret in it.** A
  schedule run goes through the same access log as a client request, and
  the default format logs the request line verbatim — so
  `/cron/SECRET_KEY` appears in the log exactly as configured. If the log
  is shared with other tooling or shipped off-host, either use a
  restrictive `access_log` `format` that omits `$request_line`/`$uri`, or
  give the schedule's application (or its listener/route) its own
  route-level access log that a wider audience does not see. FreeUnit's own
  `info`-level log line abbreviates the URI to everything up to the last
  `/`, plus `…`; the full URI is only logged at `debug`.

- **`fastcgi_finish_request()` makes a run "finish" early.** If the
  application sends its response and keeps running afterwards — which is
  exactly what PHP's `fastcgi_finish_request()` does, and what Drupal's own
  `automated_cron` relies on — the schedule considers the run complete as
  soon as the response is sent, not when the application actually stops
  working. This means `overlap: "skip"` does not protect against
  overlapping with that trailing work: a second run can start, and be
  counted as non-overlapping, while the first run's worker is still busy
  behind the scenes. `/cron/{cron_key}` itself does not call
  `fastcgi_finish_request()`, so this caveat matters most for schedules
  that target endpoints modeled after `automated_cron`, not the dedicated
  cron endpoint.

- **Schedule state does not survive a router restart.** Whether a run is in
  flight, and any `queue`d run, is kept only in memory. After a router
  restart, a `run_on_start` schedule fires again as if it were new, even if
  a run from before the restart is still executing as an orphaned worker.

- **Runs count in `/status` `requests.total`.** A schedule run is an
  ordinary application request as far as accounting is concerned: it
  increments the same request counter a client request would. Per-schedule
  counters (runs, skipped, failed, timed out) are a separate, later
  addition to `/status`.
