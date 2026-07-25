# Pluggable Event Engine Contract

Code-verified specification of Unit's pluggable event-engine interface
(`nxt_event_interface_t`), written as groundwork for an io_uring engine.

Every claim cites `file:line` against the tree as checked out. Items that
could not be resolved from the code are marked **OPEN QUESTION**.

Scope: `src/nxt_event_engine.{h,c}`, `src/nxt_fd_event.h`, the epoll / kqueue /
poll engines, the `nxt_conn_io_t` layer, `src/nxt_port_socket.c`, and
`src/nxt_timer.c`. epoll is treated as the reference implementation; kqueue and
poll are used to separate *contract* from *epoll-specific behaviour*.

---

## 1. The vtable: `nxt_event_interface_t`

Defined at `src/nxt_event_engine.h:23-167`. A single instance is copied
by value into `engine->event` (`nxt_event_engine.c:122`, `:425`), so every
engine op is dispatched through `engine->event.<member>(engine, ...)`, wrapped
by the `nxt_fd_event_*` macros at `src/nxt_event_engine.h:354-407`.

Two epoll instances exist and differ *only* in the create function and the
`io` vtable pointer:

| symbol | create / mode | `io` | file |
|---|---|---|---|
| `nxt_epoll_edge_engine` | `nxt_epoll_edge_create`, `EPOLLET \| EPOLLRDHUP` | `nxt_epoll_edge_conn_io` | `nxt_epoll_engine.c:121-162,213-219` |
| `nxt_epoll_level_engine`| `nxt_epoll_level_create`, mode `0` | `nxt_unix_conn_io` | `nxt_epoll_engine.c:167-208,224-230` |

Both share all other function pointers, including `nxt_epoll_poll`, which
branches on `engine->u.epoll.mode == 0` at runtime to get level semantics
(`nxt_epoll_engine.c:962,982,997`).

### 1.1 Member-by-member semantics

Semantics below are the epoll implementation unless a divergence is called out.
`ev` is `nxt_fd_event_t *` (`src/nxt_fd_event.h:55-115`).

