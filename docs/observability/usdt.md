# USDT tracepoints (N3/N4 observability)

Status: implemented. See `docs/observability/usdt-plan.md` for the original
analysis this follows.

## Building with USDT

```
apt-get install -y systemtap-sdt-dev   # provides <sys/sdt.h>
./configure --usdt ...
make -j2
```

`--usdt` is opt-in (default off), mirroring `--otel`'s shape (`auto/options`,
`auto/help`, `auto/summary`). `auto/usdt` runs a configure-time
`auto/feature` check that compiles a `DTRACE_PROBE()` call against
`<sys/sdt.h>` and defines `NXT_HAVE_USDT` on success; it fails the configure
step with a clear message (naming `systemtap-sdt-devel` /
`systemtap-sdt-dev`) if the header is missing. No extra library or runtime
dependency is added -- `sys/sdt.h` only emits inline asm and an ELF note,
so there is nothing to link.

## The `NXT_USDT()` macro

`src/nxt_usdt.h` defines:

```c
NXT_USDT(name, ...)
```

taking a probe name (using `__` where the provider:probe form uses `-`,
e.g. `port__send` for `freeunit:port-send`) and 0-6 cheap arguments. It
expands to `DTRACE_PROBEn(freeunit, name, ...)` (which `sys/sdt.h` itself
defines as `STAP_PROBEn`, so one macro covers dtrace, bpftrace and
SystemTap) when `NXT_HAVE_USDT` is set, and to nothing when it is not.

Per the SDT contract, a probe compiles to one `nop` instruction (plus a
`.note.stapsdt` ELF note carrying the argument locations) when built with
`--usdt` and no tracer is attached, and to literally nothing -- not even
the `nop` -- in a default build. Each call site is one line, e.g.:

```c
NXT_USDT(port__send, stream, type);
```

so a probe insertion is a trivial one-line diff against unrelated changes
in the same function -- important since other in-flight work touches
`nxt_router.c`, `nxt_conf_validation.c` and the language modules.

A bit-field argument (e.g. a `status:16` struct member) does not work
directly with the underlying `sizeof`/`typeof` machinery in `sys/sdt.h`;
cast it to a plain integer type first, as `request__done` does with
`(nxt_int_t) r->status`.

There is no semaphore guard (the cheap, "no expensive argument" contract
this macro is scoped to does not need one): every argument expression is
evaluated at the call site whenever the build has `NXT_HAVE_USDT`,
whether or not a tracer is attached. Arguments must therefore stay a
plain local or at most one pointer dereference (`port->pid`,
`process->pid`), never a function call. In particular, never pass the
firing process's own pid: `bpftrace`, SystemTap and `dtrace` all expose
it as a builtin already (`pid` in bpftrace/dtrace, the SystemTap
tapset), so passing it here would be both redundant and -- for the two
probes in `src/nxt_app_queue.h`, which also compiles into `libunit` and
so cannot use the router-only `nxt_pid` global -- an extra `getpid()`
syscall on every call under `--usdt` (glibc no longer caches it). An
earlier draft of these probes did exactly that; removed.

## Probe list

| Provider:probe | File:line (call site) | Args |
|---|---|---|
| `freeunit:port-send` | `src/nxt_port_socket.c:301` (`nxt_port_socket_write2`) | `stream, type` |
| `freeunit:port-recv` | `src/nxt_port_socket.c:1443` (`nxt_port_read_handler`, entry) | `port->pid` |
| `freeunit:mmap-chunk-alloc` | `src/nxt_port_memory.c:318` (`nxt_port_incoming_port_mmap`, after `nxt_mem_mmap()`) | `process->pid, PORT_MMAP_SIZE` |
| `freeunit:mmap-chunk-get` | `src/nxt_router.c:7652` (`nxt_router_prepare_msg`, after `nxt_port_mmap_get_buf()`) | `req_size + content_length` |
| `freeunit:queue-enqueue` | `src/nxt_app_queue.h:81` (`nxt_app_queue_send`, after `nxt_app_nncq_enqueue()`) | `slot index, tracking id` |
| `freeunit:queue-dequeue` | `src/nxt_app_queue.h:127` (`nxt_app_queue_recv`, after the slot is claimed) | `slot index` |
| `freeunit:process-spawn` | `src/nxt_process.c:670` (`nxt_process_create`, parent branch, after `fork()`) | `child pid` |
| `freeunit:request-start` | `src/nxt_http_request.c:286` (`nxt_http_request_create`) | `(uintptr_t) r` |
| `freeunit:request-done` | `src/nxt_http_request.c:1069` (`nxt_http_request_done`) | `(uintptr_t) r, (nxt_int_t) r->status` |

