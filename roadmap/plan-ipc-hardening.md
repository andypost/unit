# Plan: IPC layer hardening pass

## Context

PRs #6 (TLS OCSP stapling) and #8 (mp-pool retain & fd/buffer leaks in cert/script
IPC) closed the most urgent leaks but left several siblings open. Reviewers
(Gemini code-assist + the user-supplied audit plan that drove #8) flagged a
broader pattern: `(void) nxt_port_socket_write(...)` discarding return values
across the IPC layer, plus implicit "ends-with-`/`" path-join assumptions in the
cert/script/OCSP store handlers.

This PR consolidates those follow-ups into one cohesive pass so the codebase
doesn't carry split conventions across cert/script/OCSP/conf/socket reply paths.

## Goal

When this lands:

- Every main-process IPC reply that ships an fd via `NXT_PORT_MSG_CLOSE_FD`
  closes the fd explicitly on `nxt_port_socket_write() != NXT_OK`.
- Every main-process IPC reply that ships a `nxt_buf_t *b` runs the buf's
  `completion_handler` manually on send failure.
- Every router-side `nxt_port_socket_write(...)` that retains the temp_conf
  mp pool retains *after* the send succeeds (matches PR #8 pattern; matches
  the existing-correct `nxt_router_access_log_reopen` shape).
- The cert/script/OCSP store path-join uses a small helper that tolerates
  `rt->{certs,scripts}.start` not ending in `/`.

## Scope

### Sender-side mp-pool retain (matches PR #8 finding 1)

PR #8 fixed `nxt_cert_store_get` and `nxt_script_store_get`. PR #6 fixed the
OCSP twin `nxt_cert_store_get_ocsp`. Audit the remaining sites:

```bash
git grep -n 'nxt_mp_retain' src/
```

Known sites at master (re-verify before editing):

- [x] `src/nxt_cert.c:1117` `nxt_cert_store_get`        — fixed in PR #8
- [x] `src/nxt_script.c:483` `nxt_script_store_get`     — fixed in PR #8
- [x] `src/nxt_router_access_log.c:579` reopen path      — already correct upstream
- [ ] (post-PR #6) `src/nxt_cert.c` `nxt_cert_store_get_ocsp` — fixed in PR #6 (`52c9b54`)
- [ ] **Audit:** any other site that pairs `nxt_buf_mem_alloc(mp, ...)` →
      `nxt_mp_retain(mp)` → `b->completion_handler = nxt_xxx_buf_completion`.
      Likely none, but confirm.

### Receiver-side fd-close-on-send-failure (matches PR #8 finding 4)

PR #8 fixed cert/script/socket/access-log handlers. Remaining sites in the
main process that send fds with `NXT_PORT_MSG_CLOSE_FD` and discard the
return value:

```bash
git grep -n 'NXT_PORT_MSG_CLOSE_FD' src/
git grep -n '(void) nxt_port_socket_write' src/
```

Known candidates (re-verify line numbers before edit; numbers from the audit
done during PR #8):

- [x] `src/nxt_cert.c:1227` `nxt_cert_store_get_handler`          — fixed in PR #8
- [x] `src/nxt_script.c:587` `nxt_script_store_get_handler`        — fixed in PR #8
- [x] `src/nxt_main_process.c:1156` `nxt_main_port_socket_handler` — fixed in PR #8
- [x] `src/nxt_main_process.c:1724` `nxt_main_port_access_log_handler` — fixed in PR #8
- [ ] (post-PR #6) `src/nxt_cert.c` `nxt_cert_store_get_ocsp_handler` — fixed in PR #6
- [ ] **Audit:** anything else that builds `type = ... | NXT_PORT_MSG_CLOSE_FD`
      and sends via `nxt_port_socket_write`. Probably nothing left, but verify.

### Buffer-completion-on-send-failure (matches PR #8 reviewer finding 4 detail)

Anywhere `nxt_port_socket_write(..., b)` is called with a non-NULL buf whose
`completion_handler` releases an mp retain (or any other resource), and the
return value is discarded — invoke the handler manually on the failure path.

Known candidate after the PR #8 fixes:

- [x] `src/nxt_main_process.c` `nxt_main_port_socket_handler` `out` buffer
      — fixed in PR #8.

Audit:

```bash
git grep -nE '\(void\) *nxt_port_socket_write|nxt_port_socket_write\([^)]*,\s*b\b'
```

Sites mentioned during PR #8 review as low-impact shutdown leaks:

- `src/nxt_runtime.c:511` `nxt_runtime_stop_app_processes()` cascade
- `src/nxt_runtime.c:533` `nxt_runtime_stop_all_processes()` cascade
- `src/nxt_application.c:716` `nxt_proto_quit_children()` cascade

Classify each:

1. No fd, no buffer with refcount-bearing completion → safe to ignore (note
   in commit message).
2. Fd with `CLOSE_FD` → close fd on `!= NXT_OK`.
3. Buffer with refcount-bearing completion → run completion on `!= NXT_OK`.
4. Both → do both, FD first (matches `nxt_port_error_handler` ordering
   at `src/nxt_port_socket.c:1361` — `nxt_port_msg_close_fd(msg)` runs
   before each buf's `completion_handler` is queued).

### Path-join helper (gemini PR #6 finding 3, deferred)

Today every `*_store_*_handler` open does:

```c
file.name = nxt_malloc(rt->certs.length + name.length + 1);
p = nxt_cpymem(file.name, rt->certs.start, rt->certs.length);
p = nxt_cpymem(p, name.start, name.length + 1);
```

Three implicit assumptions:

1. `rt->{certs,scripts}.start` ends with `/`.
2. `name.start` is null-terminated and doesn't contain `/` or `..`.
3. Caller-provided `name.length` is the byte count *excluding* the NUL.

Today's invariant comes from `nxt_runtime_state_directory()`, which is fine
but undocumented at the call sites. Suggested helper next to `nxt_runtime`:

```c
/*
 * Build "<dir>/<name><suffix>" into a freshly nxt_malloc'd buffer.
 * dir is taken from rt->certs / rt->scripts and may or may not have a
 * trailing slash.  name is taken from a port message and is treated as
 * opaque bytes (no '/' / '..' rejection here; callers must validate).
 * suffix may be empty.  Returns NULL on alloc failure.
 */
nxt_file_name_t *nxt_runtime_resolve_store_path(const nxt_str_t *dir,
    const nxt_str_t *name, const nxt_str_t *suffix);
```

Concrete callers to convert (these are the ones whose path construction the
gemini comment flagged):

- `nxt_cert_store_get_handler`        (`src/nxt_cert.c`)
- `nxt_cert_store_get_ocsp_handler`   (`src/nxt_cert.c`, post-PR #6)
- `nxt_script_store_get_handler`      (`src/nxt_script.c`)
- `nxt_cert_store_delete_handler`     (`src/nxt_cert.c`) — uses the same shape

Not required:

- `nxt_main_port_access_log_handler`  — `path` comes from msg buf, not
  joined to a base dir; keep as-is.
- `nxt_main_port_conf_store_handler`  — uses `rt->conf` / `rt->conf_tmp`
  directly without joining, no leaf-name from IPC.

### Cert-name validation (security adjacency)

While touching the cert/script handlers, also validate that the IPC-supplied
`name` doesn't contain path separators or escape sequences. This is implicit
today — the controller validates cert/script names before storing — but
defense-in-depth is cheap once the helper exists. Add a `nxt_name_safe()`
predicate alongside the path helper:

```c
/*
 * True if name is a pure leaf: ASCII alnum + ['-_.'], no '/', no NUL,
 * no leading dot.  Used as a defensive check at the trust boundary
 * between the controller (validated input) and the main process
 * (privileged file operations).
 */
nxt_bool_t nxt_name_is_safe_leaf(const nxt_str_t *name);
```

Reject in handlers; alert; reply with `NXT_PORT_MSG_RPC_ERROR`.

## Out of scope (call out explicitly)

- The `nxt_mp_retain` audit in **non**-cert/script paths (router request
  pipeline, etc.). Those mp pools have different lifetime contracts and
  should be a separate pass.
- IPC layer **structural** changes (unifying the reply pattern into a single
  helper). Tempting but expands the diff and risks behavioral drift; defer.
- Cleaning up `nxt_runtime.c:511`/`:533` shutdown cascades unless they
  classify into category 2/3/4 above. Most likely category 1 (purely
  best-effort process-exit signaling).
- General FD-lifetime hygiene across the rest of the codebase (audit
  slot PR-E: accept-CLOEXEC, pipe-CLOEXEC, compression mmap FD leak,
  plain `accept()` without CLOEXEC, etc.). The scope overlaps
  conceptually but the bugs are pre-existing and unrelated to the IPC
  reply-failure path; track and ship separately.

## Suggested commit shape

One commit, one diff, single PR. Title:

```
fix(port): finish cert/script/OCSP IPC hardening pass
```

Body summarizes the four bullets above with the exact site list.

## Test plan

- [ ] Build clean (`./configure --openssl && ./configure python && make -j`).
- [ ] `pytest test/test_tls.py test/test_tls_sni.py test/test_tls_ocsp.py`
      — TLS reload paths exercise cert_store_get sender + receiver.
- [ ] `pytest test/test_configuration.py test/test_access_log.py`
      — listener / log paths exercise main_process handlers.
- [ ] `pytest test/test_njs_modules.py` (if present) — script_store paths.
- [ ] Manual: `nxt_router_access_log_reopen()` triggered via `SIGUSR1`.
- [ ] Manual: cert delete via `DELETE /certificates/<name>` exercises
      `nxt_cert_store_delete_handler` if its path-join was changed.

The leak paths themselves still require fault injection to exercise
deterministically. See `roadmap/plan-malloc-injection.md` — the natural
companion PR — for the harness.

## Effort estimate

- Sender-side mp-pool audit: 2 hours (mostly grep + verify, likely zero
  remaining sites).
- Receiver-side fd/buffer audit + edits: 4 hours.
- Path-join helper + 4 call-site conversions: 4 hours.
- Name-safety predicate + 4 reject-tests: 3 hours.
- Tests + CHANGES + commit message: 2 hours.

Total: **~2 days** for one focused session.

## Forwarding

Both leak shapes are pre-existing in upstream `freeunitorg/freeunit`. Once
this lands and is exercised here, the same diff should be filed as an
upstream PR — same content, retitled, no FreeUnit-specific framing in the
body. (Same posture as PR #6 and PR #8 from this branch.)

---

## Quick-reference command bag

For the next session, here are the exact commands to start from:

```bash
# 1. Set up branch.
git fetch origin master
git checkout -b claude/ipc-hardening-pass origin/master

# 2. Map the work surface.
git grep -n 'nxt_mp_retain'                       src/
git grep -n 'NXT_PORT_MSG_CLOSE_FD'               src/
git grep -nE '\(void\) *nxt_port_socket_write'    src/
git grep -n 'rt->certs\|rt->scripts'              src/

# 3. Sanity-check the existing fixes that landed in PR #6 / PR #8 are
#    already on master before adding the new helper:
git log --oneline --grep='cert/script' origin/master
git log --oneline --grep='OCSP'        origin/master

# 4. After implementing, the regression set:
./configure --openssl
./configure python --config=python3-config
make -j$(nproc)
pytest test/test_tls.py test/test_tls_sni.py test/test_tls_ocsp.py \
       test/test_configuration.py test/test_access_log.py -q
```