| member | signature intent (`h:` line) | epoll behaviour | contract vs epoll-specific |
|---|---|---|---|
| `create` | build event set; args `mchanges`, `mevents` (`h:34`) | allocate `changes[mchanges]`, `events[mevents]`, `epoll_create(1)`; optionally add signalfd + probe accept4 (`nxt_epoll_engine.c:233-283`) | Contract: allocate batching arrays sized `4*events` / `events` (caller `nxt_event_engine.c:116-118`). On any failure calls own `free` and returns `NXT_ERROR`. |
| `free` | close & free set (`h:38`) | close signalfd/eventfd/epoll fds, free arrays, memzero union (`:320-357`) | Contract. |
| `enable` | add fd, arm best read+write (`h:45`) | `read=write=ACTIVE`; `EPOLL_CTL_ADD` `EPOLLIN\|EPOLLOUT\|mode` (`:360-368`) | Contract: after `enable`, both directions armed. kqueue = enable_read+enable_write (`nxt_kqueue_engine.c` enable). |
| `disable` | stop notifications (`h:49`) | if either dir `> DISABLED`: set both `INACTIVE`, `EPOLL_CTL_DEL` (`:371-381`) | Contract. poll maps `disable` to the same slot as `delete` (`nxt_poll_engine.c:72-73`). |
| `delete` | remove fd from set (used to move fd between sets) (`h:56`) | if either dir `!= INACTIVE`: set both `INACTIVE`, `EPOLL_CTL_DEL` (`:384-394`) | Contract. |
| `close` | remove fd *before* `close(2)`; returns `nxt_bool_t` (`h:67`) | `nxt_epoll_delete` then `return ev->changing` (`:406-412`) | **Return value is load-bearing** — see §5. |
| `enable_read` | add fd, arm read (`h:74`) | complex ADD/MOD; preserves write arming; `read=ACTIVE` (`:415-437`) | Contract: idempotent re-arm of read direction. |
| `enable_write` | add fd, arm write (`h:81`) | symmetric to enable_read (`:440-462`) | Contract. |
| `disable_read` | stop read notifications (`h:85`) | `read=INACTIVE`; DEL if write also down, else MOD to `EPOLLOUT` (`:465-484`) | Contract. |
| `disable_write` | stop write notifications (`h:89`) | symmetric (`:487-506`) | Contract. |
| `block_read` | keep armed in kernel, stop *delivery* to app (`h:93`) | if read `!= INACTIVE`: `read=BLOCKED`. **No syscall** (`:509-515`) | epoll/kqueue: purely a state flag (edge/oneshot won't re-fire anyway). poll: actually removes POLLIN from the set (`nxt_poll_engine.c:247-253`) because poll is level-triggered and would busy-loop otherwise. **This is the key readiness-model split — see §16.3.** Guarded by `nxt_fd_event_is_active` in the macro (`h:382-387`). |
| `block_write` | as block_read for write (`h:97`) | `write=BLOCKED`, no syscall (`:518-524`) | Same split as block_read. |
| `oneshot_read` | arm a single read delivery (`h:104`) | `read=ONESHOT,write=INACTIVE`; ADD/MOD `EPOLLIN\|EPOLLONESHOT` (`:540-552`) | epoll: real `EPOLLONESHOT`. poll: emulated — arms `POLLIN`, then `poll()` deletes on fire (`nxt_poll_engine.c:266-277,608-611`). kqueue: `EV_ONESHOT`/`EV_DISPATCH` (`nxt_kqueue_engine.c:36-38,384-389`). |
| `oneshot_write` | single write delivery (`h:111`) | symmetric (`:555-567`) | as oneshot_read. |
| `enable_accept` | add listen fd, **level-triggered** read (`h:118`) | `read=ACTIVE`; `EPOLL_CTL_ADD EPOLLIN` (no EPOLLET) `+ EPOLLEXCLUSIVE` if defined (`:570-584`) | **Must be level-triggered even on the edge engine — see §6.** |
| `enable_file` | file-change notifications (`h:125`) | `NULL` for both epoll engines (`:138,184`) | epoll edge sets `NXT_FILE_EVENTS` only if inotify (`:151-155`) but the vtable slot is still NULL — file events routed elsewhere. kqueue provides `nxt_kqueue_enable_file`. **OPEN QUESTION:** how inotify file events reach the epoll engine given the NULL slot (likely a separate inotify fd registered as a normal read event; not exercised by the io_uring bring-up). |
| `close_file` | remove file before close (`h:131`) | `NULL` (`:139,185`) | kqueue only. |
| `enable_post` | enable cross-thread wakeup; set post handler for signo 0 (`h:138`) | `nxt_epoll_enable_post`: eventfd + `EPOLLIN\|EPOLLET` (`:759-811`) | If `NULL`, engine falls back to a self-pipe — see §7. |
| `signal` | wake the set; signo!=0 routes a unix signal, signo==0 runs post_handler (`h:152`) | `nxt_epoll_signal`: `write(eventfd,1)`, signo ignored because signalfd handles real signals (`:852-871`) | Called cross-thread and from signal context — see §7. |
| `poll` | wait for events, dispatch handlers (`h:156`) | `nxt_epoll_poll` (`:876-1008`) | See §8. |
| `io` | `nxt_conn_io_t *` for this facility (`h:160`) | edge→`nxt_epoll_edge_conn_io`, level→`nxt_unix_conn_io` (`:149,195`) | See §12. |
| `file_support` | 1 if facility does file events (`h:163`) | inotify-dependent (`:151-155`) | Consumed at `nxt_event_engine.c` signal/file setup. |
| `signal_support`| 1 if facility does signal events (`h:166`) | signalfd-dependent (`:157-161`) | Drives self-pipe vs native signal thread (`nxt_event_engine.c:104-108,391-403,431-442`). |

---

## 2. `nxt_fd_event_t` state machine

Enum `nxt_fd_event_state_t` (`src/nxt_fd_event.h:11-44`). `read` and `write`
are independent fields of this type (`:65-66,82-83`), each a small bitfield.

| state | value | meaning | who sets it |
|---|---|---|---|
| `NXT_EVENT_INACTIVE` | 0 | fd absent from kernel set for this direction | disable/delete/close ops; poll after oneshot fire |
| `NXT_EVENT_DISABLED` | 1 | present in kernel but silenced after a oneshot (epoll bookkeeping) | `nxt_epoll_poll` after `EPOLLONESHOT` fires (`:954,974`) |
| `NXT_EVENT_BLOCKED` | 2 | armed in kernel, delivery suppressed by app | `block_read`/`block_write` (`:513,522`) |
| `NXT_EVENT_ONESHOT` | 3 | single-shot arm outstanding | `oneshot_read`/`oneshot_write` (`:548,564`) |
| `NXT_EVENT_LEVEL` | 4 | active level-triggered (eventport) | eventport engine only |
| `NXT_EVENT_DEFAULT` = `NXT_EVENT_ACTIVE` | 5 | active, facility's default trigger mode | enable/enable_read/enable_write/enable_accept (`:363-364,436,461,575`) |

Two predicates gate the macros and poll logic
(`src/nxt_fd_event.h:47-52`):

```
nxt_fd_event_is_disabled(state)  ((state) < NXT_EVENT_ONESHOT)   /* INACTIVE,DISABLED,BLOCKED */
nxt_fd_event_is_active(state)    ((state) >= NXT_EVENT_ONESHOT)  /* ONESHOT,LEVEL,DEFAULT */
```

Entry/exit expectations that an alternative engine must honour:

- `enable_read` is called by app code *only after* checking
  `nxt_fd_event_is_disabled(c->socket.read)` (e.g. `nxt_conn_read.c:121-123`,
  `nxt_conn_write.c:115-117`). So `enable_read` on an already-ACTIVE fd must be
  a safe no-op-ish re-arm — epoll guards this with the `read != BLOCKED` check
  and ADD-vs-MOD decision (`:421-433`).
- `block_read` is only invoked when `nxt_fd_event_is_active((ev)->read)`
  (macro `h:382-387`), i.e. never on INACTIVE. Its post-condition is: no
  further read handler will be dispatched for this fd until `enable_read`.
- After a oneshot fires, epoll leaves the fd in `DISABLED` (not INACTIVE), so a
  subsequent `oneshot_read`/`enable_read` chooses `EPOLL_CTL_MOD` not `ADD`
  (`:545-546,426-427`). An io_uring bridge that emulates oneshot must preserve
  the ADD-vs-MOD distinction or the "already registered" state equivalently.
- `read_ready` / `write_ready` (`src/nxt_fd_event.h:67-68`) are the *readiness
  latches* the I/O layer consumes; the engine sets them on event delivery
  (`:949,969,992-993`) and the I/O functions clear them on `EAGAIN`/short read
  (`nxt_conn_read.c:161,169,179,213,235`). **These latches are the crux of the
  readiness model — see §16.**

---

## 3. Changes-batching model (epoll)

State lives in `nxt_epoll_engine_t` (`src/nxt_event_engine.h:198-219`):
`changes[]`, `nchanges`, `mchanges`, and `error`.

- Every arming op funnels through `nxt_epoll_change(engine, ev, op, events)`
  (`:592-611`). It appends `{op, epoll_event}` to `changes[]`, sets
  `ev->changing = 1`, and stores `ev` in `event.data.ptr`.
- **Flush triggers:** (a) the array is full — `nchanges >= mchanges` forces
  `nxt_epoll_commit_changes` *before* appending (`:601-603`); (b) the top of
  `nxt_epoll_poll` flushes any pending changes before `epoll_wait`
  (`:888-890`).
- `nxt_epoll_commit_changes` (`:614-652`) walks the batch, clears
  `ev->changing = 0` per entry, calls `epoll_ctl` for each, and resets
  `nchanges = 0`.
- **Error mid-batch:** on `epoll_ctl` failure it does *not* abort the batch. It
  enqueues `nxt_epoll_error_handler` for that `ev` on `fast_work_queue`, sets
  `engine->u.epoll.error = 1`, and continues committing the rest (`:637-649`).
  `nxt_epoll_error_handler` (`:655-666`) forces `read=write=INACTIVE` then calls
  `ev->error_handler`.
- The `error` flag is consumed at the top of the next `poll`: if set, it is
  cleared and `timeout` is forced to `0` so `epoll_wait` returns immediately and
  the just-enqueued error handlers run without delay (`:892-896`).
- There is **no ENOMEM-specific path**; allocation of `changes[]` happens once
  in `create` (`:245-248`). Runtime failures are only `epoll_ctl` errors,
  handled as above.

poll's batching mirror: `nxt_poll_change` / `nxt_poll_commit_changes`
(`nxt_poll_engine.c:300-382`); on commit failure it enqueues the error handler
and returns `NXT_ERROR`, and `nxt_poll` then forces `timeout = 0`
(`:511-516`). Contract: **committing changes is a batched, deferred operation;
a change references `ev` by pointer, so `ev` must stay alive until committed**
(this is what `close()`/`ev->changing` protects, §5).

---

## 4. Per-op state transitions (epoll reference)

```
enable          : read=ACTIVE,  write=ACTIVE                 CTL_ADD IN|OUT|mode
enable_read     : (read!=BLOCKED) CTL_ADD|MOD IN[|OUT]       read=ACTIVE
enable_write    : (write!=BLOCKED) CTL_ADD|MOD OUT[|IN]      write=ACTIVE
disable_read    : read=INACTIVE; DEL if write<=DISABLED else MOD OUT
disable_write   : write=INACTIVE; DEL if read<=DISABLED  else MOD IN
disable         : both>DISABLED -> both INACTIVE, CTL_DEL
delete          : either!=INACTIVE -> both INACTIVE, CTL_DEL
block_read      : read!=INACTIVE -> read=BLOCKED   (no syscall)
block_write     : write!=INACTIVE-> write=BLOCKED  (no syscall)
oneshot_read    : read=ONESHOT, write=INACTIVE, ADD|MOD IN|ONESHOT
oneshot_write   : write=ONESHOT,read=INACTIVE,  ADD|MOD OUT|ONESHOT
enable_accept   : read=ACTIVE,  ADD IN[|EXCLUSIVE]           (LEVEL, no ET)
```

---

## 5. `close()` return-value semantics (`nxt_bool_t`)

`nxt_epoll_close` returns `ev->changing` (`:406-412`); `nxt_poll_close` returns
`ev->changing` (`nxt_poll_engine.c:158-164`). The value is **true iff this fd
still has an uncommitted entry sitting in the changes batch**.

Sole meaningful consumer: `nxt_conn_close_handler`
(`src/nxt_conn_close.c:116-135`):

```
events_pending = nxt_fd_event_close(engine, &c->socket);
if (events_pending == 0) {
    nxt_socket_close(...); fd = -1; nxt_conn_untrack(...);
    if (timers_pending == 0) { run ready_handler; return; }
}
/* else defer close via zero-timer so the batch/timers drain first */
nxt_timer_add(engine, &c->write_timer, 0);
```

Contract: **`close()` returning true means "do not `close(2)` the fd yet — an
in-flight change still references this event object"**. The caller reschedules
itself on a zero-timer; the next `poll` commits the batch (`:888-890`), after
which the fd may be closed safely. `true` is only ever produced when the DEL
issued inside `close()` itself got batched rather than executed. **For a
completion-based engine this generalises to "there are still SQEs / CQEs in
flight that reference this fd or its user_data"** (§16.9).

---

## 6. `enable_accept`: the level-trigger requirement

`nxt_epoll_enable_accept` deliberately arms plain `EPOLLIN` **without**
`engine->u.epoll.mode` (i.e. no `EPOLLET`), plus `EPOLLEXCLUSIVE` when available
(`nxt_epoll_engine.c:570-584`). Even on the edge engine the listen socket is
level-triggered. kqueue's `enable_accept` uses `EV_ADD | EV_ENABLE` (level,
no `EV_CLEAR`) and installs the kqueue-specific listen handler
(`nxt_kqueue_engine.c` enable_accept).

