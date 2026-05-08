# Roadmap & implementation plans

Detailed plans for upcoming work. Each `plan-*.md` is self-contained: a
new session can pick one up and execute it without prior context.

## Current

- [`plan-ipc-hardening.md`](plan-ipc-hardening.md) — finish the IPC layer
  cleanup pass started by PRs #6 and #8 (path-join helper, audit of
  `(void) nxt_port_socket_write(...)` sites, leaf-name validation).
- [`plan-malloc-injection.md`](plan-malloc-injection.md) — `LD_PRELOAD`
  fault-injection harness so the leak fixes from PRs #6 and #8 are
  regression-fenced.

These two plans are designed to be tackled as separate PRs; the malloc
harness is a natural follow-up to the IPC hardening pass and would
backfill regression coverage for both PRs after the fact.

## See also

The repo-wide security audit
([gist `andypost/e04a4a642e168de2b8435a593f03b84b`](https://gist.github.com/andypost/e04a4a642e168de2b8435a593f03b84b))
catalogues 45+ findings across 14 vectors and slots them into PRs
PR-A through PR-I. The plans in this folder sit **outside** that
tracker — they're follow-on cleanup of PR #56's precedent (see the
audit appendix "Known/Already-Fixed"). The malloc-injection harness is
also useful for fencing audit findings that need allocator failures to
trigger (e.g. V11 — compression mmap FD leak).
