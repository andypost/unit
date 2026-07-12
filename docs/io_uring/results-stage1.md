# Stage 1 benchmark results — io_uring poll-mode engine vs epoll

Date: 2026-07-12. Branch `feat/io-uring-stage1` (after the edge-delivery
correctness fixes, commits `235819ae` + `8a690980`).

## Method

- Harness: `tools/bench-engines.sh` (branch `bench/io-engine-harness`),
  5 interleaved A/B rounds, engine identity asserted from the
  `using "<name>" event engine` log line (`epoll_edge` vs `io_uring`).
- Builds: `./configure [--io-uring-default] && ./configure php`, same tree,
  non-debug. Server tree pinned to cores 0-3, load generator (oha 1.15
  primary, ab secondary) pinned to cores 4-7. Kernel 7.0, liburing 2.14.
- Noise floor established with a 3-round null A/A run (epoll vs epoll,
  `tools/bench-results-io-uring-aa-baseline.txt`): oha RPS ±5-8%,
  ab RPS ±1.5%, router CPU-µs/req ±1% on close/ka_hi (noisier on ka_lo/php).
- Raw data: `tools/bench-results-io-uring-stage1.txt`.

## Results (mean ± stdev over 5 rounds)

Scenarios: `ret200_ka_hi` = return-200 keepalive c=256; `ka_lo` = c=8
(listen-herd detector); `close` = Connection: close c=16; `proxy_ka` =
proxy→self c=64; `php_ka` = PHP hello app c=8 (process-tree CPU).

### Throughput (RPS)

| scenario | epoll (oha) | io_uring (oha) | Δ | Δ (ab) | verdict vs noise |
|---|---|---|---|---|---|
| ret200_ka_hi | 82720 ± 2862 | 82439 ± 2032 | -0.3% | +0.2% | parity |
| ret200_ka_lo | 56328 ± 4189 | 51070 ± 6229 | -9.3% | -0.7% | parity (ab); oha within its ka_lo noise |
| ret200_close | 13576 ± 517 | 13870 ± 89 | +2.2% | -0.6% | parity |
| proxy_ka | 10472 ± 62 | 10247 ± 112 | -2.1% | +2.1% | parity |
| php_ka | 20251 ± 2032 | 17792 ± 586 | -12.1% | -2.6% | parity-ish (oha php noise ±13% in A/A) |

### Router CPU µs/request (the tight metric; A/A noise ~±1-4%)

| scenario | epoll | io_uring | Δ (oha) | Δ (ab) |
|---|---|---|---|---|
| ret200_ka_hi | 31.4 | 32.5 | **+3.6%** | **+18.7%** |
| ret200_ka_lo | 33.2 | 39.7 | **+19.6%** | +6.8% |
| ret200_close | 122 | 152 | **+24.7%** | **+49.5%** |
| proxy_ka | 379 | 385 | +1.6% | -0.6% |
| php_ka (tree) | 157 | 182 | **+15.4%** | **+39.7%** |

Latency: p50 parity everywhere (≤ +4% outside php's noisy ±15%); p99 within
A/A noise both ways (close p99 -39% better, ka_lo p99 +34% worse, both with
large stdevs).

## Reading

1. **Throughput and latency: parity.** On the tight ab metric every scenario
   is within ±2.6%. Nothing regressed user-visibly; zero failed requests in
   all 50 measurements.
2. **Router CPU per request regresses 4-50%, scaling with connection rate.**
   The regression concentrates where connections are created frequently
   (`close`, `php_ka` at low concurrency, ab's lower request rates); it
   vanishes on `proxy_ka` (long-lived conns, work dominated by relay).
   Two known causes, both design-understood:
   - **Listen-socket thundering herd** (design §1.8): every new connection
     wakes all 8 worker rings; epoll has `EPOLLEXCLUSIVE`, `POLL_ADD` has no
     analogue. Dominant in `close`/accept-heavy runs.
   - **BLOCKED-disarm/re-arm SQE churn** (deliberate Stage-1 choice): a CQE
     landing on a BLOCKED direction costs a POLL_REMOVE + later POLL_ADD,
     ~1 extra SQE pair per proxied/keepalive request, where epoll edge pays
     nothing.
3. **Stage-1 poll-mode was expected to be at best parity** (it removes
   `epoll_ctl` churn but adds its own arm churn; the recv/accept/send
   syscalls are unchanged). The gate criterion was parity-or-better on
   stability and throughput, which holds.

## Implications for Stage 2

- **Multishot accept (5.19+) directly eliminates the dominant regression**:
  accept is a consuming, wake-one operation — N rings with multishot accepts
  behave like N threads blocked in `accept(2)`, killing the herd (design
  §1.8/§2.1). The `close`-scenario CPU gap is the quantified motivation.
- **Buffer-ring multishot recv (6.0+)** then removes the per-read `recv(2)`
  and one copy — the actual win the effort targets.
- **Stage-1 refinement worth testing alongside**: epoll-edge-style
  "stay armed while BLOCKED, latch-only" instead of disarm-on-BLOCKED,
  removing the per-request SQE pair (needs the double-dispatch dedupe to
  keep holding, which it does — dispatch is gated on state, not arming).

## Correctness findings during this phase (fixed on the branch)

Three real bugs found only under concurrent proxy load, all rooted in
multishot poll's edge-like delivery (design §1.7a): pending-EOF loss
(proxy wedge), double handler dispatch per CQ drain (router SIGSEGV), and
non-monotonic CQE masks erasing the EOF latch. Plus: engine-create fallback
moved into `nxt_event_engine_create()/_change()` so ring exhaustion in any
process degrades to epoll instead of dying. Soak: 3200/3200 proxied requests
clean, 10/10 SIGTERM-under-load clean shutdowns.