Why the accept path depends on level-trigger
(`src/nxt_conn_accept.c`):

1. On a readable listen socket `nxt_conn_listen_handler` sets
   `lev->ready = lev->batch` and calls `lev->accept` (`:123-132`).
2. Each `accept`/`accept4` decrements `lev->ready` and sets
   `lev->socket.read_ready = (lev->ready != 0)` (`:147-148`,
   `nxt_epoll_engine.c:1025-1026`).
3. `nxt_conn_accept` re-arms the *next* accept **only while there is a spare
   conn slot and `lev->socket.read_ready`** (`:237-242`). When the batch is
   exhausted (`ready` hit 0 → `read_ready = 0`) it stops looping.
4. Because the fd is level-triggered, if the listen backlog still has pending
   connections, epoll re-delivers `EPOLLIN` on the next `poll` and the cycle
   restarts. **An edge-triggered or oneshot listener would silently stall
   whenever the accept batch is smaller than the backlog.** This is the single
   hardest constraint for a completion/edge bridge to reproduce.

EAGAIN / resource handling (`nxt_conn_accept_error`, `:331-368`):

- `NXT_EAGAIN` → benign, just return (backlog drained; `read_ready` was set 0 at
  `:339`).
- `ECONNABORTED` → log and return.
- `EMFILE`/`ENFILE`/`ENOBUFS`/`ENOMEM` → `nxt_conn_accept_close_idle`
  (`:353-360`): schedule an idle-connection reaper on `close_work_queue`,
  `nxt_timer_add(lev->timer, 100)`, and **`nxt_fd_event_disable_read` on the
  listen socket** (`:265-280`). After 100 ms `nxt_conn_listen_timer_handler`
  re-`enable_accept`s and retries (`:371-390`). So the accept path oscillates
  between level-armed and disarmed under fd pressure — an engine must let
  `disable_read` + later `enable_accept` re-establish level delivery cleanly.

