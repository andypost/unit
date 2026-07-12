# Stage 2 benchmark results — io_uring (tier ACCEPT) vs epoll

Date: 2026-07-12. Branch `feat/io-uring-stage2` (5 commits on Stage 1:
tier detection + FORCE_TIER, stay-armed-while-BLOCKED, oneshot accept —
after the multishot-accept starvation fix `4fefba69`), quiet box.
Method identical to results-stage1.md (5 interleaved rounds, oha + ab,
engine asserted, noise floor from the A/A baseline). Raw:
`tools/bench-results-io-uring-stage2.txt`.

## Results (Δ io_uring vs epoll; ab = tight metric ±1.5%, oha RPS ±5-8%)

### Throughput (RPS)

| scenario | Δ oha | Δ ab | verdict |
|---|---|---|---|
| ret200_ka_hi | +1.3% | +0.6% | parity |
| ret200_ka_lo | -1.8% | -1.0% | parity |
| ret200_close | +0.1% | -1.5% | parity |
| php_ka | -6.6% | -0.3% | parity (oha php noise ±13%) |
| proxy_ka | **-8.7%** | **-8.7%** | real regression |

### Router CPU µs/request

| scenario | epoll | io_uring | Δ oha | Δ ab | Stage-1 Δ (oha/ab) for comparison |
|---|---|---|---|---|---|
| ret200_ka_hi | 31.2 | 32.0 | +2.6% | +3.8% | +3.6% / +18.7% |
| ret200_ka_lo | 37.5 | 38.2 | +1.9% | +15.5% | +19.6% / +6.8% |
| ret200_close | 120 | 129 | **+7.1%** | **+12.5%** | +24.7% / +49.5% |
| php_ka (tree) | 156 | 176 | +12.6% | +13.8% | +15.4% / +39.7% |
| proxy_ka | 378 | 418 | **+10.4%** | **+11.3%** | +1.6% / -0.6% |

Latency: p50 parity on direct scenarios; proxy p50 +10% (+0.6ms) but proxy
**p99 -17..-19% (better)** — accept-path latency moved from tail to median.

## Reading

1. **Throughput parity everywhere except proxy (-8.7%).** Attributed by a
   tier bisect (`FORCE_TIER=poll` proxy ≈ 10.1k ≈ epoll; `=accept` ≈ 9.5k):
   the oneshot-accept re-arm round-trip adds latency to the proxy's
   per-request internal peer connections (upstream closes per request, so
   proxy-to-self is maximally accept-bound). Direct `close` (client-side
   accept churn) shows no throughput cost.
2. **The accept-path CPU regression halved vs Stage 1** (`close`
   +25/+50% → +7/+12.5%; php +15/+40% → +13/+14%): the worker-distributed
   oneshot accept beats Stage 1's thundering-herd poll listener, but each
   accept still costs an SQE re-arm + CQE + `getpeername()` vs epoll's
   amortized `EPOLLEXCLUSIVE` + `accept4()` batch drain.
3. **Keepalive CPU is near-parity now** (+2-4% oha) — the
   stay-armed-while-BLOCKED fix removed Stage 1's per-request SQE churn
   (ka_hi ab: +18.7% → +3.8%).
4. **Net Stage-2 standing: parity-minus.** No scenario shows an io_uring
   win on this workload set; the poll-mode bridge plus completion-mode
   accept costs slightly more CPU than epoll's mature paths. The projected
   wins were always in the **deferred RECV tier** (buffer-ring multishot
   recv: removes per-read `recv(2)` + one copy), which remains detected but
   clamped pending the three documented constraints (conn-only scoping,
   buffer lifetime across the app round-trip, read discipline).

## Journey notes (what the phased benching caught)

- Stage-2 v1 (multishot accept) looked like a CPU win (-8..-32%) but was
  **worker starvation**: a persistent wake-one registration funneled 32/32
  connections to one ring; 7/8 workers idle; throughput at single-worker
  ceiling (design §2.1a). Caught by per-thread accept accounting; fixed
  with oneshot accept re-armed per completion (distribution now 11-14%
  across all 8 workers).
- SINGLE_ISSUER/DEFER_TASKRUN: kernel binds the submitter task at setup;
  router workers poll on a different thread than the creator → off by
  default (design §2.5 finding).

## Options from here

- **A. Land as opt-in experimental** (epoll default, `--io-uring` opt-in):
  correctness is soaked (0 failures in all measurement runs; SIGTERM-under-
  load clean), architecture documented, and the RECV tier follow-up has a
  clean base. Cost: none to default users.
- **B. Hold PRs until the RECV tier ships** and demonstrates a win.
- **C. Also invest in accept-path tuning first** (e.g. multi-accept batching
  per re-arm, skipping `getpeername()` via `accept4`-style addr reuse, or
  keepalive upstream conns for the proxy) to close the -9% proxy gap.
