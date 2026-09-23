# USDT probe plan (N3)

Status: analysis / plan only. No probes are wired up yet; this lists where they
would go and how a `--usdt` configure option would detect the prerequisite.

## `<sys/sdt.h>` availability

Checked in this environment:

```
$ echo '#include <sys/sdt.h>' | cc -E -
fatal error: sys/sdt.h: No such file or directory
```

`sys/sdt.h` ships in `systemtap-sdt-devel` (Fedora/RHEL) or
`systemtap-sdt-dev` (Debian/Ubuntu). It is not installed in this worktree's
image, so the header is unavailable here, and detection has to be a
configure-time feature test rather than an assumed default.

### Proposed `auto/usdt`

Follow the existing pattern in `auto/otel` (`src/nxt_otel.c:1`-style guarded
feature) and `auto/feature`'s test-compile helper. Sketch:

```sh
# auto/usdt
if [ $NXT_USDT = YES ]; then
    $echo -n "checking for USDT (sys/sdt.h) ... "

    nxt_feature="USDT probes (sys/sdt.h)"
    nxt_feature_name=NXT_HAVE_USDT
    nxt_feature_run=no
    nxt_feature_incs=
    nxt_feature_libs=
    nxt_feature_test="#include <sys/sdt.h>

                      int main(void) {
                          DTRACE_PROBE(freeunit, configure_test);
                          return 0;
                      }"
    . auto/feature

    if [ ! $nxt_found = yes ]; then
        $echo
        $echo $0: error: --usdt requires sys/sdt.h \
                          \(systemtap-sdt-devel / systemtap-sdt-dev\).
        $echo
        exit 1;
    fi
fi
```

`auto/options` gains a `--usdt` flag (default NO, mirroring `--otel`'s
opt-in shape), and `auto/configure` sources `auto/usdt` next to `auto/otel`.
Call sites wrap each probe in `#if (NXT_HAVE_USDT)` the same way
`src/nxt_otel.h:12` wraps `NXT_OTEL_TRACE()`, so a probe compiles to nothing
(a single `nop`, per the SDT contract) when the option is off or the header
is missing.

## Candidate probe points

Static, zero-overhead-when-disabled `DTRACE_PROBE*` sites. Each is a real
call site that already exists; the "line" is the location the probe would be
inserted at (immediately before/after the marked statement).

| # | Probe | File:line | Notes |
|---|-------|-----------|-------|
| 1 | `port__send` | `src/nxt_port_socket.c:264` (`nxt_port_socket_write2`, entry) | Args: message type, size. (Not the firing process's pid -- every USDT consumer already exposes that as a builtin.) Covers both the direct write and the enqueue-for-later path inside the same call. |
| 2 | `port__recv` | `src/nxt_port_socket.c:1428` (`nxt_port_read_handler`, entry) | Fires once per readv() cycle; pairs with #1 for router↔worker latency histograms. |
| 3 | `mmap__chunk__alloc` | `src/nxt_port_memory.c:308` (`nxt_mem_mmap()` call in `nxt_port_incoming_port_mmap`) | New shared-memory segment mapped for a port; rare (per-port, not per-request) but marks SHM growth. |
| 4 | `mmap__chunk__get` | `src/nxt_router.c:7646` (`nxt_port_mmap_get_buf()` call in `nxt_router_prepare_msg`) | The actual per-request chunk handout; this is the hot one for chunk-reuse pressure, not #3. |
| 5 | `queue__enqueue` | `src/nxt_app_queue.h:76` (`nxt_app_nncq_enqueue(&q->queue, i)` in `nxt_app_queue_send`) | App-queue (SHM, router→worker) enqueue; args: cookie/index, tracking id. |
| 6 | `queue__dequeue` | `src/nxt_app_queue.h:115`-area (`nxt_app_nncq_dequeue` in `nxt_app_queue_recv`) | Matching dequeue on the worker side; the enqueue/dequeue pair gives queue residency time. |
| 7 | `process__start` | `src/nxt_process.c:631` (`pid = fork()` in `nxt_process_create`) | App/router process spawn. `nxt_process_start()` (`src/nxt_process.c:548`) is the sibling fork used for the initial set of processes at boot — worth a second probe if boot-time spawn matters separately from steady-state respawn. |
| 8 | `request__start` / `request__end` | `src/nxt_http_request.c:249` (`nxt_http_request_create`, entry) and `src/nxt_http_request.c:1058` (`nxt_http_request_done`, entry) | Bookends the whole request; pairs directly with the OTel span lifecycle in `src/nxt_otel.c` (`NXT_OTEL_INIT_STATE` / `NXT_OTEL_COLLECT_STATE`) so the two telemetry sources can be cross-checked. |

That is 8 distinct events (10 probe sites counting the start/end pair and the
optional second process-spawn site separately).

## bpftrace sketch (once probes exist)

```
# ipc-latency.bt -- port send -> recv round trip, queue residency
usdt:/usr/sbin/unitd:freeunit:port__send
{
    @start[pid] = nsecs;
}

usdt:/usr/sbin/unitd:freeunit:port__recv
/@start[pid]/
{
    @port_rtt_ns = hist(nsecs - @start[pid]);
    delete(@start[pid]);
}

usdt:/usr/sbin/unitd:freeunit:queue__enqueue
{
    @qstart[args->cookie] = nsecs;
}

usdt:/usr/sbin/unitd:freeunit:queue__dequeue
/@qstart[args->cookie]/
{
    @queue_residency_ns = hist(nsecs - @qstart[args->cookie]);
    delete(@qstart[args->cookie]);
}
```

## Follow-ups (not done here)

- Actually add `auto/usdt` + `--usdt` option and wire the 8 probes; each is a
  small, isolated diff and should go through the same gates as any other
  `src/*.c|h` change (a test that a probe fires, via `bpftrace -l` or a
  built-in self-test, counts as the required test).
- Confirm probe naming convention against any existing provider name (none
  found in this codebase yet — `freeunit` is a placeholder above).
- Decide whether `queue__enqueue`/`queue__dequeue` should also exist for the
  port-level `nxt_nncq_t` (`src/nxt_nncq.h`) independent of the app-level
  `nxt_app_queue_t` (`src/nxt_app_queue.h`) — they are separate rings.