---

## 7. `enable_post` / `signal`: cross-thread wake-up contract

Two mechanisms, selected by whether the vtable provides `enable_post`
(`nxt_event_engine_post_init`, `src/nxt_event_engine.c:168-180`):

**Native (epoll eventfd).** `nxt_epoll_enable_post` stores `post_handler` and
registers an `eventfd(0,0)` with `EPOLLIN | EPOLLET`
(`nxt_epoll_engine.c:759-811`). `nxt_epoll_signal` does
`write(eventfd, 1)` and ignores `signo` (`:852-871`). `nxt_epoll_eventfd_handler`
(`:814-849`) runs `post_handler` on `fast_work_queue`; it only actually
`read()`s the eventfd once per ~2^32 notifications, relying on ET + eventfd
saturation semantics.

**Fallback (self-pipe).** If `enable_post == NULL`,
`nxt_event_engine_signal_pipe_create` makes a pipe (blocking writer,
non-blocking reader) and enables read on `pipe->event`
(`src/nxt_event_engine.c:183-216`). `nxt_event_engine_signal` then writes one
byte `= signo` to `fds[1]` (`:267-286`). `nxt_event_engine_signal_pipe`
drains bytes; byte 0 means "run post_handler once", non-zero routes a unix
signal (`:289-327`).

**Thread-safety expectations.**
`nxt_event_engine_post` adds work to `engine->locked_work_queue` (a mutex-guarded
queue) then calls `nxt_event_engine_signal(engine, 0)` (`:250-264`). The
`post_handler` (`nxt_event_engine_post_handler`, `:330-341`) moves the locked
queue into the engine's own `fast_work_queue`. So: **arbitrary threads may call
`post`/`signal`; the wake primitive (eventfd write or pipe write) must be
async-signal-safe and thread-safe, but the actual work payload travels through
the locked work queue, never through the wake primitive itself** (comment at
`:274-277`). An io_uring engine needs an equivalent doorbell (e.g. an eventfd
registered via `IORING_REGISTER_EVENTFD`, or an `IORING_OP_MSG_RING`) that
merely unblocks the wait; the payload stays in `locked_work_queue`.

---

## 8. `poll()` obligations and the main loop

Driver: `nxt_event_engine_start` (`src/nxt_event_engine.c:521-568`):

```
for (;;) {
    for (;;) { handler = queue_pop(); if (!handler) break; handler(...); }  // drain work
    timeout = nxt_timer_find(engine);          // §15
    engine->event.poll(engine, timeout);       // block for events
    now = monotonic_ms();
    nxt_timer_expire(engine, now);             // fire due timers
}
```

The inner loop fully drains all work queues (via `nxt_event_engine_queue_pop`,
`:478-518`, which round-robins `fast → accept → read → socket → connect → write
→ shutdown → close`) *before* every `poll`. So a `poll` implementation must
assume all previously-enqueued handlers have already run.

`poll()` obligations, from `nxt_epoll_poll` (`:876-1008`):

1. Commit any pending changes first (`:888-890`).
2. If `error` flag set, clear it and force `timeout = 0` (`:892-896`).
3. `epoll_wait(fd, events, mevents, timeout)`; update thread time immediately
   after (`:901-906`) so timers see a fresh clock.
4. On `-1`: `EINTR` → info log + return; else alert + return (`:910-917`). It
   must **not** throw away the pending timer expiry that runs after `poll`.
5. Per returned event, set the readiness latch and enqueue the direction handler
   onto **the fd's own per-direction work queue**:
   - `EPOLLIN` → `ev->read_ready = 1`; if `read != BLOCKED`, enqueue
     `ev->read_handler` on `ev->read_work_queue` (`:948-960`). ONESHOT→DISABLED
     transition here (`:953-955`). In level mode, a BLOCKED fd that still fires
     is disarmed via `nxt_epoll_disable_read` to avoid a busy-loop (`:962-965`).
   - `EPOLLOUT` → symmetric with `ev->write_*` (`:968-986`).
   - `EPOLLERR`/`EPOLLHUP` without IN/OUT → enqueue `nxt_epoll_error_handler`
     on `fast_work_queue`, unless both directions are BLOCKED (`:928-1006`).
6. `poll()` never runs handlers inline — it only *enqueues* them; the outer loop
   runs them next iteration. Contract: **event dispatch is deferred through work
   queues, and each direction lands on its designated queue** (`ev->read_work_queue`,
   `ev->write_work_queue`), which for connections are wired to
   `read_work_queue` / `write_work_queue` in the `nxt_conn_read`/`nxt_conn_write`
   macros (`src/nxt_conn.h:299-318`).

`timeout` handling: `NXT_INFINITE_MSEC` (from `nxt_timer_find`,
`src/nxt_timer.c:276`) means "block indefinitely"; `0` means "return
immediately". See §15.

---

## 9. Oneshot semantics — precisely

epoll uses **genuine `EPOLLONESHOT`** (`nxt_epoll_engine.c:527-538` comment,
`:551,566`). After the kernel delivers the event it internally disables the fd;
`nxt_epoll_poll` records this by moving the direction `ONESHOT → DISABLED`
(`:953-955,973-975`), which keeps the fd *registered* so the next re-arm uses
`EPOLL_CTL_MOD`. The eventfd post channel is the other `EPOLLET` user but is not
"oneshot".

poll **emulates** oneshot: it arms a normal `POLLIN`/`POLLOUT` and, when the
event fires, transitions `ONESHOT → INACTIVE` and issues `NXT_POLL_DELETE`
inline (`nxt_poll_engine.c:266-292,608-611,620-623`).

