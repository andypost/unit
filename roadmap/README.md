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
