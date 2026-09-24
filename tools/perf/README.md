# tools/perf

Codegen and struct-layout regression gates for the hot IPC paths
(`nxt_port_queue`, `nxt_app_queue`, `nxt_nncq`/`nxt_app_nncq`, the
`nxt_port_mmap_*` chunk allocator) plus a microbenchmark and a
developer-side `llvm-mca` workflow. Background and tool evaluation:
see the Day-2 OPT stream report (not shipped in this tree).

## Gates (CI-facing)

### `disasm-diff.sh` -- codegen gate

Builds `unitd` (+ test binaries) with a given compiler, disassembles
the hot functions listed in `hot-functions.txt` with
`objdump -d --no-show-raw-insn`, normalizes out address/layout noise,
and diffs against a baseline built from another commit.

```sh
tools/perf/disasm-diff.sh --cc clang            # check against baseline
tools/perf/disasm-diff.sh --cc clang --update    # (re)write the baseline
tools/perf/disasm-diff.sh --cc gcc               # informational only, see below
```

Exits non-zero (and prints a normalized `diff -u`) when any tracked
function's codegen changed. `nm --print-size --size-sort` is the
companion size check -- see "size check" below.

The baseline is not stored in the tree: it depends on the exact
compiler, so it is generated on the same machine from the commit to
compare against (CI builds it from the PR's base commit, see
`.github/workflows/perf-gates.yml`). Locally:

```sh
git stash; tools/perf/disasm-diff.sh --cc clang --update; git stash pop
tools/perf/disasm-diff.sh --cc clang
```

clang is the canonical leg; gcc is informational only.
Baselines land in `tools/perf/baseline/<cc>-<version>-<libc>/` (ignored).

#### Harness for header-only inlines

`nxt_port_queue_send/recv`, `nxt_app_queue_send/recv/cancel`,
`nxt_nncq_enqueue/dequeue`, `nxt_app_nncq_enqueue/dequeue` and
`nxt_port_mmap_get_free_chunk` are `static inline` in their headers,
so at `-O1`/`-O2` they get folded into whatever caller happens to
survive, rather than keeping their own ELF symbol -- most of
`hot-functions.txt` tracks that surviving caller instead (see the
comments there). That works, but a caller's own unrelated changes
(or a change to some *other* inline it also folds in) shift the diff
for reasons that have nothing to do with the queue code itself.

`tools/perf/harness/mca_harness.c` wraps each of those inline
functions in a `__attribute__((noinline))` function (`mca_port_queue_send`,
`mca_nncq_dequeue`, ...), giving every one of them its own stable
symbol. `disasm-diff.sh` compiles it itself, every run, with the
exact `CC`/`CFLAGS`/`-I` the real build just used (read back from the
generated `$BUILD_DIR/Makefile`, so `--hardening` or any other
configure flag stays in sync automatically) and only ever
disassembles the resulting `.o` -- it is never linked into `unitd` or
`libunit`. The `mca_*` symbols are tracked in `hot-functions.txt`
alongside the caller-based ones.

### `layout-check.sh` -- struct-layout gate

```sh
tools/perf/layout-check.sh --cc clang            # check against baseline
tools/perf/layout-check.sh --cc clang --update   # (re)write the baseline
tools/perf/layout-check.sh --cc musl-gcc --update \
    --configure-opt=--no-regex                   # see musl leg below
```

Dumps `pahole -C <struct>` layouts (size, field offsets, holes,
cacheline boundaries) for the shared-memory (SHM) structs
`nxt_nncq_t`, `nxt_app_nncq_t`, `nxt_port_queue_t`, `nxt_app_queue_t`,
`nxt_port_queue_item_t`, `nxt_app_queue_item_t`,
`nxt_port_mmap_header_t`, `nxt_unit_request_t`, and the process-local
structs `struct nxt_port_s` and `nxt_unit_ctx_impl_t`, and diffs them
against `tools/perf/layout-baseline/<cc>-<libc>.txt`.