kqueue uses `EV_ONESHOT` (auto-delete) or `EV_DISPATCH` (auto-disable) selected
by `NXT_KEVENT_ONESHOT` (`nxt_kqueue_engine.c:32-38,384-399,758-789`).

Contract: **after a oneshot fires, no further delivery occurs until an explicit
re-arm**, and the post-fire resting state is engine-private (DISABLED for epoll,
INACTIVE for poll). Consumers only rely on "fires exactly once". An io_uring
bridge can implement oneshot as a single (non-multishot) `IORING_OP_POLL_ADD`.

Primary consumers of oneshot in app code are timers/signal plumbing and any
`nxt_fd_event_oneshot_*` callers — grep shows these macros exist
(`src/nxt_event_engine.h:398-403`); the conn read/write fast paths instead use
`enable_read`/`block_read` (level/edge), not oneshot.

---

## 10. `EPOLLET` usage and the edge conn-io overrides

Only the edge engine sets `EPOLLET` (`nxt_epoll_engine.c:218`,
`mode = EPOLLET|EPOLLRDHUP`), applied to every non-listen `enable*` via
`engine->u.epoll.mode` (`:367,424,449` etc.). The eventfd post channel also uses
`EPOLLET` independently (`:796`).

Edge mode changes two I/O strategies, which is why the edge engine ships its own
`nxt_epoll_edge_conn_io` (`:99-118`):

- **connect** — `nxt_epoll_edge_conn_io_connect` (`:1063-1132`): instead of the
  generic connect-then-getsockopt-test dance, on `NXT_AGAIN` it arms *both*
  directions with a single `nxt_epoll_enable` and sets `read = BLOCKED`
  (`:1082-1091`), then `nxt_epoll_edge_conn_connected` uses `epoll_error` to
  decide success without a `getsockopt(SO_ERROR)` syscall (`:1135-1157`). The
  generic level path (`nxt_conn_io_connect`, `nxt_conn_connect.c:34-74`) instead
  enables write only and tests via `nxt_conn_connect_test`.
- **recvbuf** — `nxt_epoll_edge_conn_io_recvbuf` (`:1166-1178`) wraps
  `nxt_conn_io_recvbuf` and, if a positive read coincides with a pending
  `EPOLLRDHUP` (`c->socket.epoll_eof`), forces `read_ready = 1` so the caller
  loops again to observe EOF. **Under edge-trigger the kernel will not re-deliver
  readability, so the app must keep reading until EAGAIN; this shim guarantees
  the trailing EOF is seen.** `epoll_eof` is set from `EPOLLRDHUP` at
  `:942-946`.

Contract distinction: level engines rely on the kernel re-asserting readiness;
the edge engine and its conn-io shims take responsibility for draining to EAGAIN
themselves. An io_uring readiness bridge that emulates *level* semantics
(re-arming poll after each partial drain) can reuse `nxt_unix_conn_io`; one that
emulates *edge* must supply the equivalent of the recvbuf/connect shims.

---

## 11. Engine lifecycle and selection

### 11.1 Registration and lookup

`nxt_services[]` (`src/nxt_service.c:10-57`) registers, in order:
kqueue (if any), then **`epoll` / `epoll_edge` / `epoll_level`** (edge build) or
`epoll`/`epoll_level` (level-only build), then eventport/devpoll/pollset (per
platform), then always `poll` and `select`. On Linux the **first entry is
`{"engine","epoll", &nxt_epoll_edge_engine}`** (`:17`).

`nxt_service_get(services, "engine", name)` (`:130-165`): with `name == NULL` it
returns the **first** `"engine"` entry — i.e. the platform default (epoll edge on
Linux). With a name it string-matches. On miss it logs and returns `NULL`
(`:161-164`).

### 11.2 The five `engine` lookup sites

| site | `name` arg | purpose |
|---|---|---|
| `nxt_runtime.c:285` | `NULL` | initial main-process engine (default = epoll edge) |
| `nxt_runtime.c:397` | `rt->engine` | re-select after daemon `fork()` (`nxt_runtime_initial_start`) |
| `nxt_runtime.c:704` | `rt->engine` | `nxt_runtime_event_engine_change` when name/batch changed |
| `nxt_runtime.c:874` | `rt->engine` | conf parse; also normalises `rt->engine = interface->name` (`:879`) |
| `nxt_process.c:665` | `rt->engine` | every child process re-creates its engine post-`fork` |

Additionally the **router** looks up the default engine with `name == NULL`
(`src/nxt_router.c:1262`) and creates **one engine per worker thread**
(`nxt_router_engines_create` → `nxt_event_engine_create`, `:3275-3341`,
worker loop `nxt_event_engine_start` at `:3727`). **Note:** router worker
engines therefore always use the *default* (first-registered) engine, not the
configured `rt->engine`.

### 11.3 create / change / free

- `nxt_event_engine_create` (`src/nxt_event_engine.c:41-165`): zalloc engine,
  set up the eight named work queues (`:74-94`), optional signals (`:96-109`),
  then `interface->create(engine, 4*events, events)` where
  `events = batch ? batch : 32` (`:116-118`), copy vtable (`:122`),
  `post_init` (`:124`), `timers_init` (`:128`). On any failure it unwinds via
  `interface->free` + frees and returns `NULL` (`:146-164`).
- `nxt_event_engine_change` (`:383-445`): used to swap facilities (e.g. after
  fork, or on reconfigure). Stops/starts the signal thread as needed, closes the
  self-pipe if the new facility has native post, `free`s the old set, `create`s
  the new one, re-`post_init`s.

### 11.4 What happens if `create()` fails — **there is no retry-next-engine loop**

Confirmed: `nxt_runtime_event_engines` calls `nxt_service_get(..., NULL)` once,
then `nxt_event_engine_create` once; if that returns `NULL` it immediately
returns `NXT_ERROR` (`src/nxt_runtime.c:285-297`). There is **no** fallback to
the next `nxt_services[]` entry. `nxt_event_engine_change` likewise returns
`NXT_ERROR` on `create` failure with no fallback (`:421-423`). Every caller
treats engine-create failure as fatal:

