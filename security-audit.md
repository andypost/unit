# FreeUnit Security & Leak Audit

**Audited commit:** `7b126961` (master, 2026-05-08)
**Methodology:** read-only static review across 14 vectors, one Explore subagent per vector (Claude Code). No code was modified during the audit; this report enumerates findings only.
**Status / PR slot:** every finding tagged with a `PR: PR-X` slot below; status flips to "fixed" as each PR merges. See **Remediation tracker** for an at-a-glance view.

> Severity is the auditor's recommendation, not a CVSS computation. "Critical/High" merit a near-term fix; "Medium" merit a tracked issue; "Low/Informational" are notes for hygiene.

---

## Executive summary

| Severity | Count |
|----------|-------|
| Critical | 1 |
| High | 17 |
| Medium | 28 |
| Low | 9 |
| Informational | 3 |

**Top 5 findings (auditor's pick):**

1. **[Critical] Cgroup TOCTOU around `/proc/self/cgroup`** — `src/nxt_process.c:619`, `src/nxt_cgroup.c:102`. Parent reads `/proc/self/cgroup` and writes the resulting path while the child is mid-unshare into a new cgroup namespace. Resource-control bypass possible. (V6)
2. **[High] Missing `SSL_CTX_check_private_key()`** — `src/nxt_openssl.c:522`. A mismatched key/cert pair is accepted silently; production deployments may run with the wrong key without warning. (V3)
3. **[High] Untrusted `chunk_id` and `chunk_id + nchunks` from shmem peer** — `src/nxt_port_memory.c:698,701`. No bounds check before pointer arithmetic on a mapped region; OOB read/write reachable from any peer process that can craft a port message. (V5)
4. **[High] Java `InputStream.readLine(byte[], off, len)` lacks bounds check** — `src/java/nxt_jni_InputStream.c:89`. `off`/`len` from app are used without validating against `GetArrayLength()`; heap corruption from app-supplied arguments. (V9)
5. **[High] WebSocket frame-size decrement is a no-op** — `src/nxt_http_websocket.c:87`. `frame_size -= copy_size` runs after `copy_size` has been decremented to zero, so the outer loop never advances; data beyond the declared payload can be copied out. (V12)

---

## Remediation tracker

PR slots (`PR-A` through `PR-I`) are defined in **Recommended next actions** at the bottom of this file. PR #8 on `andypost/unit` (`fix(port): plug mp-pool retain and fd/buffer leaks in IPC reply paths`) is the precedent and is listed in the Appendix; it does not appear here because its findings predate this audit.

| Vector | Finding | Severity | PR slot | Status |
|---|---|---|---|---|
| V6 | Cgroup TOCTOU around /proc/self/cgroup | Critical | PR-C | open |
| V1 | Proxy Content-Length underflow | High | PR-F | open |
| V3 | Missing `SSL_CTX_check_private_key()` | High | PR-D | open |
| V4 | Unbounded JSON recursion | High | PR-H | open |
| V4 | Unbounded JSON array/object count | High | PR-H | open |
| V4 | Control socket has no peer-cred check | High | PR-C | open |
| V5 | `chunk_id` no bounds check | High | PR-B | open |
| V5 | `chunk_id + nchunks` overflow | High | PR-B | open |
| V6 | Mount-destination symlink TOCTOU | High | PR-C | open |
| V7 | PHP `isspace` skip past header end | High | PR-G | open |
| V8 | WSGI environ NULL after copy fail | High | PR-G | open |
| V9 | Java `InputStream.readLine` no bounds | High | PR-G | open |
| V10 | WS hsize OOB read in libunit | High | PR-A | open |
| V11 | Compression mmap FD leak | High | PR-E | open |
| V12 | 64-bit payload MSB not validated | High | PR-A | open |
| V12 | Frame-size decrement no-op | High | PR-A | open |
| V14 | Accept sockets missing CLOEXEC | High | PR-E | open |
| V14 | Pipe leak on nonblocking-set failure | High | PR-E | open |
| V1 | Proxy CL > OFF_T_MAX handling | Medium | PR-F | open |
| V2 | IPv4 /32 fallthrough | Medium | PR-I | open |
| V2 | Asymmetric pattern/request decode | Medium | PR-I | open |
| V2 | `nxt_regex_match_create(..,0)` size arg | Medium | PR-I | open |
| V2 | PCRE2 ReDoS | Medium | _excluded (DoS policy)_ | doc-only |
| V3 | Cert chain mutated during reload | Medium | PR-D | open |
| V3 | `realloc` failure leaves count inconsistent | Medium | PR-D | open |
| V4 | Validator allows arbitrary executable | Medium | PR-H | open |
| V5 | RPC stream-id wrap | Medium | PR-B | open |
| V5 | No sender-type ACL on dispatch | Medium | PR-C | open |
| V5 | TOCTOU on shmem mmap_id lookup | Medium | PR-B | open |
| V7 | `realpath` failure leaks `tmp` buffer | Medium | PR-G | open |
| V7 | TrueAsync skips `php_request_shutdown` | Medium | PR-H | open |
| V8 | ASGI WS pending_payload_len overflow | Medium | PR-A | open |
| V8 | Ruby per-request io not reset | Medium | PR-H | open |
| V8 | Perl ERRSV not cleared on init failure | Medium | PR-G | open |
| V9 | Java `sendWsFrame` no capacity check | Medium | PR-A | open |
| V9 | WASM offset arithmetic unchecked | Medium | PR-G | open |
| V9 | WASM `send_headers(offset)` unchecked | Medium | PR-G | open |
| V10 | WS `buf.free` advance unchecked | Medium | PR-A | open |
| V10 | Response field count×size overflow | Medium | PR-B | open |
| V10 | `nxt_unit_websocket_retain()` hsize unchecked | Medium | PR-A | open |
| V12 | RSV bits not validated | Medium | PR-A | open |
| V12 | No ping/pong rate limit | Medium | _excluded (DoS policy)_ | doc-only |
| V13 | `nxt_is_complex_uri_encoded` off-by-one | Medium | PR-F | open |
| V14 | Pipe FDs lack CLOEXEC | Medium | PR-E | open |
| V14 | `nxt_file_redirect` close transient | Medium | PR-E | open |
| V14 | `accept4()` ENOSYS detection | Medium | PR-E | open |
| V2 | Port-range parsing off-by-one | Low | PR-I | open |
| V2 | Host matching always lowercased | Low | PR-I | open |
| V3 | Wildcard SAN OOB read | Low | PR-D | open |
| V5 | PID reuse confusion across mmap | Low | PR-B | open |
| V7 | PATH_INFO not NUL-terminated | Low | PR-G | open |
| V8 | ASGI lifespan checks wrong NULL | Low | PR-G | open |
| V10 | sptr offset dereferenced without bounds | Low | PR-B | open |
| V13 | `nxt_rmemstrn` length underflow | Low | PR-F | open |
| V14 | `socketpair` SO_PASSCRED handling | Low | PR-E | open |
| V1 | Slowloris on chunked-encoding | Informational | _doc-only_ | doc-only |
| V3 | OCSP stapling commit `5d35b44f` review | Informational | _doc-only_ | doc-only |
| V6 | `capset()` is never called | Informational | PR-I (docs) | open |

After each PR merges, the relevant rows flip from `open` → `fixed (andypost/unit#NN)`, and the per-finding `- PR:` bullet picks up the merged-PR reference.

---

## Themes (cross-vector patterns)

### Trust-untrusted-input-from-shmem-peer
The router writes structures into shared memory (`nxt_port_memory.c`), language modules and the main process read them. Several sites dereference offsets without confirming they fall within the mapped region:

- V5: `nxt_port_mmap_chunk_start()` (`src/nxt_port_memory.c:698,701`).
- V10: WebSocket `hsize` and `sptr` offsets in libunit (`src/nxt_unit.c:1637,1694,3455,1329`).
- V10: `max_fields_count * sizeof(field) + 2` integer overflow on response build (`src/nxt_unit.c:2049,2128`).

These are the same shape: an offset/size that came across a process boundary, used in pointer arithmetic without being checked against the buffer end.

### Bounds checks missing on language-binding boundaries
Across SAPIs, the C side trusts arguments coming from app code (which is often the threat boundary in shared-isolation deployments):

- V9: Java `readLine(off,len)` (`src/java/nxt_jni_InputStream.c:89`), Java `sendWsFrame(pos,len)` (`src/java/nxt_jni_Request.c:731`).
- V9: WASM `send_headers(offset)` and request-handler offset arithmetic (`src/wasm/nxt_wasm.c:43,104`).
- V7: PHP header-value `while (isspace(*value))` skip without buffer-end check (`src/nxt_php_sapi.c:1794`).

### CLOEXEC and FD lifetime on error paths
PR #56 fixed two FD leaks; the audit found additional ones in adjacent code:

- V14: `accept()` / `accept4()` paths leave inherited FDs available across `exec` (`src/nxt_conn_accept.c:161`, `src/nxt_epoll_engine.c:1028`, plain pipes in `src/nxt_file.c:728`).
- V14: `nxt_pipe_create()` leaks one end if `nxt_fd_nonblocking()` fails (`src/nxt_file.c:737`).
- V11: Compression mmap failure orphans the input fd (`src/nxt_http_compression.c:280`).

### Config-supplied resource exhaustion
Control-plane DoS surfaces:

- V4: JSON parser has unbounded recursion and unbounded array/object element count (`src/nxt_conf.c:1387,1548`).
- V2: Configured PCRE2 patterns run with no JIT/match limit; ReDoS via routes (`src/nxt_http_route.c:2148`).

These are exploitable only by whoever can write to the control socket — but see V4/control-socket auth.

### Auth on the control socket
- V4: The control socket has no peer-credential check; any local user with write access to the socket file can mutate config (`src/nxt_controller.c:720`). Mitigation today is filesystem permissions on `control.unit.sock`. Worth documenting and tightening (SO_PEERCRED + UID match).

---

## V1 — HTTP/1 parser & request lifecycle

**Scope examined:** `nxt_h1proto.c`, `nxt_h1proto.h`, `nxt_h1proto_websocket.c`, `nxt_http_parse.c`, `nxt_http_parse.h`, `nxt_http_request.c`, `nxt_http_chunk_parse.c`, `nxt_http_proxy.c`.

### [High] Integer underflow in proxy response Content-Length tracking
- File: `src/nxt_h1proto.c:2881-2883`
- Class: logic / integer underflow
- Trigger: Upstream sends more bytes than its `Content-Length` declares.
- Impact: A — `h1p->remainder` (signed) underflows; subsequent `> 0` checks silently drop excess data. Inconsistent flag is set on close, but underflow value could be used in arithmetic if downstream code changes.
- Suggested fix: Validate `length <= h1p->remainder` before subtracting; truncate and flag inconsistency immediately.
- PR: PR-F

### [Medium] Unsafe proxy Content-Length overflow handling
- File: `src/nxt_http_proxy.c:407-424`
- Class: logic
- Trigger: Upstream sends `Content-Length` > `NXT_OFF_T_MAX`.
- Impact: A — `nxt_off_t_parse()` returns -2; the `n >= 0` check skips assignment, leaving content_length_n at -1. Defensively handled at the call site, but overflow is not logged.
- Suggested fix: Distinguish `n == -2` and log/error explicitly.
- PR: PR-F

### [Informational] Slowloris on chunked-encoding
Misconfigured timeouts allow extreme slow-drip chunked bodies. Code is correct; configuration concern only.
- PR: _doc-only (Informational)_

CL/TE smuggling is correctly rejected at `src/nxt_h1proto.c:896` (request with both headers errors out). No memory-safety issues found in primary parsing paths.

---

## V2 — HTTP routing & access control

**Scope examined:** `nxt_http_route.c`, `nxt_http_route_addr.c`, `nxt_http_route_addr.h`, `nxt_http_proxy.c`, `nxt_http_return.c`, `nxt_pcre2.c`, `nxt_pcre.c`.

### [Medium] PCRE2 ReDoS via configured route patterns
- File: `src/nxt_http_route.c:2148-2157`
- Class: DoS
- Trigger: Operator configures a pathological regex (e.g. `~^(a+)+b`) plus matching input.
- Impact: A — CPU exhaustion in router process. No match/JIT limit applied.
- Suggested fix: Set `pcre2_set_match_limit()` / `pcre2_set_depth_limit()` on compile; document complexity ceiling.
- PR: _excluded (DoS policy)_

### [Medium] IPv4 `/32` CIDR fallthrough
- File: `src/nxt_http_route_addr.c:259-263`
- Class: logic
- Trigger: `/32` parses correctly today but doesn't set `match_type = EXACT`; falls through to re-parse. Harmless now, fragile under refactor.
- Suggested fix: Set `EXACT` explicitly when `cidr_prefix == 32`.
- PR: PR-I

### [Medium] Asymmetric pattern/request URI decoding
- File: `src/nxt_http_route.c:1228-1231,1180-1214`
- Class: logic
- Trigger: Pattern with `%2e%2e` may decode at compile-time differently from request-time decoding semantics.
- Impact: I — matcher bypass under specific encoding combinations.
- Suggested fix: Ensure pattern and request go through identical decode passes (or none).
- PR: PR-I

### [Medium] `nxt_regex_match_create(.., 0)` size argument
- File: `src/nxt_http_route.c:2150`
- Class: logic
- Trigger: Hardcoded 0 may produce undefined behavior depending on PCRE2 version.
- Suggested fix: Pass actual capture-group count or a documented sentinel.
- PR: PR-I

### [Low] Port-range parsing off-by-one
- File: `src/nxt_http_route_addr.c:286`
- Class: logic
- Trigger: `memchr(.., port.length - 1)` skips the last byte; a trailing `-` is silently ignored.
- Suggested fix: Drop the `-1` and validate `port.length >= 3` before searching.
- PR: PR-I

### [Low] Host matching always lowercased
- File: `src/nxt_http_route.c:496-497`
- Class: logic
- Trigger: Implicit `LOWCASE` matcher; admin expecting case-sensitive host filtering is silently overridden.
- Suggested fix: Document the behavior or add a per-rule sensitivity flag.
- PR: PR-I

---

## V3 — TLS / OpenSSL / certificates / OCSP

**Scope examined:** `nxt_openssl.c`, `nxt_cert.c`, `nxt_cert.h`, `nxt_tls.h`, plus commit `5d35b44f` (static OCSP stapling, on `claude/tls-modernization-unit-NQehK`, not on master).

### [High] Missing `SSL_CTX_check_private_key()` after key load
- File: `src/nxt_openssl.c:522`
- Class: crypto / logic
- Trigger: Cert and key are loaded; mismatch is detected only when the first handshake fails.
- Impact: I/A — config silently accepts wrong key; failed handshakes look like client problems.
- Suggested fix: Call `SSL_CTX_check_private_key(ctx)` after `SSL_CTX_use_PrivateKey()` and fail the bundle install on mismatch.
- PR: PR-D

### [Medium] Cert chain mutated on active SSL_CTX during reload
- File: `src/nxt_openssl.c:504-510`
- Class: logic / race
- Trigger: Concurrent handshakes during config reload may see a partial chain because `SSL_CTX_add0_chain_cert()` mutates the active context.
- Suggested fix: Build a new SSL_CTX and atomically swap; never modify a context that is in service.
- PR: PR-D

### [Medium] `realloc` failure leaves chain count inconsistent
- File: `src/nxt_cert.c:278-283`
- Class: logic
- Trigger: `nxt_realloc()` fails after at least one X509 added; partial state propagates to `nxt_cert_destroy()`.
- Suggested fix: Either decrement `count` before freeing, or fail without freeing the last X509 and let destroy walk the consistent count.
- PR: PR-D

### [Low] Wildcard SAN matcher modifies `str.start`/`str.length` without bounds check
- File: `src/nxt_openssl.c:983-988`
- Class: OOB read
- Trigger: Cert with wildcard `*` at position 0 and no separator.
- Suggested fix: Add `i < str.length` guard before adjusting `str.start`.
- PR: PR-D

### [Informational] OCSP stapling commit `5d35b44f` (separate branch)
The static OCSP stapling change cleans up `OCSP_RESPONSE`/`OCSP_BASICRESP` correctly on all error paths and validates response status/timestamps before use. Re-audit recommended after the dynamic-refresh story is added (timer-driven reload, not yet present).
- PR: _doc-only (Informational)_

---

## V4 — Control API & config validation

**Scope examined:** `nxt_controller.c`, `nxt_conf.c`, `nxt_conf.h`, `nxt_conf_validation.c`.

### [High] Unbounded JSON recursion
- File: `src/nxt_conf.c:1387-1399,1497-1609,1724-1802`
- Class: DoS
- Trigger: `PUT /config` with deeply nested JSON.
- Impact: A — stack exhaustion in controller.
- Suggested fix: Thread a depth counter through `nxt_conf_json_parse_value/object/array` and cap at e.g. 100.
- PR: PR-H

### [High] Unbounded JSON array/object element count
- File: `src/nxt_conf.c:1548,1764,1805`
- Class: DoS
- Trigger: Large flat JSON array or object.
- Impact: A — memory exhaustion.
- Suggested fix: Cap element count and abort with a parse error.
- PR: PR-H

### [High] Control socket has no peer-cred check
- File: `src/nxt_controller.c:720-750,456-463`
- Class: logic
- Trigger: Any local user with write access to `control.unit.sock` can mutate config.
- Impact: I — privileged config change by non-root.
- Suggested fix: `getsockopt(SO_PEERCRED)` and require UID match (or 0); rely on filesystem perms only as defense-in-depth, not the boundary.
- PR: PR-C

### [Medium] Validator allows arbitrary executable / isolation = false
- File: `src/nxt_conf_validation.c:1330-1372`
- Class: logic
- Trigger: Operator configures arbitrary `executable` paths and disables isolation.
- Impact: I — privilege boundary widened by config.
- Suggested fix: Document the implicit trust model; consider a deploy-time policy hook to reject executables outside an allow list.
- PR: PR-H

---

## V5 — Port IPC / shared memory / RPC

**Scope examined:** `nxt_port.c`, `nxt_port.h`, `nxt_port_socket.c`, `nxt_port_memory.c`, `nxt_port_memory.h`, `nxt_port_memory_int.h`, `nxt_port_rpc.c`.

### [High] Untrusted `chunk_id` used without bounds check
- File: `src/nxt_port_memory.c:698`
- Class: OOB read/write
- Trigger: Peer sends `mmap_msg` with `chunk_id >= PORT_MMAP_CHUNK_COUNT`; `nxt_port_mmap_chunk_start()` computes `hdr + HEADER_SIZE + chunk_id * CHUNK_SIZE`.
- Suggested fix: Check `chunk_id < PORT_MMAP_CHUNK_COUNT` in `nxt_port_mmap_get_incoming_buf()` before the call.
- PR: PR-B

### [High] `chunk_id + nchunks` arithmetic past mapped region
- File: `src/nxt_port_memory.c:701`
- Class: OOB write
- Trigger: Peer sends `nchunks` such that `chunk_id + nchunks > PORT_MMAP_CHUNK_COUNT`.
- Suggested fix: Validate the sum before computing `b->mem.end`.
- PR: PR-B

### [Medium] RPC stream-id wrap
- File: `src/nxt_port_rpc.c:131`
- Class: logic
- Trigger: After 2³² requests the global counter wraps; collisions if stale registrations linger.
- Suggested fix: Skip ID 0 on wrap; ideally per-port stream namespaces with cleanup guarantees.
- PR: PR-B

### [Medium] No sender-type ACL on port handler dispatch
- File: `src/nxt_port.c:177-191`, `src/nxt_port_socket.c:758-778`
- Class: logic / privilege boundary
- Trigger: A compromised worker could send messages of types only main process should send (cert/script/socket).
- Suggested fix: Validate `(sender_type, msg_type)` pairs in `nxt_port_handler()`.
- PR: PR-C

### [Medium] TOCTOU on shmem `mmap_id` lookup
- File: `src/nxt_port_memory.c:676-678`
- Class: TOCTOU
- Trigger: Mutex released between lookup and use of `mmap_handler->hdr`.
- Suggested fix: Bump refcount under the mutex; release after use.
- PR: PR-B

### [Low] PID reuse confusion across mmap incarnations
- File: `src/nxt_port_memory.c:235-245`
- Class: logic
- Trigger: PID wrap + same `src_pid`/`dst_pid`.
- Suggested fix: Add a generation counter (or fd-inode/timestamp check).
- PR: PR-B

---

## V6 — Process isolation

**Scope examined:** `nxt_isolation.c`, `nxt_clone.c`, `nxt_cgroup.c`, `nxt_fs_mount.c`, `nxt_capability.c`, `nxt_credential.c`, plus `nxt_process.c`, `nxt_application.c`.

### [Critical] Cgroup write races child unshare
- File: `src/nxt_process.c:619`, `src/nxt_cgroup.c:102`
- Class: TOCTOU / privilege boundary
- Trigger: With `CLONE_NEWCGROUP` + relative cgroup path, parent calls `nxt_cgroup_proc_add()` after fork but before the child has unshared into the new cgroup namespace; parent reads `/proc/self/cgroup` (its own state) and writes a path derived from it.
- Impact: A — child may end up in the wrong cgroup, escaping intended limits.
- Suggested fix: Move `nxt_cgroup_proc_add()` after the child confirms unshare via `PROCESS_CREATED`, or read `/proc/<child_pid>/cgroup` instead of `/proc/self/cgroup`.
- PR: PR-C

### [High] Mount-destination symlink TOCTOU
- File: `src/nxt_fs.c:13-44`, `src/nxt_isolation.c:783,789`
- Class: TOCTOU
- Trigger: `nxt_fs_mkdir_p()` creates mount destinations under a (possibly-attacker-influenced) rootfs; a component could be replaced with a symlink between mkdir and `mount(2)`. Code does not use `openat2(RESOLVE_BENEATH)`.
- Suggested fix: Validate the resolved path stays under the rootfs (`openat2(RESOLVE_BENEATH)`) or perform mkdir+mount atomically inside the mount namespace.
- PR: PR-C

### [Informational] `capset()` is never called
The capability module defines structures but doesn't drop capabilities programmatically. Acceptable today because `setuid` + `PR_SET_NO_NEW_PRIVS` are the primary barriers, but worth documenting — operators expecting "isolation drops caps" will be surprised.
- PR: PR-I (docs)

---

## V7 — PHP SAPI

**Scope examined:** `nxt_php_sapi.c` (and request hand-off in `nxt_application.c`).

### [High] `isspace()` skip past header buffer end
- File: `src/nxt_php_sapi.c:1794-1801`
- Class: OOB read
- Trigger: PHP `header("X: ")` (trailing space, no value).
- Impact: I — heap memory disclosed via response header.
- Suggested fix: Bound the loop with `value < h->header + h->header_len`.
- PR: PR-G

### [Medium] `realpath` failure leaks `tmp` buffer
- File: `src/nxt_php_sapi.c:883-886`
- Class: leak
- Trigger: Static-app config with non-existent/unreadable `script`.
- Suggested fix: `nxt_free(tmp)` before returning `NXT_ERROR` (mirror line 953).
- PR: PR-G

### [Medium] TrueAsync entry skips `php_request_shutdown`
- File: `src/nxt_php_sapi.c:2350-2392`
- Class: logic
- Trigger: TrueAsync mode preserves an EG callback zval across fork; child inherits stale executor state.
- Impact: A/I — first request in child can act on stale exception or symbol-table state.
- Suggested fix: Either run `php_request_shutdown()` then re-init in child, or scrub `EG(exception)`/symbol tables explicitly post-fork.
- PR: PR-H

### [Low] `PATH_INFO` not NUL-terminated
- File: `src/nxt_php_sapi.c:1467-1473`
- Class: logic
- Trigger: Length-based today; a future C-string consumer would walk past the buffer.
- Suggested fix: Document length-only contract or NUL-terminate.
- PR: PR-G

---

## V8 — Python / Ruby / Perl SAPIs

**Scope examined:** `src/python/nxt_python*.c`, `src/ruby/nxt_ruby*.c`, `src/perl/nxt_perl_psgi*.c`.

### [High] WSGI environ left NULL after copy failure
- File: `src/python/nxt_python_wsgi.c:455-457`
- Class: logic
- Trigger: `nxt_python_copy_environ(NULL)` returns NULL; next request dereferences NULL.
- Suggested fix: Check return; mark request as `NXT_UNIT_ERROR` if copy failed.
- PR: PR-G

### [Medium] ASGI WS `pending_payload_len` overflow
- File: `src/python/nxt_python_asgi_websocket.c:712,753`
- Class: integer overflow / DoS
- Trigger: Many fragmented frames whose lengths sum past `UINT64_MAX`.
- Impact: A — bypasses `max_buffer_size` cap.
- Suggested fix: `if (pending > UINT64_MAX - frame_len) error;` before accumulation.
- PR: PR-A

### [Medium] Ruby per-request `io_input`/`io_error` not reset
- File: `src/ruby/nxt_ruby_stream_io.c`
- Class: logic
- Trigger: App caches stream object across requests.
- Impact: I — body data from prior request visible.
- Suggested fix: Recreate or rewind IO objects each request.
- PR: PR-H

### [Medium] Perl `ERRSV` not cleared on init failure
- File: `src/perl/nxt_perl_psgi.c:543-546,555-558`
- Class: leak
- Trigger: `eval_pv()` or `io_init` fails; ERRSV propagates.
- Suggested fix: `sv_setsv(ERRSV, &PL_sv_undef)` in fail label.
- PR: PR-G

### [Low] ASGI lifespan checks wrong NULL
- File: `src/python/nxt_python_asgi_lifespan.c:162,168`
- Class: logic
- Trigger: After `send = PyObject_GetAttrString()`, the code checks `receive` (line 162) and `send` (line 168) instead of `send` and `done`.
- Suggested fix: Use the matching variables.
- PR: PR-G

---

## V9 — Java / Node / WASM / Go

**Scope examined:** `src/java/nxt_jni*.c`, `src/nodejs/unit-http/*.js`, `src/nodejs/unit-http/unit.cpp`, `src/wasm/nxt_wasm.c`, `src/wasm/nxt_rt_wasmtime.c`, `src/go/request.go`.

### [High] Java `InputStream.readLine(byte[], off, len)` lacks bounds check
- File: `src/java/nxt_jni_InputStream.c:89-112`
- Class: OOB write
- Trigger: App calls with malicious `off`/`len`.
- Impact: I/A — heap corruption from app-controllable arguments.
- Suggested fix: `GetArrayLength(out)` and validate `off + res <= len` before write.
- PR: PR-G

### [Medium] WASM offset arithmetic on guest-controlled fields
- File: `src/wasm/nxt_wasm.c:104-156`
- Class: OOB write
- Trigger: Many/large request headers.
- Suggested fix: Bound each `offset += strlen(s) + 1` against `NXT_WASM_MEM_SIZE`.
- PR: PR-G

### [Medium] WASM `send_headers(offset)` follows untrusted offset
- File: `src/wasm/nxt_wasm.c:43-70`
- Class: OOB read/write
- Trigger: Malicious WASM module supplies large `offset`.
- Suggested fix: Validate `offset < NXT_WASM_MEM_SIZE` before pointer cast.
- PR: PR-G

### [Medium] Java `sendWsFrame(buf, pos, len)` no capacity check
- File: `src/java/nxt_jni_Request.c:731-769`
- Class: OOB read
- Trigger: App passes `pos + len > capacity`.
- Impact: I — leaks heap into outgoing frame.
- Suggested fix: Validate against `GetDirectBufferCapacity()`.
- PR: PR-A

CVE-2025-1695 fix verified in place.

---

## V10 — libunit ABI

**Scope examined:** `nxt_unit.c`, `nxt_unit.h`, `nxt_unit_request.h`, `nxt_unit_response.h`, `nxt_unit_field.h`, `nxt_unit_websocket.h`, `nxt_unit_sptr.h`, `nxt_websocket.h`, `nxt_websocket.c`.

### [High] WebSocket header `hsize` OOB read
- File: `src/nxt_unit.c:1637-1694`
- Class: OOB read
- Trigger: 2-byte WS frame whose header indicates 14-byte extended length.
- Impact: I — discloses memory at `start + hsize - 4`.
- Suggested fix: Verify `recv_msg->size >= hsize` before reading mask field.
- PR: PR-A

### [Medium] WS `buf.free` advanced past `buf.end`
- File: `src/nxt_unit.c:1694-1697`
- Class: logic
- Trigger: Same as above; even when no immediate read, invariant break sets up later writes.
- Suggested fix: Validate `hsize <= buf.end - buf.start` before advance.
- PR: PR-A

### [Medium] `max_fields_count * sizeof(field) + 2` overflow on response
- File: `src/nxt_unit.c:2049-2051,2128-2130`
- Class: integer overflow
- Trigger: App requests very many response fields; multiply wraps and undersized buffer is allocated.
- Suggested fix: Pre-check `max_fields_count <= UINT32_MAX / sizeof(field)` and cap total against shmem limit.
- PR: PR-B

### [Medium] `nxt_unit_websocket_retain()` `hsize` unchecked
- File: `src/nxt_unit.c:3455-3466`
- Class: OOB read
- Suggested fix: Validate `hsize <= size` before `b + hsize - 4`.
- PR: PR-A

### [Low] `sptr` offset dereferenced without bounds
- File: `src/nxt_unit.c:1329,1354-1356`
- Class: logic
- Trigger: Malformed offset in request fields.
- Suggested fix: Validate every sptr against the receiving buffer's bounds at request-arrival time.
- PR: PR-B

---

## V11 — Static files / sendfile / path resolution

**Scope examined:** `nxt_http_static.c`, `nxt_event_conn_job_sendfile.c`, `nxt_linux_sendfile.c`, `nxt_freebsd_sendfile.c`, `nxt_macosx_sendfile.c`, `nxt_solaris_sendfilev.c`, `nxt_http_compression.c`.

### [High] FD leak on compression mmap failure
- File: `src/nxt_http_compression.c:280-284`
- Class: leak
- Trigger: Static response with compression enabled; `mmap()` of source file fails after temp file is created.
- Impact: A — FD exhaustion under sustained failure.
- Suggested fix: Close `*f` (and reset to `-1`) on every mmap-fail return path before `NXT_ERROR`.
- PR: PR-E

Otherwise the static handler is sound: `chroot_match()` + `openat2(RESOLVE_IN_ROOT)` cover the path-traversal/symlink space; index/range arithmetic is bounds-checked.

---

## V12 — WebSocket framing

**Scope examined:** `nxt_websocket.c`, `nxt_websocket.h`, `nxt_websocket_header.h`, `nxt_websocket_accept.c`, `nxt_http_websocket.c`, `nxt_h1proto_websocket.c`.

### [High] 64-bit payload length MSB not validated
- File: `src/nxt_websocket.c:80-96`
- Class: logic / protocol
- Trigger: Client sends `payload_len = 127` with the high bit of the 64-bit length set.
- Impact: A — RFC 6455 §5.2 violation accepted; max-frame-size policy must catch this implicitly.
- Suggested fix: Reject if `(p & 0x8000000000000000ULL) != 0`.
- PR: PR-A

### [High] Frame-size decrement is a no-op
- File: `src/nxt_http_websocket.c:87`
- Class: logic
- Trigger: Outer copy loop subtracts `copy_size` after the inner loop has zeroed it; frame_size never decreases. Data beyond the declared payload can be copied into the outgoing buffer.
- Impact: I — cross-frame data leak.
- Suggested fix: Save the initial `copy_size` before the inner loop and subtract that, or decrement `frame_size` inside the inner loop alongside the chunk copy.
- PR: PR-A

### [Medium] RSV bits not validated
- File: `src/nxt_h1proto_websocket.c:216-320`
- Class: logic
- Trigger: Client sets RSV1/2/3 without negotiated extension.
- Suggested fix: Reject the frame as protocol error after mask validation.
- PR: PR-A

### [Medium] No ping/pong rate limiting
- File: `src/nxt_h1proto_websocket.c:411-412`
- Class: DoS
- Suggested fix: Per-connection ping rate limit; drop the connection above threshold.
- PR: _excluded (DoS policy)_

---

## V13 — Memory pool / buffers / strings

**Scope examined:** `nxt_mp.c`, `nxt_mp.h`, `nxt_buf.c`, `nxt_buf.h`, `nxt_buf_pool.c`, `nxt_array.c`, `nxt_string.c`.

### [Medium] Off-by-one in `nxt_is_complex_uri_encoded`
- File: `src/nxt_string.c:718`
- Class: OOB read
- Trigger: Malformed URI ending in `%` followed by < 2 hex digits.
- Impact: I/A — read at `end`.
- Suggested fix: `if (end - src < 3)` to account for the two pre-increment reads.
- PR: PR-F

### [Low] `nxt_rmemstrn` length underflow
- File: `src/nxt_string.c:319`
- Class: logic
- Trigger: Caller passes `length > end - s`; `s1 = end - length` underflows.
- Suggested fix: Defensive guard, or document caller contract.
- PR: PR-F

No retain/release-asymmetry analogues to PR #56 found in this scope.

---

## V14 — FD / socket lifetime

**Scope examined:** `nxt_file.c`, `nxt_file.h`, `nxt_file_name.c`, `nxt_socket.c`, `nxt_socket.h`, `nxt_socketpair.c`, `nxt_listen_socket.c`, `nxt_conn_accept.c`, `nxt_external.c`, `nxt_epoll_engine.c`.

### [High] Accepted sockets missing `CLOEXEC`
- File: `src/nxt_conn_accept.c:161`, `src/nxt_epoll_engine.c:1028`
- Class: logic / privilege boundary
- Trigger: Plain `accept()` path; `accept4()` called with `SOCK_NONBLOCK` only.
- Impact: I — accepted client fd inherited by app processes spawned later.
- Suggested fix: Use `SOCK_CLOEXEC` in `accept4()`; `fcntl(F_SETFD, FD_CLOEXEC)` in the plain path.
- PR: PR-E

### [High] Pipe FD leak on `nxt_fd_nonblocking()` failure
- File: `src/nxt_file.c:737-745`
- Class: leak
- Trigger: First or second `nxt_fd_nonblocking()` call fails; the other end of the pipe is never closed.
- Suggested fix: Close both ends on error before returning.
- PR: PR-E

### [Medium] Pipe FDs lack `CLOEXEC`
- File: `src/nxt_file.c:728`
- Class: logic
- Suggested fix: Use `pipe2(.., O_CLOEXEC)` where available; `fcntl` fallback otherwise.
- PR: PR-E

### [Medium] `nxt_file_redirect` may leak on `close()` failure
- File: `src/nxt_file.c:638-642`
- Class: logic
- Suggested fix: Caller-side fallback close; documented contract.
- PR: PR-E

### [Medium] `accept4()` capability test masks non-ENOSYS errors
- File: `src/nxt_epoll_engine.c:297`
- Class: logic
- Suggested fix: Distinguish `errno == ENOSYS` from other errors when deciding the fallback path.
- PR: PR-E

### [Low] `socketpair` `SO_PASSCRED` failure handling
- File: `src/nxt_socketpair.c:33-47,52-64`
- Class: logic
- Suggested fix: Log and retain consistent credential state.
- PR: PR-E

---

## Appendix — known/already-fixed (audit acknowledgement)

These are *not* findings; they are listed so a reader can confirm they were considered:

- **PR #56** (`fix(port): plug mp-pool retain and fd/buffer leaks in IPC reply paths`) — `nxt_cert_store_get`, `nxt_script_store_get`, `nxt_main_port_socket_handler`, `nxt_main_port_access_log_handler`. Retain-after-write fix and fd cleanup on `nxt_port_socket_write()` failure.
- **1.35.4** — CLOSE-WAIT cleanup, idle-connection-queue iteration, systemd file-descriptor handling.
- **CVE-2025-1695** — Java WebSocket payload length validation (fixed in 1.35.0). Verified the fix is still in place; analogous C-side hazards are reported under V10/V12 above.
- **OCSP stapling** — commit `5d35b44f` (on `claude/tls-modernization-unit-NQehK`, not yet on master). Static stapling implementation reviewed; cleanup looks correct, but a dynamic-refresh story is still missing.

## Recommended next actions

Remediation is sliced into PR-A through PR-I per the **Remediation tracker** above. Each finding's `- PR:` bullet flips from `open` to a merged-PR reference (`andypost/unit#NN`, `freeunitorg/freeunit#MM`) as PRs land, so this file doubles as the remediation log. Per-PR scope and ordering:

- **PR-A** — WebSocket framing safety (libunit + protocol). CVE-track. ~250 lines.
- **PR-B** — shared-memory offset bounds hardening (`nxt_port_memory.c`, `nxt_unit.c`). CVE-track. ~200 lines.
- **PR-C** — privilege-boundary tightening (V6 cgroup TOCTOU + mount symlink, V5 sender-type ACL, V4 control-socket peer-cred). Carries the only Critical finding. ~300 lines.
- **PR-D** — TLS / cert hygiene (`nxt_openssl.c`, `nxt_cert.c`). ~90 lines.
- **PR-E** — FD / CLOEXEC lifetime (V14 + V11). ~120 lines.
- **PR-F** — HTTP/URI parser & string bounds (V1, V13). ~70 lines.
- **PR-G** — language-binding bounds (PHP / Python / Perl / Java / WASM). ~180 lines.
- **PR-H** — controller robustness (V4 JSON limits, V7 TrueAsync, V8 Ruby IO). ~150 lines.
- **PR-I** — routing matcher polish + isolation docs. Low priority. ~60 lines.

Submission order: **first wave** PR-C → PR-A → PR-B → PR-D (Critical/High, two CVE-track); **second wave** PR-E → PR-F → PR-G; **third wave** PR-H → PR-I. Themes worth highlighting outside the tracker:

- **Cross-cutting helper:** PR-B introduces a single shmem-offset bounds helper used at every `nxt_port_memory.c` and `nxt_unit.c` deserialization site (V5/V10 cluster).
- **CVE-track PRs:** PR-A (WebSocket framing safety) and PR-B (shmem bounds) are the two CVE candidates; coordinate disclosure with the maintainer before opening publicly.
- **Fuzz targets:** the JSON parser (V4) and the WebSocket assembler (V10/V12) are the strongest fuzz candidates and would have caught most of the High findings here.

Each finding's section header (e.g. `V12 — High — Frame-size decrement is a no-op`) is the canonical issue title to use when filing follow-ups.