- A layout change to any of the **SHM structs** is a **hard failure**
  unless `tools/perf/layout-baseline/ABI-BUMP` has been touched in the
  same change (see "bumping the ABI baseline" below) -- these structs
  are read/written across process boundaries by more than one binary
  built at different times, so a silent layout change is a live ABI
  break, not just a local codegen concern.
- A layout change to a **process-local** struct (`nxt_port_s`,
  `nxt_unit_ctx_impl_t`) is a **warning only**: it never crosses a
  process boundary by value, so a reorder is a valid, ordinary
  optimization (see the `nxt_port_s` hot/cold reorder below) as long
  as it does not change any *other* SHM struct.

Baselines are generated for `clang-18.1.3-glibc-2.39` (the same
gate-blocking toolchain as `disasm-diff.sh`) and, when
`musl-tools` is installed, for `musl-gcc`. The eval found glibc and
musl x86_64 layouts byte-identical for every struct in this list
(including `pthread_mutex_t`-bearing ones), so the musl leg is a
belt-and-suspenders check for a future architecture where that stops
being true, not a leg expected to ever disagree with glibc here; it
runs the layout check only, never `disasm-diff.sh` (musl's own
`memcpy`/atomics codegen would just be permanent, uninteresting noise
against a glibc-built baseline).

#### Bumping the ABI baseline

If an SHM struct's layout is intentionally changing (a real, reviewed
ABI bump -- coordinate this across the router/app boundary, since
mismatched binaries then can't share memory), regenerate the baseline
with `--allow-abi-bump` in the same commit:

```sh
tools/perf/layout-check.sh --cc clang --update --allow-abi-bump
```

`--update` alone still writes a fresh baseline, but `layout-check.sh`
is meant to be re-run (without `--update`) as the actual gate; passing
`--allow-abi-bump` there too is what downgrades an SHM diff from a
hard failure to an acknowledged warning, so a reviewer looking at the
PR diff of `tools/perf/layout-baseline/*.txt` plus the flag in the
commit message/PR description is the paper trail -- there is no
separate marker file to forget to update.

## Developer tools (not gates)

### `bench-queues.sh` / `queue_bench` -- timing microbenchmark

```sh
./configure --tests && make -j2 tests
tools/perf/bench-queues.sh build
```

Uses `perf stat` hardware counters when they actually work; falls
back to `queue_bench`'s own `clock_gettime(CLOCK_MONOTONIC)` timings
otherwise. **In this container (and any CI runner without PMU
passthrough), hardware counters are unavailable** (`perf stat`
reports every event as `<not supported>`) -- treat the wall-clock
`ns/op` numbers as the metric, run a few repetitions, and expect
several percent of run-to-run noise on a shared/virtualized CPU.

### `llvm-mca` -- instruction-level analysis (developer-only, not a gate)

Not automated: extract a hot loop's disassembly into a `.s` file by
hand (or via a small script -- e.g. the normalized output the harness
symbols above already give you) and run:

```sh
llvm-mca-18 -mcpu=cascadelake --iterations=1000 \
    --timeline --bottleneck-analysis --instruction-tables loop.s
```

Good for a *relative* before/after comparison of one loop's port
pressure/uop count between two builds. **Not a source of absolute
latency numbers**: it models independent, pipelined iterations, while
the queues' `lock cmpxchg` retry loops are single-threaded chains of
dependent atomics -- llvm-mca's throughput estimate for
`nxt_nncq_dequeue` (`Block RThroughput ~3.5`) undershoots the
measured `queue_bench` latency (~57 cycles for a dequeue+enqueue
pair) by about 8x for exactly that reason. Use it to answer "did this
change make the uop schedule worse", not "how many nanoseconds will
this take".

## CI

`.github/workflows/perf-gates.yml` runs `disasm-diff.sh` (pinned to
`clang-18`; if the runner's clang is a different version, the job
prints a warning and skips the codegen diff rather than failing) and
`layout-check.sh`. **The workflow is `continue-on-error: true` for
now** -- it reports status on every PR but does not block merges,
until the baselines have survived a normal round of real traffic
(a few merged PRs) without false positives. Flip `continue-on-error`
to `false` once that's confirmed.