- main process init → `NXT_ERROR` propagates, startup aborts.
- `nxt_runtime_start` → `nxt_runtime_quit(task, 1)` on failure (`:355-370`).
- child `nxt_process_setup` → returns `NXT_ERROR` (`nxt_process.c:665-672`).
- router → `nxt_router_engines_create` returns `NXT_ERROR`
  (`nxt_router.c:3331-3333`).

So a new io_uring engine that fails `create()` (e.g. old kernel) will **not**
transparently degrade to epoll; selection must be gated *before* create (e.g.
kernel probe at registration, or a config-level opt-in), otherwise the process
dies. This is the single most important operational caveat for adding an engine.

### 11.5 Per-process / per-thread engine map

| process | engine(s) | threads |
|---|---|---|
| main | one, default facility, with signals | single event thread |
| controller | one, `rt->engine`, re-created in child setup | single |
| router (parent) | one "main" engine | router main thread |
| router workers | one engine **each**, default facility, no signals (`nxt_router.c:3330`) | N worker threads (`rtcf->threads`, default `nxt_ncpu`, `:1719-1720`) |
| application processes | one, `rt->engine` | single |

Auxiliary `nxt_thread_pool` threads exist (`nxt_runtime.c:329`,
`nxt_process.c:674`) but are blocking worker pools, not event engines.

---

## 12. The `nxt_conn_io_t` layer

Definition `src/nxt_conn.h:39-87`. Default instance `nxt_unix_conn_io`
(`src/nxt_conn.c:10-39`); edge/kqueue variants override `connect`/`recvbuf`
(and, for kqueue, `read`). A conn caches `c->io = engine->event.io` at creation
(`src/nxt_conn.c:76`); the engine's `io` pointer therefore *is* the per-facility
I/O policy.

### 12.1 Accept path (`src/nxt_conn_accept.c`)

`nxt_listen_event` wires the listen fd: `read_handler =
nxt_conn_listen_handler`, `read_work_queue = accept_work_queue`,
`lev->accept = engine->event.io->accept`, then `nxt_fd_event_enable_accept`
(`:37-82`). Accept flow: handler → `lev->accept` (`nxt_conn_io_accept` or the
accept4 variant) → `nxt_conn_accept` → re-arm loop (§6). Buffers: the accepted
conn's `remote` sockaddr comes from `nxt_sockaddr_cache_alloc(engine, ...)`
(`:109`), the per-engine mem cache.

### 12.2 Read path (`src/nxt_conn_read.c`, recvbuf)

`nxt_conn_io_read` (`:37-128`): early-out on `c->socket.error || c->block_read`
(`:52-54`). If `read_ready`, call `c->io->recvbuf` (or the state's
`io_read_handler`) (`:66-75`). On `n > 0`: update buf, **`nxt_fd_event_block_read`**
(stop further delivery), disable read timer, enqueue `ready_handler` (`:77-91`).
On `n == 0` (peer close) or `NXT_ERROR`: block_read, disable timer, enqueue
close/error handler (`:93-103`). On `NXT_AGAIN` with `read_ready` still set:
just reset the read timer (SSL renegotiation case, `:105-118`). Otherwise
**re-`enable_read`** if disabled and (re)arm timer (`:121-127`).

`nxt_conn_io_recvbuf` (`:131-194`): coalesce buffers, `readv` (or `recv` for a
single iov). **Clears `read_ready = 0` on short read (`n < rb.size`, `:160-161`),
on EOF (`:169`), and on `EAGAIN` (`:179`).** Loops on `EINTR`. This EAGAIN-clears-
readiness contract is fundamental (§16.1).

### 12.3 Write path (`src/nxt_conn_write.c`)

`nxt_conn_io_write` (`:16-154`): early-out on `error || block_write` (`:30-32`)
or `!write_ready` (`:34-36`). Loops calling `c->io->sendbuf` (writev/sendfile),
updating `write_ready = sb.ready` each pass (`:57-85`). When the buffer chain
drains it `nxt_fd_event_block_write`s (`:73-76`). On `sb.limit == 0` (10 MB cap,
`:53`) it **defers continuation to the next poll via a zero-timer** (`:97-104`)
so other events are serviced — an explicit fairness yield. On `NXT_AGAIN` it
re-`enable_write`s if disabled and arms the write timer (`:105-118`).

There is **no `TCP_CORK`/`TCP_NOPUSH`** in this path; only `TCP_NODELAY`
via the `nxt_conn_tcp_nodelay_on` macro (`src/nxt_conn.h:217-242`). sendfile is
platform-dispatched (`nxt_conn_io_sendbuf` → `nxt_conn_io_sendfile`,
`:174-259`).

### 12.4 Connect path

Level: `nxt_conn_sys_socket` → `nxt_conn_io_connect` (`nxt_conn_connect.c:34-74`):
on `NXT_AGAIN` set `write_handler = nxt_conn_connect_test`, arm write via
`nxt_fd_event_enable_write` (`:53-62`); `nxt_conn_connect_test` blocks write,
tests `SO_ERROR`, dispatches ready/error (`:121-145`). Edge variant: §10.

### 12.5 Buffer lifetime

Engine-scoped allocators live in `src/nxt_event_engine.c:571-755`:
`nxt_event_engine_mem_alloc` uses a per-size free-list cache
(`engine->mem_cache`, keyed by size with a hint byte, `:571-640`);
`nxt_event_engine_buf_mem_alloc` wraps it to make a `nxt_buf_t` with
`completion_handler = nxt_event_engine_buf_mem_completion` (`:692-719`). Freed
buffers return to the cache (cap 16 per size, `:678-684`). **These buffers are
owned by the engine thread and reused synchronously after the I/O call returns —
a completion-based engine that keeps a buffer registered with the kernel across
a `poll` boundary would violate this reuse assumption** (§16.6).