None of these pass the firing process's own pid -- every USDT consumer
(bpftrace/dtrace's `pid` builtin, the SystemTap tapset) already exposes
it without an argument, and adding it here would cost a `getpid()`
syscall per call under `--usdt` for the two `nxt_app_queue.h` probes
(see above).

`(uintptr_t) r` (the `nxt_http_request_t` pointer) is the per-request id
that pairs `request-start` with `request-done`; it is unique for the
request's lifetime and costs nothing extra to obtain.

`queue-enqueue`/`queue-dequeue` are on the *app-level* SHM queue
(`nxt_app_queue_t`, router-to-worker), not the lower-level `nxt_nncq_t`
ring it is built on -- see the "Follow-ups" note in `usdt-plan.md` if a
second pair on the raw ring is wanted later.

`process-spawn` only fires in the parent, once per `fork()`, right after
the existing `nxt_debug(task, "fork(%s): %PI", ...)` line -- not
immediately after `fork()` itself, since both parent and child execute
the code between `fork()` and the `if (pid == 0)` child-return, which
would otherwise double-fire the probe (once per process).

## Verification performed

```
$ readelf -n build/sbin/unitd | grep -A2 stapsdt
```

lists all 8 probes that are reachable from `unitd` (everything above
except `queue-dequeue`, which only exists in application code -- see
`readelf -n build/unit_app_test`, and any real PHP/Python app process
built with `--usdt`). Confirmed against `build-usdt-gcc/sbin/unitd` and
`build-usdt-gcc/unit_app_test` for all 8 names.

Build matrix, all with `-Werror`:

| Compiler | `--usdt` | Result |
|---|---|---|
| gcc 13.3.0  | off | OK (`build/`) |
| gcc 13.3.0  | on  | OK (`build-usdt-gcc/`), `make -j2 tests` OK |
| clang 18.1.3 | off | OK (`build-nousdt-clang/`), `make -j2 tests` OK |
| clang 18.1.3 | on  | OK (`build-usdt-clang/`), `make -j2 tests` OK |

### Zero-cost check (default build, `--usdt` off)

`tools/perf/disasm-diff.sh` (Day-1) already tracks `nxt_port_socket_write2`
(the `port-send` call site) and `nxt_port_read_handler` as hot functions
with stored per-toolchain baselines. Running it after adding the probes:

```
$ sh tools/perf/disasm-diff.sh --cc gcc
$ sh tools/perf/disasm-diff.sh --cc clang
```

produced **no diff output** against the stored baselines for either
compiler (exit 0, nothing under "=== ... differs from baseline ==="),
confirming the probed functions disassemble identically with the probes
present but `NXT_HAVE_USDT` undefined -- i.e. `NXT_USDT()` truly costs
nothing when the feature is off. (clang additionally reprinted its
pre-existing "no disassembly found... nxt_port_msg_insert_tail
nxt_unit_port_queue_recv" warning, unrelated to this change -- those two
symbols were already fully inlined away under clang before any probe was
added.)

## bpftrace

`bpftrace` was installed and tried once against a `--usdt` build in this
container:

```
$ bpftrace -l 'usdt:build-usdt-gcc/sbin/unitd:*'
```

worked (static probe listing, all 8 names resolved, no BPF needed for
that). Actually attaching:

```
$ bpftrace -e 'usdt:build-usdt-gcc/sbin/unitd:freeunit:port__send { printf("hit\n"); }'
```

failed:

```
ERROR: Unknown error -1: couldn't set RLIMIT_MEMLOCK for bpftrace.
Attaching 1 probe...
open(/sys/kernel/tracing/uprobe_events): No such file or directory
ERROR: failed to detach probe: ...
```

i.e. this container has no BPF privileges and no tracefs
`uprobe_events`, as anticipated. The three scripts below are shipped
*unrun* -- reviewed for the probe args and semantics they rely on, not
verified against a live process:

- `tools/usdt/port-rtt.bt` -- port send-to-recv round-trip histogram.
- `tools/usdt/queue-residency.bt` -- app-queue slot residency (enqueue to
  dequeue) and current depth.
- `tools/usdt/requests-by-status.bt` -- request count and latency
  histogram grouped by HTTP status, plus in-flight count.

Each script's header comment repeats this caveat and gives its `-p`/`-c`
usage. Re-check them against a real `--usdt` binary on a host with BPF
access (e.g. `bpftrace -l` first to confirm probe names, then a short
attach) before trusting their output.

## Day-3 follow-ups

- Wire `queue-enqueue`/`queue-dequeue` into the N4 `/status` design
  (`docs/observability/status-extensions.md`) as the source of a live
  queue-depth counter, if that design is picked up next.
- Consider a second `process-spawn`-style probe at
  `nxt_process_start()` (`src/nxt_process.c`, the boot-time sibling of
  `nxt_process_create()`) if boot-time spawn needs to be distinguished
  from steady-state respawn, per the original plan's note.
- Run the three `.bt` scripts for real once a host with BPF access is
  available, and record actual output/latency numbers here.
