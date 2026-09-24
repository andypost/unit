# 0004. `schedules`: periodic internal requests to an application

- Status: proposed
- Date: 2026-09-23
- Stream: Drupal and debugging (Day 1, design only; the C work starts on Day 3)
- Code base: every `file:line` below refers to commit `eb1767c9`. The code
  stream is changing `src/nxt_router.c` and `src/nxt_conf_validation.c` this
  week (#431, `prepare_msg`), so re-check each line before editing.

## Context and problem statement

Drupal, like most PHP applications, needs a periodic trigger: cron. Today a
FreeUnit deployment has three ways to get one, and none is good:

1. **`automated_cron`**, which runs cron at the end of an ordinary visitor
   request. It works on FreeUnit because `fastcgi_finish_request()` exists
   (`src/nxt_php_sapi.c:228`): the visitor gets the response and the worker
   runs cron afterwards. But it needs traffic, it runs at random moments, and
   it holds an application process that FreeUnit counts as detached.
2. **A host or sidecar cron** that runs `curl` or `drush cron`. That is one
   more moving part in every container, and it does not follow the FreeUnit
   configuration.
3. **A worker loop inside the application.** This is not an option for PHP.

We want FreeUnit to issue the request itself, on a schedule set in the same
JSON as the application:

```json
"schedules": {
    "drupal-cron": {
        "pass": "applications/drupal/index",
        "uri": "/cron/SECRET_KEY",
        "interval": 300,
        "jitter": 15,
        "timeout": 240,
        "overlap": "skip",
        "headers": { "Host": "example.org" },
        "run_on_start": false
    }
}
```

The question this ADR answers is how to inject a request that has no client
connection into the router's application path without breaking the many
places that assume there is one.

## Decision drivers

- **No new failure modes on the hot path.** Listener requests must not become
  slower or riskier. A NULL check added to every `r->conf` dereference is not
  acceptable.
- **Reuse the existing lifetime rules.** Configuration objects stay alive
  because of reference counts (`joint → skcf → rtcf`). A synthetic request
  must hold the same references a real one holds, or reconfiguration will
  free memory under it.
- **Same application semantics as a real request.** Accounting, `limits`,
  the request timeout, the app queue, OTel and the access log should all work
  as they do for a listener request.
- **Reviewable in about two days** (Day 3–4), with unit tests in the style of
  `src/test/nxt_router_*_test.c`.

## Considered options

1. **Synthetic request through a "devnull" HTTP protocol and an internal
   joint** (chosen).
2. **Synthetic request with `r->proto.any = NULL` and `r->conf = NULL`**, with
   NULL checks added where it crashes. This is the approach first sketched in
   the factory plan, section 5.
3. **Loopback HTTP client**: the router connects to its own listener, using
   the upstream/proxy peer code (`nxt_h1p_peer_connect`). This is plan B.
4. **External timer**: a script in the image runs `curl --unix-socket` or
   `curl http://localhost`. This is the fallback that ships no C code.

## Decision outcome

Chosen: **option 1**. Option 2 does not work as written (see §6.1): with a
NULL protocol the request never gets past `nxt_http_request_start()`, never
completes, and leaks its pool. Making it work would need the same handlers
option 1 adds, but spread as special cases across the HTTP code. Option 3 is
kept as plan B (§10).

### 1. Configuration shape

The top level gains a `schedules` object, keyed by schedule name:

| field          | type    | required | default  | meaning |
|----------------|---------|----------|----------|---------|
| `pass`         | string  | yes      | —        | `applications/<app>` or `applications/<app>/<target>`. v1 accepts nothing else (§2). |
| `uri`          | string  | yes      | —        | request target, `/path?query`. Sent as a `GET`. |
| `interval`     | integer | yes      | —        | seconds between runs, measured from one scheduled start to the next. |
| `jitter`       | integer | no       | `0`      | up to this many seconds, uniformly random, added to each wait. |
| `timeout`      | integer | no       | `interval` | seconds before an unfinished run is abandoned (§7.2). |
| `overlap`      | string  | no       | `"skip"` | `"skip"`: a run that comes due while the previous one is still running is dropped and logged. `"queue"` (at most one run waits) shipped in 1.36.2 and was removed afterwards: see "Amendments" below. |
| `headers`      | object  | no       | `{}`     | extra request headers, string to string. `Host` also sets `server_name`. |
| `run_on_start` | boolean | no       | `false`  | when the schedule first appears, run it about 1 s after the configuration is applied (plus jitter), instead of waiting a full interval. |

`GET` and `HTTP/1.1` are fixed in v1. There is no request body, so methods
other than `GET` are not useful yet. A `method` field can be added later
without breaking anything.

### 2. Validation rules

These go in `src/nxt_conf_validation.c`, **after** #431 lands. The new root
member sits next to `applications` in `nxt_conf_vldt_root_members`
(`src/nxt_conf_validation.c:324-356`), as `NXT_CONF_VLDT_OBJECT` with
`nxt_conf_vldt_object_iterator`. A `nxt_conf_vldt_schedule_members[]` table
follows the pattern of `nxt_conf_vldt_app_limits_members`
(`src/nxt_conf_validation.c:1370`).

- **Name**: non-empty. The same rules as an application name.
- **`pass`**: must be a string with no variables (`NXT_CONF_VLDT_TSTR` is not
  set, unlike the listener's `pass` at `src/nxt_conf_validation.c:540-543`).
  It is first checked by `nxt_conf_vldt_pass()`
  (`src/nxt_conf_validation.c:2451`), which already confirms that the
  application, and the target if given, exist. Then its first segment must be
  `applications`. `routes/...` and `upstreams/...` are rejected with a clear
  message: a route can lead to `proxy` or an upstream, and those call
  `nxt_http_proto[r->protocol].peer_*`, which the devnull protocol does not
  provide (§6.4). The upstream handler also indexes `r->conf->upstreams`
  (`src/nxt_upstream.c:146`), which the internal joint does not fill.
- **`uri`**: a string of 1 to 4096 bytes that starts with `/`, with no
  whitespace, control bytes or `#`. The router then parses it with the real
  request-line parser (§6.5), so percent-encoding and dot segments are
  handled exactly as for a client request. The parse is repeated in the
  validator so that a target the router would reject fails the `PUT`, not the
  apply.
- **`interval`**: an integer from 1 to 2147483. Timers are `nxt_msec_t`
  (`uint32_t`, `src/nxt_time.h:93`), and the timer tree compares them with
  `nxt_msec_diff()` as a signed 32-bit difference (`src/nxt_time.h:103`,
  `src/nxt_timer.c:65`), so about 24.8 days is the ceiling. Longer schedules (weekly or monthly) would need
  re-arming in steps. That is out of scope for v1.
- **`jitter`**: an integer, 0 ≤ `jitter` ≤ `interval`.
- **`timeout`**: an integer from 1 to 2147483.
- **`overlap`**: `"skip"` or `"queue"`.
- **`headers`**: an object whose names are HTTP tokens and whose values are
  strings with no CR, LF or NUL. These names are reserved and rejected:
  `Content-Length`, `Transfer-Encoding`, `Connection`, `Upgrade`,
  `Keep-Alive`, `TE`, `Expect`, and `Sec-WebSocket-*`. There is no body, and
  the h1 field handlers for those names assume an h1 connection
  (`src/nxt_h1proto.c:160-182`); `Content-Length` goes through
  `nxt_http_request_content_length()`, which reads
  `r->conf->socket_conf->max_body_size` (`src/nxt_http_request.c:234`). All
  headers together must stay under 8 KiB; the whole request must fit
  `PORT_MMAP_DATA_SIZE` (`src/nxt_router.c:7639`).
- **`run_on_start`**: a boolean.

The OpenAPI document (`docs/unit-openapi.yaml`) gets the matching schema and
`/config/schedules{,/name}` paths. The controller needs no change, because
configuration paths are generic.

### 3. Router data structures

```c
/* One per schedule per configuration.  Allocated from rtcf->mem_pool,
 * so it lives exactly as long as the configuration that defined it. */
typedef struct {
    nxt_str_t                name;
    nxt_http_action_t        *action;     /* resolved "pass" */
    nxt_str_t                uri;
    nxt_array_t              *headers;    /* of nxt_http_name_value_t */
    nxt_msec_t               interval;
    nxt_msec_t               jitter;
    nxt_msec_t               timeout;
    uint8_t                  overlap;     /* NXT_SCHEDULE_SKIP | _QUEUE */
    uint8_t                  run_on_start;
    nxt_router_schedule_state_t  *state;  /* set on activation */
} nxt_router_schedule_t;

/* In nxt_router_conf_t (src/nxt_router.h:52-78): */
    nxt_router_schedule_t    *schedules;
    uint32_t                 nschedules;
    nxt_socket_conf_joint_t  *schedule_joint;   /* §6.3, NULL if none */

/* Router-global, survives reconfiguration.  malloc()ed, not pooled.
 * Touched only on the main engine (the router's configuration thread). */
typedef struct {
    nxt_queue_link_t         link;        /* in nxt_router_t.schedules */
    nxt_str_t                name;        /* key */
    nxt_router_schedule_t    *conf;       /* current config, or NULL */
    nxt_timer_t              timer;       /* on the main engine */
    nxt_msec_t               next_due;    /* engine->timers.now based */
    uint8_t                  running;     /* a run is in flight */
    uint8_t                  pending;     /* "queue": one run waiting */
    uint8_t                  armed;
    uint32_t                 runs, skipped, failed, timed_out;
    nxt_msec_t               last_duration;
    nxt_http_status_t        last_status;
} nxt_router_schedule_state_t;
```

`nxt_router_t` (`src/nxt_router.h:41-49`) gains `nxt_queue_t schedules`.
These counters are what the observability stream's `/status` extension (N4)
will read.

**Parsing** goes in `nxt_router_conf_create()` (`src/nxt_router.c:2528`),
after `nxt_http_routes_resolve()` (`src/nxt_router.c:3078`).
`nxt_http_action_create()` resolves an `applications/...` pass immediately
against `rtcf->apps_hash` (`src/nxt_http_route.c:1549-1577`), so it must run
after the applications loop (`src/nxt_router.c:2638-2873`). The parse only
allocates from `rtcf->mem_pool`. It takes no references and arms no timers,
because `nxt_router_conf_error()` destroys that pool unconditionally
(`src/nxt_router.c:2201`), and anything that escaped the pool would dangle.

### 4. Activation and reconfiguration: carrying `running`

Activation runs in `nxt_router_conf_apply()`, on the main thread, after
`nxt_router_engines_post()` (`src/nxt_router.c:2061`) and before
`nxt_router_conf_ready()` (`src/nxt_router.c:2074`). By then the new engine
list is final and the configuration can no longer fail.

1. For each `nxt_router_schedule_t` in the new rtcf, look up the state by
   name in `nxt_router->schedules`. If it is found, repoint `state->conf`.
   The `running` and `pending` flags and `next_due` carry over, so a run in
   flight during a reconfiguration still blocks overlap. If it is new,
   create the state and arm its first run: interval plus jitter, or about
   1 s when `run_on_start` is set.
2. If the interval changed, re-arm from the old run's start time, not from
   now. That way a change such as `jitter` alone does not reset the clock.
3. A state whose name is gone gets `conf = NULL` and its timer deleted. It
   is freed right away if not `running`, otherwise when that run completes.
4. The new rtcf's `schedule_joint` is created (§6.3). The old rtcf's joint
   gets its base reference released through a job posted to its engine.

A run in flight keeps the *old* configuration alive through its joint
reference (§6.3), exactly as an in-flight listener request does. Its
completion is reported to the state object, which does not belong to any
configuration.

### 5. The timer

The timer follows the application idle timer pattern:
`app_joint->idle_timer` is set up with `bias`, `work_queue =
&engine->fast_work_queue`, `handler`, `task = &engine->task` and `log`
(`src/nxt_router.c:2836-2840`), on `app->engine`, which is the main engine
(`src/nxt_router.c:2814-2816`). The schedule's `state->timer` is initialised
the same way on the same engine, so every state transition happens on one
thread and needs no lock.

Teardown copies `nxt_router_free_app()`. `nxt_timer_delete()` can return
non-zero while a queued change still refers to the timer; the code then
re-arms at 0 with a release handler instead of freeing
(`src/nxt_router.c:7295-7300`; the reasoning is spelled out at
`src/nxt_router.c:840-855`).

When the timer fires:

- If `running` is set: with `skip`, count `skipped`, log a warning and
  re-arm. With `queue`, set `pending`.
- Otherwise set `running`, record the start time, re-arm for the next
  interval, and **post** the run to a worker engine
  (`nxt_event_engine_post()`, `src/nxt_event_engine.c:236`).

**The run must not execute on the main engine.** The main router port uses
`nxt_router_process_port_handlers`, whose `.data` handler is
`nxt_router_conf_data_handler` (`src/nxt_router.c:437-453`). An
application's reply arriving there would be parsed as a new configuration.
Only worker-engine ports get `nxt_router_app_port_handlers`, which route
`.data` and `.req_headers_ack` to `nxt_port_rpc_handler`
(`src/nxt_router.c:4669-4675`, enabled at `:4730`). The run therefore goes
to the engine that owns the rtcf's `schedule_joint`: the first entry of
`router->engines`, which lists only worker engines (`src/nxt_router.c:4640`).
Spreading runs over several engines is possible later, with one joint per
engine.

### 6. The synthetic request

#### 6.1 Why `r->proto.any = NULL` does not work

`nxt_http_request_create()` (`src/nxt_http_request.c:249-305`) needs no
connection, and several protocol calls are guarded by `proto.any != NULL`.
But the guards cause silent stalls:

| step | code | effect of a NULL protocol |
|---|---|---|
| start | `nxt_http_request_start()` calls `nxt_http_request_read_body()`, which does nothing (`src/nxt_http_request.c:316-345`, `:677-683`) | `ready_handler` is never called, so the request never reaches the action |
| local address | `nxt_http_request_proto_info()` is guarded (`src/nxt_http_request.c:668-673`), called from `nxt_http_application_handler()` (`:643-665`) | `r->local` stays NULL, and `nxt_router_prepare_msg()` dereferences it (`src/nxt_router.c:7619-7620`, `:7679-7684`) |
| response headers | `nxt_http_request_header_send()` returns without calling `body_handler` (`src/nxt_http_request.c:816-818`); the router calls it at `src/nxt_router.c:5564` | the body is never sent, and `r->last`, the sync buffer whose completion ends the request (`src/nxt_router.c:7452-7454`), is never completed |
| close | `nxt_http_request_close_handler()` releases `r->mem_pool` only when `proto.any != NULL` (`src/nxt_http_request.c:1134-1140`) | the request pool leaks, and with it the reference the joint must drop |
| `$body_bytes_sent` | calls `nxt_http_proto[r->protocol].body_bytes_sent` without a guard (`src/nxt_http_variables.c:445`) | NULL call: a crash as soon as the access log uses the default format (`src/nxt_router_access_log.c:168`) |
| `$response_connection`, `$response_transfer_encoding` | dereference `r->proto.h1` (`src/nxt_http_variables.c:529`, `:599`) | NULL dereference |

#### 6.2 Every `r->conf` dereference

Found with `grep` over `src/nxt_http*.c`, `src/nxt_router*.c`,
`src/nxt_upstream.c` and `src/nxt_h1proto_websocket.c`. `src/nxt_router.c`
has no `r->conf` itself. None of these sites checks for NULL, so
`r->conf == NULL` is not an option.

| site | function | reached by a schedule run? |
|---|---|---|
| `src/nxt_http_request.c:234` | `nxt_http_request_content_length` | no: `Content-Length` is reserved (§2) |
| `src/nxt_http_request.c:328` | `nxt_http_request_start` (`forwarded`, `client_ip`) | no: the run calls `nxt_http_request_action()` directly (§6.5); both would be NULL anyway |
| `src/nxt_http_request.c:595` | `nxt_http_request_ready` (`socket_conf->action`) | no, as above |
| `src/nxt_http_request.c:766` | `nxt_http_request_header_send` (`server_version`) | **yes**, on every response |
| `src/nxt_http_request.c:1102-1103` | `nxt_http_request_close_handler` (`router_conf`, access log) | **yes**, on every run |
| `src/nxt_http_request.c:1558` | `nxt_http_cond_value` (access log `if`) | **yes**, if `access_log.if` is set |
| `src/nxt_http_set_headers.c:203` | `nxt_http_set_headers` | **yes**, via `header_send` (returns before this line when the action has no `response_headers`) |
| `src/nxt_http_compression.c:172`, `:873` | compression config and check | **yes**, from the response handler (`src/nxt_router.c:5548-5556`) |
| `src/nxt_router_access_log.c:279`, `:310` | access log text/JSON | **yes**, when an access log is set |
| `src/nxt_http_rewrite.c:47` | rewrite | no: `r->action` is NULL (`src/nxt_http_rewrite.c:37-40`) |
| `src/nxt_http_route.c:1379`, `:1419` | `pass` with variables | no: variables are rejected (§2) |
| `src/nxt_http_return.c:124`, `src/nxt_http_static.c:397`, `:511`, `src/nxt_http_js.c:387` | return, share, njs | no: routes are rejected (§2) |
| `src/nxt_upstream.c:146` | upstream handler | no: upstreams are rejected (§2) |
| `src/nxt_h1proto_websocket.c:104`, `:360` | websocket | no: `Upgrade` is reserved |

So a run needs a **real, fully populated `r->conf`**. Its `socket_conf`
must have a valid `router_conf` and sane defaults (`server_version`,
buffers). That is the internal joint.

#### 6.3 The internal joint and its reference counts

Configuration memory is freed by reference counts, and the chain is:

- a request holds its `nxt_socket_conf_joint_t`: `joint->count++` in h1
  (`src/nxt_h1proto.c:535`), released by `nxt_router_conf_release()` in the
  protocol's close (`src/nxt_h1proto.c:1906`);
- a joint holds its `nxt_socket_conf_t` (`skcf->count++`,
  `src/nxt_router.c:4477`);
- a socket configuration holds the `nxt_router_conf_t`
  (`skcf->router_conf->count++`, `src/nxt_router.c:3063`);
- `nxt_router_conf_release()` (`src/nxt_router.c:5229-5292`) walks the chain
  down under `router->lock` and destroys the rtcf pool at zero (`:5290`).

Two facts make this non-optional:

- **An rtcf with no listeners dies at once.** `nxt_router_conf_ready()`
  destroys it when `rtcf->count == 0` (`src/nxt_router.c:2138-2150`), and
  only listeners raise that count. A configuration with `applications` and
  `schedules` but no `listeners` would free the schedule's action under its
  timer.
- **A worker engine exits once its joint queue is empty.** See
  `nxt_router_worker_thread_quit()` (`src/nxt_router.c:4913`) and
  `nxt_router_listen_event_release()` (`:5222`). A run on an engine that is
  being removed (a `listen_threads` decrease) must hold that engine open.

Design: per rtcf that has schedules, one internal `nxt_socket_conf_t` and
one `nxt_socket_conf_joint_t`, both from `rtcf->mem_pool`.

- `skcf`: copy the listener defaults block (`src/nxt_router.c:2942-2965`) and
  map `settings.http` over it (`:2969-2978`), so that `server_version`,
  `discard_unsafe_fields` and the buffer sizes match a listener's. Set
  `skcf->router_conf = rtcf`, `action = NULL`, `listen = NULL`, and
  `nxt_queue_self(&skcf->link)`. That last step matters:
  `nxt_router_conf_release()` does `nxt_queue_remove(&skcf->link)`
  (`src/nxt_router.c:5262`), and that macro NULLs the neighbours'
  pointers (`src/nxt_queue.h:134-140`). A self-linked node survives it; a
  zeroed one crashes.
- At activation (§4), under `router->lock`: `rtcf->count++` and
  `skcf->count = 1`.
- `joint`: `count = 1` (the schedule set's base reference), `socket_conf =
  skcf`, `engine = E`, `upstreams = NULL`. A job posted to E inserts it into
  `E->joints`, as `src/nxt_router.c:4781` and `:4842` do for listeners, so E
  cannot exit under a run.
- Each run: `joint->count++` on E. Released by the devnull `close`
  (§6.4) through the ordinary `nxt_router_conf_release()`, followed by the
  same `engine->shutdown && nxt_queue_is_empty(&engine->joints)` exit check
  as `src/nxt_router.c:5222`.
- Deactivation (the next reconfiguration, or none left): post a job to E
  that drops the base reference, as the listener close does. The last
  release frees the rtcf.

`joint->count` is not atomic, which is why the joint lives on one engine and
is only touched there. `skcf->count` and `rtcf->count` change only under
`router->lock`, as they already do.

#### 6.4 A devnull HTTP protocol

`nxt_http_protocol_t` already reserves `NXT_HTTP_PROTO_DEVNULL`
(`src/nxt_http.h:71`), and `nxt_http_proto[3]` has room for it, but the slot
is empty (`src/nxt_h1proto.c:135-157`). It was left by the 2019 refactor
(commit `17bb22a4`) and never filled. The run sets `r->protocol =
NXT_HTTP_PROTO_DEVNULL` and points `r->proto.any` at its per-run object
`nxt_router_schedule_run_t`. Every guarded call site then does the right
thing without being edited:

| slot | devnull behaviour |
|---|---|
| `body_read` | no body: `r->state->ready_handler(task, r, NULL)`, as h1 does (`src/nxt_h1proto.c:1099-1101`) |
| `local_addr` | nothing to do: `r->local` is preset (§6.5) |
| `header_send` | count the header bytes, then call `body_handler(task, r, data)` directly |
| `send` | add the buffer sizes to `run->body_bytes`, keep the first 256 body bytes for the log, then complete the whole chain with `nxt_sendbuf_drain(task, &engine->fast_work_queue, out)`, as the router's error path does (`src/nxt_router.c:5671`). This completes `r->last`, and the router's own completion handler (`nxt_router_http_request_done`, `src/nxt_router.c:7482-7496`) closes the request. |
| `body_bytes_sent` | return `run->body_bytes` |
| `discard` | complete `last` and anything queued; set `run->failed` |
| `close` | stop `run->timer`, `nxt_router_conf_release(task, joint)`, run the engine-exit check, then post the result (status, duration, bytes) to the main engine, where the state object clears `running` and fires a pending `queue` run |
| `peer_*`, `ws_frame_start` | `NULL`; the validator keeps the run away from them (§2) |

`nxt_http_request_close_handler()` then releases `r->mem_pool` because
`proto.any != NULL` (`src/nxt_http_request.c:1134-1140`). The pool's second
reference, taken in `nxt_router_process_http_request()`
(`src/nxt_router.c:7431`), goes away through the usual
`nxt_router_http_request_release_post()` (`:7974-7978`).

The variables that dereference `r->proto.h1` without checking the protocol
(`src/nxt_http_variables.c:529`, `:599`) get a `r->protocol ==
NXT_HTTP_PROTO_H1` guard. That is a two-line fix, and a latent bug for any
future H2 too.

#### 6.5 Building the request

This runs on engine E, in the posted handler:

1. `r = nxt_http_request_create(task)`. This also counts the run in
   `engine->requests_cnt` (`src/nxt_http_request.c:283`), which `/status`
   reports as a request. That is acceptable, and it should be documented.
2. `r->conf = joint; joint->count++`, `r->protocol = DEVNULL`,
   `r->proto.any = run`.
3. Build `"GET <uri> HTTP/1.1\r\n" <headers> "\r\n"` into a buffer from
   `r->mem_pool` and run `nxt_http_parse_request()` and
   `nxt_http_parse_fields()` (`src/nxt_http_parse.h:212-217`) on a
   `nxt_http_request_parse_t` allocated from `r->mem_pool`. Then copy the
   results the way `nxt_h1p_header_process()` does
   (`src/nxt_h1proto.c:686-705`): `target`, `quoted_target`, `version`,
   `method`, `path`, `args`, and the inline fields. This gives the same
   normalisation of `%xx` and dot segments as a client request. The parser
   is also the only code that fills `r->path` and `r->args` consistently
   with `r->target`, which `nxt_router_prepare_msg()` relies on when it
   computes the query offset (`src/nxt_router.c:7714-7718`).
4. Process the fields with a devnull-specific hash that holds only the
   protocol-neutral handlers from the h1 table: `Host`
   (`nxt_http_request_host`, `src/nxt_http_request.c:86`), `Cookie`,
   `Referer`, `User-Agent`, `Content-Type`, `Authorization`, and with OTel
   `Traceparent`/`Tracestate` (`src/nxt_h1proto.c:167-182`). `Connection`,
   `Upgrade` and `Transfer-Encoding` are excluded; they write into
   `r->proto.h1`.
5. Add `User-Agent: FreeUnit-Schedule/<name>` unless the configuration sets
   one.
6. `r->remote` and `r->local`: two `nxt_sockaddr_t` built once per joint
   with `nxt_sockaddr_parse()` (as `src/nxt_router.c:3706` does):
   `127.0.0.1` and `127.0.0.1:80`. **The local port must not be 0.**
   Symfony, and therefore Drupal, falls back to `SERVER_PORT` when `Host`
   has no port, and would build `example.org:0` URLs.
7. Call `nxt_http_request_action(task, r, sched->action)` directly
   (`src/nxt_http_request.c:612`). This skips `nxt_http_request_start()`
   and `_ready()`, which would read `socket_conf->action`, a single value
   per skcf, while each schedule has its own. The application handler sets
   `server_name` from `r->host`, or to `localhost`
   (`src/nxt_http_request.c:655-660`), and hands the request to
   `nxt_router_process_http_request()`.

Everything `nxt_router_prepare_msg()` reads is then set: `method`,
`version`, `remote`, `local` (address and port), `server_name`, `target`,
`path`, `args`, the fields, `content_length_n = -1` (from the create call),
`tls = 0` and `app_target` (`src/nxt_router.c:7615-7722`).

### 7. Completion, timeout and logging

#### 7.1 Completion

The run ends in the devnull `close` (§6.4). The result is posted to the
main engine, where the state object records `last_status`,
`last_duration` and the counters, clears `running`, and, when `pending` is
set, starts the queued run.

The status is whatever the application returned. A 503 from the
application's own `limits.timeout` (`src/nxt_router.c:7885-7971`) or from a
failed dispatch (`src/nxt_router.c:7463-7479`) counts as `failed`.

#### 7.2 The schedule `timeout`

The run has **its own timer** (`run->timer`, on engine E). It cannot share
`r->timer`: that timer already carries the application timeout
(`src/nxt_router.c:5419-5421`, `:7381-7384`) and, at the end, the pool
release (`:7974-7978`).

On expiry the run must do exactly what `nxt_router_app_timeout()` does
(`src/nxt_router.c:7885-7971`):

- retract the message if it is still queued (`nxt_router_msg_retract`,
  `:1185`);
- leave a claimed-but-unacknowledged request alone;
- `nxt_router_app_abandon()` (`:6709`) a request a worker is running, so
  its port does not return to the idle pool while PHP is still executing;
- answer with `nxt_http_request_error(503)` and unlink.

That function is static, and its logic is subtle, so the plan is to factor
its body into `nxt_router_request_expire(task, r, req_rpc_data)` and call it
from both places. This is a small, mechanical change inside `nxt_router.c`,
and it must be ordered after the code stream's `prepare_msg` work.

**What a timeout cannot do:** it does not stop PHP. A worker running a
10-minute cron keeps running. It is abandoned: it stays counted, and its
port is withheld until it answers. The schedule timeout only frees the
schedule so that the next interval can run. Operators should keep the
schedule `timeout` at or below the application's `limits.timeout`, and give
the application enough `processes` that cron does not starve visitors. The
documentation will say this.

#### 7.3 Logging

- `info`: `schedule "drupal-cron" run 42: GET /cron/… -> 204 in 812 ms`.
- `warn`: skipped because the previous run is still running; timed out
  after N s; a status of 400 or more, with the first 256 body bytes.
- `alert`: the dispatch failed (no memory, no engine).
- `debug`: the timer arming, with the chosen jitter.
- **The access log**: runs go through `nxt_http_request_close_handler()`,
  so an access log configured on the router records them, with remote
  `127.0.0.1` and user agent `FreeUnit-Schedule/<name>`.
  `$body_bytes_sent` works because devnull implements it.
- **Secrets**: `uri` usually carries the cron key
  (`/cron/SECRET_KEY`). The `info` line logs only the path up to the last
  `/` plus `…`. The full URI is logged only at `debug`. The access log will
  contain it, as it would for a client request; the documentation says so.

### 8. Consequences

Positive:

- One JSON object replaces a sidecar cron. The run is an ordinary
  application request: `limits`, processes, the application queue, OTel
  spans (`r->otel` is created in `nxt_http_request_create()`,
  `src/nxt_http_request.c:288-293`) and the access log all apply.
- The lifetime handling reuses `nxt_router_conf_release()` unchanged. No
  new reference-count scheme.
- Filling the reserved devnull slot gives the router a general way to issue
  internal requests: warm-up requests and health probes later.

Negative, and the risks:

- **R1. The shared `nxt_router.c`.** The code stream is changing
  `prepare_msg` and #431 now. The schedule change touches `conf_create`,
  `conf_apply`, the app-timeout factoring and a new devnull table. Keep the
  schedule code in a new file, `src/nxt_router_schedule.c`, with a narrow
  header, and keep the `nxt_router.c` diff to about 60 lines of hooks.
- **R2. Engine removal.** Inserting the joint into `E->joints` should keep a
  quitting engine alive until the run ends. This is unverified; test it
  (§9).
- **R3. `fastcgi_finish_request()`.** If the application finishes the
  response early and carries on (`src/nxt_php_sapi.c:228`; Drupal's
  `automated_cron` does this, while `/cron/{key}` does not), the run
  "completes" while work continues, and `overlap: skip` protects nothing.
  Document it. A later version could hold `running` until the detached
  edge (`nxt_router_detached_handler`) is cleared.
- **R4. The `Host` header and `trusted_host_patterns`.** Drupal answers 400
  to a `Host` its settings do not trust. The run's default `Host` is
  absent, which yields `server_name` `localhost`. The documentation example
  sets `headers.Host`.
- **R5. Capacity.** A cron run occupies one application process for its
  duration. With `processes.max: 1`, cron blocks the site.
- **R6. Timer range.** The limit is 24.8 days (§2). Longer schedules are
  rejected, not silently truncated.
- **R7. A restart loses the state.** `running` survives reconfiguration but
  not a router restart. After a restart, `run_on_start` fires again, and an
  interval run may overlap a still-running orphan worker from before.
- **R8. Status accounting.** Runs count in `/status` `requests.total`
  (`src/nxt_router.c:1741`). N4 adds separate `schedules` counters.

### 9. Implementation plan, Day 3–4

Each step builds (`make -j2`), and `make tests` still passes.

**Day 3: configuration and scaffolding** (after #431 is merged):

1. Validator: the `schedules` member and the member table (§2).
   pytest `test/test_schedules.py::test_schedules_validation*` cases:
   - accepted: a minimal schedule; all fields; a `pass` to an application
     target;
   - rejected: a missing `pass`, `uri` or `interval`; `pass` to `routes`,
     to `upstreams` or to a missing application or target; a variable in
     `pass`; a `uri` without a leading `/`, or with a space, `#` or a
     control byte; `interval` of 0, negative or 2147484; `jitter` greater
     than `interval`; a bad `overlap`; a reserved header; a header value
     containing CR or LF; oversized headers; an unknown member.
2. `docs/unit-openapi.yaml` schema; a `docs/changes.xml` entry.
3. `src/nxt_router_schedule.{c,h}`: the structures, and the parse called
   from `nxt_router_conf_create()`. A configuration with schedules applies
   and nothing fires yet.
4. The devnull table and the internal joint (§6.3, §6.4). C unit test
   `src/test/nxt_router_schedule_test.c`, registered in
   `src/test/nxt_tests.c` like `nxt_router_app_timeout_test`:
   - **joint refcount**: an rtcf with no listener and one schedule joint
     stays alive after `nxt_router_conf_ready()`, and is destroyed exactly
     when the base reference and one run reference are both released, in
     either order;
   - **skcf self-link**: `nxt_router_conf_release()` on the internal joint
     does not touch `router->sockets`;
   - **devnull send**: a chain ending in the sync `last` buffer is drained,
     `body_bytes_sent` equals its size, and `last`'s completion handler runs
     exactly once;
   - **request build**: a URI with `%2F`, `..` and a query string yields
     the same `path`, `args` and `target` as the h1 parser, and the
     `nxt_unit_request_t` from `nxt_router_prepare_msg()` has a non-empty
     `local_port` of `80`;
   - **reserved headers** never reach the h1 handlers (the hash has no
     `Connection` entry).

**Day 4: timer, completion, timeout:**

5. The timer on the main engine, and the post to E. C test: with a fake
   clock, and the timer and the engine driven as in
   `src/test/nxt_router_app_timeout_test.c`, `skip` drops the run that
   falls due while one is running, `queue` coalesces two due runs into one,
   and jitter stays within `[0, jitter]`.
6. Completion posting and the state object. Factor
   `nxt_router_request_expire()` out of `nxt_router_app_timeout()`. C test:
   the schedule timeout on a queued message retracts it; on a claimed one
   it waits; on an acknowledged one it abandons the port. These mirror the
   three existing app-timeout cases.
7. Reconfiguration: carrying state by name, deletion, and the engine-exit
   check.
8. pytest `test/test_schedules.py`, with a Python application that appends
   `time, REQUEST_URI, HTTP_HOST, HTTP_USER_AGENT` to a file and can be
   told to sleep:
   - `test_schedules_fires`: interval 1, wait for three records at about
     1 s spacing, with the right URI, `Host` and user agent;
   - `test_schedules_run_on_start`: the first record within about 1.5 s;
   - `test_schedules_jitter_bounds`: spacing within `[interval, interval +
     jitter]`, with a margin;
   - `test_schedules_overlap_skip`: the application sleeps 2.5 s with
     interval 1; the log shows skips, and there is never more than one
     concurrent run;
   - `test_schedules_overlap_queue`: the same, with a queued run starting
     right after the previous one ends, and no pile-up;
   - `test_schedules_timeout`: the application sleeps longer than
     `timeout`; the log shows a timeout, and the next interval still runs;
   - `test_schedules_reconfigure_running`: `PUT` a changed `jitter` while a
     run is in flight; no duplicate run, and no crash under ASan;
   - `test_schedules_no_listeners`: a configuration with only
     `applications` and `schedules` still fires (the rtcf-lifetime case);
   - `test_schedules_remove`: deleting `/config/schedules/x` stops the runs;
     a run in flight still completes and is logged;
   - `test_schedules_listen_threads`: change `settings.listen_threads` while
     a run is in flight (risk R2);
   - `test_schedules_access_log`: the default-format access log records the
     run, which proves `$body_bytes_sent` does not crash;
   - `test_schedules_app_timeout_503`: the application's `limits.timeout`
     shorter than its sleep; the run is logged as `failed` with 503.
9. The Drupal image: an example `schedules` block in the README and in
   `pkg/docker/drupal/unit-drupal.json` behind an environment switch, since
   the cron key is per site. Demo: Drupal cron on FreeUnit, with no
   `automated_cron`.

### 10. Plan B

Plan B takes over if Day 3 shows that devnull and the joint are too invasive
for the shared `nxt_router.c`, or if R2 cannot be closed.

**B1: a loopback client inside the router.** The same timer and state (§4,
§5). The run opens an HTTP/1.1 connection to one of the router's own
listeners through the peer machinery (`nxt_h1p_peer_connect` and friends,
`src/nxt_h1proto.c:146-150`), the same code the `proxy` action uses. The
request is real. Everything in §6 disappears, and the joint is the
listener's own.

It costs a listener to target, so `pass` becomes a listener address and a
URI rather than an application. Each run costs one TCP connection. The
access log shows it as a normal client. And `nxt_http_peer_t` is
request-bound (`r->peer`), so a "client without a server-side request"
still needs a small holder object. It is more code in `nxt_http_proxy.c`,
less in the router core.

**B2: an external timer, with no C change.** In the Drupal image,
`drupal-dev.sh` (or a `/docker-entrypoint.d/*.sh` hook) starts a background
loop:

```sh
while sleep "$CRON_INTERVAL"; do
    curl -fsS -H "Host: $CRON_HOST" "http://127.0.0.1/cron/$CRON_KEY" \
        >/dev/null || echo "cron failed" >&2
done &
```

It works today. The next run starts `interval` after the previous one
ends, so there is no overlap by construction. It is invisible to the
FreeUnit configuration and `/status`. It is the demo fallback if the C work
slips.

## Pros and cons of the options

**1. devnull protocol and internal joint.** Good: it reuses reference
counts, accounting, the timeouts and the access log, touches no hot-path
code, and fills a slot that is already reserved. Bad: the most design work;
it touches the shared `nxt_router.c`; it needs the app-timeout factoring.

**2. `proto.any = NULL` with NULL checks.** Good: it looks small. Bad: it
does not work (§6.1). Fixing it means adding NULL guards to about 12
`r->conf` sites and 6 protocol paths on the hot path, plus a separate
completion and pool-release path. Every future `r->conf` user becomes a
possible crash. Rejected.

**3. Loopback client (plan B1).** Good: the request is real, so none of §6
applies. Bad: a listener is required; it uses TCP; the configuration shape
changes; it needs peer code without a server request.

**4. External timer (plan B2).** Good: zero C, works today. Bad: outside
FreeUnit's configuration and status; one more process; it does not help
non-Docker deployments.

## Open questions

- Should a schedule be able to target `routes/...` in v2, now that devnull
  would support `return` and `share`? Only `proxy` and upstreams are truly
  blocked.
- Where exactly does `nxt_router_request_expire()` land relative to the
  code stream's `prepare_msg` change? Agree the order at the Day-3 sync.
- Per-schedule `/status` counters (N4): the shape belongs to the
  observability stream.


## Amendments

- **`overlap: "queue"` removed** (after 1.36.2). It kept a `pending` flag
  per schedule and a second start path, for a mode no known user needs: a
  cron endpoint catches up on its own at the next run. `"skip"` is the
  only value, and the only behaviour; the key stays so that configurations
  that spell it out keep validating.