---

## 13. UDS port-socket path (`src/nxt_port_socket.c`)

Ports are `AF_UNIX` socketpairs used for inter-process messaging; they use the
engine's fd-event ops directly, *not* the `nxt_conn_io_t` layer.

- Enable: `port->socket.read_handler = nxt_port_read_handler` (or the queue
  variant), then `nxt_fd_event_enable_read` (`:746-752`). Writes:
  `write_handler = nxt_port_write_handler`, `write_ready = 1` initially
  (`:161-166`).
- **Read handler** `nxt_port_read_handler` (`:766-868`) loops: alloc buf →
  `nxt_socketpair_recv` (recvmsg with SCM_RIGHTS/creds). On `n > 0` process the
  message and, **only if `port->socket.read_ready`, continue the loop**
  (`:848-852`); on `NXT_AGAIN` free buf and `nxt_fd_event_enable_read`
  (`:855-860`). Buffer-alloc failure disarms read via
  `nxt_fd_event_block_read` and routes to the error handler (`:781-799`).
- **Write handler** `nxt_port_write_handler` (`:403-601`) drains the queued
  message list while `port->socket.write_ready` (`:425-573`); on empty queue it
  sets `block_write` (`:432-434`); when the socket buffer fills it eventually
  `enable_write`s if disabled (`:575-577`). Cross-thread arming is deferred:
  `block_write`/`enable_write` are applied via `nxt_port_post`
  (`:389-401,590-596`) because another thread's engine owns the fd.
- `read_ready`/`write_ready` are the same readiness latches
  (`:759,848,913,964`), set by the socketpair recv/send helpers on EAGAIN.

Assumptions that break under a completion engine:

- The port loop is a classic **readiness → drain-to-EAGAIN** loop; it calls
  `recvmsg`/`sendmsg` itself and consults `read_ready`/`write_ready` to decide
  whether to continue (§16.2, §16.4). A pure completion model would move the
  recvmsg into the kernel and deliver bytes, not readiness — the loop would need
  a poll-mode shim.
- Cross-thread `nxt_port_post` + `block_write`/`enable_write` assume arming is a
  cheap state toggle; with io_uring, arming means submitting an SQE on the
  owning thread's ring (`nxt_port_post` already routes to that thread, which
  helps).
- SCM_RIGHTS fd passing and credential cmsgs (`nxt_socket_msg_oob_get`,
  `:824`) must be preserved by whatever recvmsg mechanism the engine uses
  (io_uring `IORING_OP_RECVMSG` supports cmsg, so this is feasible).

---

## 14. Timers and the event loop

`nxt_timers_t` is embedded in the engine (`src/nxt_event_engine.h:443`),
initialised with a batched `changes` array (`nxt_timers_init`,
`nxt_timer.c:31-46`). Timer add/disable batch into `changes[]` and commit at
`nxt_timer_find` (`:240-242`), mirroring the fd-change batching.

Integration with `poll()` timeout (main loop, §8):

1. `nxt_timer_find(engine)` (`:229-277`) commits pending timer changes, walks the
   rbtree for the earliest *enabled* timer, records `timers->minimum`, and
   returns `max(delta, 0)` ms — or `NXT_INFINITE_MSEC` if none (`:274-276`).
2. That value is passed straight to `engine->event.poll(engine, timeout)`
   (`nxt_event_engine.c:560-562`). The engine blocks at most that long.
3. After `poll`, `nxt_timer_expire(engine, now)` (`:280-327`) fires every timer
   with `time <= now + bias`, enqueuing `nxt_timer_handler` on each timer's
   `work_queue`.

Contract for a new engine's `poll`: **honour the millisecond timeout exactly
(0 = immediate, `NXT_INFINITE_MSEC` = block until an event), and refresh the
thread clock immediately after waking** (`nxt_epoll_engine.c:906`) so
`nxt_timer_expire` measures against a current `now`. io_uring's
`io_uring_enter` with a timeout SQE (`IORING_OP_TIMEOUT`) or
`io_uring_wait_cqe_timeout` satisfies this.

---

## 15. Work-queue dispatch summary

Eight per-engine queues, drained round-robin by
`nxt_event_engine_queue_pop` in the order `fast, accept, read, socket, connect,
write, shutdown, close` (`nxt_event_engine.c:478-518`; created/named at
`:78-94`). Plus `locked_work_queue` for cross-thread posts (§7). The engine sets
`ev->read_work_queue`/`ev->write_work_queue` per event; `poll` enqueues onto
those. This indirection means **a new engine does not choose where a handler
runs — it only enqueues onto the queue the event object already names**, so
handler ordering/fairness is preserved regardless of the underlying facility.

---

## 16. Readiness → completion mismatch inventory

Every place the codebase assumes *readiness* semantics (level/edge "the fd is
ready, now you syscall"). A poll-mode io_uring bridge — e.g. multishot
`IORING_OP_POLL_ADD` producing CQEs that set `read_ready`/`write_ready` and then
letting existing handlers do the actual `readv`/`recvmsg`/`writev` — must
preserve each of these. Item count: **12**.

1. **recvbuf EAGAIN loop.** `nxt_conn_io_recvbuf`/`nxt_conn_io_recv` call
   `readv`/`recv` and clear `read_ready = 0` on `EAGAIN`, short read, or EOF
   (`nxt_conn_read.c:160-161,169,179,213,222,235`). The read handler
   (`:66-119`) only calls recvbuf when `read_ready` is set. *Bridge:* the POLL
   CQE must set `read_ready`; the actual byte read stays in userspace, so EAGAIN
   semantics are unchanged. A completion `RECV` that returns bytes (not
   readiness) would bypass this latch entirely and desynchronise the state
   machine.

2. **Port recvmsg drain loop.** `nxt_port_read_handler` loops on `read_ready`
   and re-`enable_read`s on `NXT_AGAIN` (`nxt_port_socket.c:848-860`);
   `nxt_port_queue_read_handler` mirrors it (`:913,964,983`). *Bridge:* keep
   recvmsg in userspace; POLL CQE sets `read_ready` and re-arms.

3. **`block_read`/`block_write` = "stay armed in kernel but don't deliver".**
   epoll/kqueue implement this as a pure state flag with **no syscall**
   (`nxt_epoll_engine.c:509-524`); the fd remains registered and `poll` simply
   refuses to dispatch a BLOCKED direction (`:951,971`). poll, being level, must
   actually remove the fd from the pollset (`nxt_poll_engine.c:247-262`).
   *Bridge:* if using multishot POLL, "blocked" must suppress dispatch of
   received CQEs (drop/ignore) **without** cancelling the SQE, or must cancel and
   re-submit on unblock — the app relies on `block_read` being cheap and
   reversible, called on every successful read (`nxt_conn_read.c:82`).

4. **Port write drain loop.** `nxt_port_write_handler` loops
   `while (port->socket.write_ready)` doing its own `sendmsg`, toggling
   `write_ready` via the send helper, and arming/blocking write cross-thread
   (`nxt_port_socket.c:573-596`). *Bridge:* POLL CQE sets `write_ready`; sendmsg
   stays in userspace.

5. **enable_accept level re-fire.** The accept re-arm loop stops when the batch
   is exhausted and relies on the kernel re-delivering `EPOLLIN` while the
   backlog is non-empty (`nxt_conn_accept.c:237-242`, §6). *Bridge:* a
   **multishot** POLL on the listen fd, or explicit re-arm after each accept
   batch, is mandatory; a oneshot/edge listener stalls under partial-batch
   accept.

6. **Engine buffer reuse across poll.** `nxt_event_engine_buf_mem_*` hands out
   thread-local buffers that are reused immediately after the synchronous I/O
   call (`nxt_event_engine.c:571-755`, §12.5). *Bridge:* do **not** hand these
   buffers to the kernel across a `poll` boundary (completion `READ`/`WRITE`);
   poll-mode keeps the buffer in userspace so reuse is safe.

7. **conn_write fairness zero-timer.** After sending its 10 MB limit,
   `nxt_conn_io_write` yields via `nxt_timer_add(&c->write_timer, 0)` to let the
   next `poll` service other fds (`nxt_conn_write.c:97-104`). *Bridge:* the
   engine must still honour zero-ms timeout as "return immediately after
   draining ready work" (§14) so the yield actually yields.

8. **Edge-mode EOF shim.** `nxt_epoll_edge_conn_io_recvbuf` forces
   `read_ready = 1` when a positive read coincides with pending `EPOLLRDHUP`
   (`nxt_epoll_engine.c:1166-1178`), because edge won't re-deliver. `epoll_eof`
   comes from `EPOLLRDHUP` (`:942-946`). *Bridge:* an edge-emulating engine must
   surface a RDHUP-equivalent (io_uring POLL can report `POLLRDHUP`) or use
   level semantics and reuse `nxt_unix_conn_io` instead.

9. **`close()` returns "in-flight change pending".** `nxt_epoll_close` returns
   `ev->changing`; `nxt_conn_close_handler` defers the actual `close(2)` until it
   is false (`nxt_epoll_engine.c:406-412`, `nxt_conn_close.c:116-135`, §5).
   *Bridge:* generalise to "SQEs/CQEs still reference this fd or user_data";
   return true so the conn layer defers close, then reap outstanding completions
   before the fd is closed. Closing an fd with an in-flight POLL/RECV SQE risks a
   late CQE against a reused user_data — the single most important correctness
   hazard.

10. **`ev->changing` protects a dangling pointer in the batch.** A queued change
    stores `event.data.ptr = ev` (`nxt_epoll_engine.c:610`) and is dereferenced
    at commit (`:628`). The event object must outlive its uncommitted change.
    *Bridge:* if SQEs carry a pointer/user_data to the event, the object's
    lifetime must extend until the matching CQE is consumed — same invariant,
    stronger (kernel-side) than epoll's userspace batch.

11. **Error-only events map to an error handler.** `poll` treats
    `EPOLLERR/EPOLLHUP` (or `POLLERR/POLLHUP`) without IN/OUT as an error,
    enqueuing `error_handler`, and sets both readiness latches so a subsequent
    syscall observes the real errno (`nxt_epoll_engine.c:928-1006`,
    `nxt_poll_engine.c:601-627`). *Bridge:* io_uring POLL CQEs report
    `POLLERR/POLLHUP` in `res`; map them the same way and let the userspace
    syscall surface the errno (rather than trusting a completion result code).

12. **Cross-thread wake is a doorbell, not a data channel.** `signal`/`post`
    only unblock the wait; the payload rides `locked_work_queue`
    (`nxt_event_engine.c:250-286`, §7). *Bridge:* register an eventfd with the
    ring (or use `IORING_OP_MSG_RING`) purely to break `io_uring_enter`; keep
    the work item in `locked_work_queue`. Must remain async-signal-safe.

### 16.1 Net design implication

The lowest-risk io_uring bring-up is a **poll-mode / readiness bridge**:
multishot `IORING_OP_POLL_ADD` per armed direction whose CQE sets
`read_ready`/`write_ready` and enqueues the existing `read_handler`/
`write_handler`, leaving all `readv`/`recvmsg`/`writev`/`sendmsg`/`sendfile`
calls in userspace exactly as today. That satisfies items 1-8, 11 by
construction and reuses `nxt_unix_conn_io`. The genuinely new work is items
9-10 (deferred close / in-flight-SQE lifetime), item 3 (block semantics without
cancelling a multishot SQE), item 5 (multishot on the listener), and item 12
(ring doorbell). A full **completion-mode** engine (kernel does the I/O) would
additionally invalidate items 1-2, 4, 6 and require replacing the conn-io and
port-socket layers wholesale — out of scope for a drop-in engine.
