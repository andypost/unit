# FreeUnit TODO Inventory

Aggregated from a codebase sweep across `src/`, `auto/`, `test/`, `tools/`, `.github/`, `fuzzing/`, `docs/` — matching `TODO`, `FIXME`, `XXX`, `HACK`, `BUG` (word-bounded), plus `todo!()` / `unimplemented!()` in Rust.

**Last re-swept** against upstream master `7c9c5d96` (includes issue #28 CLOSE-WAIT fix, Go 1.26 / Node 24 docker variants, WASMTIME 43.0.1 bump). No new source-level TODOs introduced between `142560e0..7c9c5d96`; only line-number shifts in `nxt_h1proto.c`. `nxt_conn_accept.c` is clean after the #28 fix.

Format: `path:line — <classification> — <comment, summarized>`

Classifications: **BUG** (known defect), **PERF** (perf issue), **FEATURE** (missing functionality), **CLEANUP** (refactor/tech debt), **PORTABILITY** (OS/arch), **VERSION** (version-guard compat), **SECURITY**, **CI**, **UNKNOWN** (unclear intent).

Counts at a glance: ~60 source TODOs in core daemon, ~5 in PHP tests, 0 in Python source, ~10 across Java/Node, ~7 in WASM component, 1 big one in Ruby build. Core daemon and Java are the heaviest debt carriers.

---

## Core daemon

### Router / HTTP

- `src/nxt_router.c:748` — FEATURE — find-and-add missing for port waiters in port_hash
- `src/nxt_router.c:1396` — CLEANUP — new engines and threads initialization
- `src/nxt_router.c:4072` — CLEANUP — remove `engine->port` field
- `src/nxt_router.c:4109` — FEATURE — notify all apps when engine changes
- `src/nxt_router.c:4496` — FEATURE — cancel message and return if cancelled
- `src/nxt_router.c:5898` — **BUG** — `get_mmap_handler`: app == NULL reply-port handling incomplete
- `src/nxt_router.c:5914` — **BUG** — app response handling incomplete
- `src/nxt_h1proto.c:2298` — CLEANUP — queues should go via client proto interface
- `src/nxt_http_request.c:650` — FEATURE — need application flag to get local address (`SERVER_ADDR`)
- `src/nxt_http_request.c:701` — CLEANUP — `Server` / `Date` / `Content-Length` processing should move to filter
- `src/nxt_http_parse.c:505` — UNKNOWN — absolute path or `*` parsing incomplete
- `src/nxt_http_websocket.c:106` — CLEANUP — handle websocket RPC error

### Process / IPC

- `src/nxt_port.c:201` — CLEANUP — join with `process_ready`, move to `nxt_main_process.c`
- `src/nxt_port.c:270` — CLEANUP — check buffer size and simplify
- `src/nxt_port.c:308` — CLEANUP — move to `nxt_main_process.c`
- `src/nxt_port_socket.c:749` — PERF — disable event for some time on buffer alloc failure
- `src/nxt_port_socket.c:892` — PERF — disable event for some time on buffer alloc failure
- `src/nxt_port_socket.c:1345` — UNKNOWN — port error handler incomplete
- `src/nxt_main_process.c:841` — CLEANUP — fast exit optimization needed
- `src/nxt_main_process.c:855` — CLEANUP — graceful exit implementation needed
- `src/nxt_main_process.c:1137` — CLEANUP — check buffer size and simplify
- `src/nxt_port_memory.c:503` — FEATURE — introduce `port_mmap` limit and release wait
- `src/nxt_port_memory.c:744` — CLEANUP — clear buffer / error path incomplete

### Event engine / I/O

- `src/nxt_conn.c:180` — CLEANUP — adjust non-freeable block end in conn mem_pool
- `src/nxt_conn_write.c:176` — **BUG** — temporary fix for issue #1125 (HTTP sendfile)
- `src/nxt_event_engine.c:459` — CLEANUP — free timers on engine shutdown
- `src/nxt_kqueue_engine.c:439` — UNKNOWN — pending event handling in kqueue `close_file`
- `src/nxt_listen_socket.c:75` — UNKNOWN — why is `IPV6_V6ONLY` error ignored
- `src/nxt_listen_socket.c:84` — UNKNOWN — why is `SO_SNDBUF` error ignored (disabled code)

### Controller / Config

- `src/nxt_conf.h:131` — CLEANUP — reimplement and reorder functions

### libunit / App interface

- `src/nxt_unit.c:6015` — **BUG** — should be `alert` level after router graceful shutdown is implemented

### TLS

- `src/nxt_openssl.c:393` — CLEANUP — verify callback implementation needed
- `src/nxt_openssl.c:396` — CLEANUP — verify depth implementation needed
- `src/nxt_gnutls.c:98` — CLEANUP — `gnutls_global_deinit` missing
- `src/nxt_gnutls.c:155` — CLEANUP — mem_pool cleanup for credentials and priorities
- `src/nxt_cyassl.c:86` — CLEANUP — `CyaSSL_Cleanup()` missing
- `src/nxt_cyassl.c:159` — CLEANUP — CA certificate handling incomplete
- `src/nxt_polarssl.c:43` — CLEANUP — mem_pool allocation needed
- `src/nxt_polarssl.c:81` — CLEANUP — ciphers configuration missing
- `src/nxt_polarssl.c:83` — CLEANUP — CA certificate handling missing

### Misc core

- `src/nxt_lib.c:149` — CLEANUP — stop engines on shutdown
- `src/nxt_main.h:77` — CLEANUP — remove unused forward declarations
- `src/nxt_runtime.c:288` — CLEANUP — add logging for engine service lookup failure
- `src/nxt_spinlock.c:53` — PERF — spinlock count should be 10 on virtualized systems
- `src/nxt_work_queue.h:19` — FEATURE — exception_handler, prev/next task, subtasks support

---

## PHP module

### Source / Tests

- `test/php/async_slow/entrypoint.php:17` — FEATURE — Replace `\Async\sleep()` with correct TrueAsync API once stable
- `test/php/async_mirror/entrypoint.php:10` — FEATURE — Adjust `\Unit\Request` API surface once `nxt_php_extension.c` is implemented
- `test/php/async_shutdown/entrypoint.php:13` — FEATURE — Replace `\Unit\Server::setHandler()` with actual API once `nxt_php_extension.c` is implemented
- `test/test_php_trueasync.py:13` — FEATURE — TDD tests written; items #1, #2 in `TODO.md` pending
- `test/test_php_trueasync.py:94` — FEATURE — `async`/`entrypoint` fields missing from `nxt_php_app_conf_t` (TODO.md #1)
- `test/test_php_trueasync.py:136` — FEATURE — PHP async config fields missing from `nxt_php_app_conf_t` (TODO.md #1)
- `test/test_php_trueasync.py:480` — FEATURE — TrueAsync scheduler may cancel vs complete coroutines (TODO.md #4)
- `test/test_php_trueasync.py:547` — FEATURE — `ZEND_ASYNC_SHUTDOWN()` may cancel in-flight coroutines (TODO.md #4)

### Version-guard hotspots (PHP)

Removable once minimum PHP version is bumped to the listed threshold:

- `src/nxt_php_sapi.c:77` — `< 70200` — `zif_handler` typedef shim
- `src/nxt_php_sapi.c:106` — `< 80500` — `nxt_php_disable()` unnecessary on 8.5+
- `src/nxt_php_sapi.c:129` — `< 70400` — `nxt_zend_stream_init_fp()` wrapper
- `src/nxt_php_sapi.c:173` — `< 70200` — `ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX` signature change
- `src/nxt_php_sapi.c:401` — `< 70400 && ZTS` — `tsrm_ls` static TSRM variable
- `src/nxt_php_sapi.c:1125` — `< 80500` — `disable_functions` handling
- `src/nxt_php_sapi.c:1234` — `< 80500` — `disable_classes` handling
- `src/nxt_php_sapi.c:1553` — `< 70400` — `nxt_zend_stream_init_fp()` call
- `src/nxt_php_sapi.c:1572,1663,1676` — `< 50600` — `read_post` SAPI field save/restore dance
- `src/nxt_php_sapi.c:1695` — `< 80200` — `php_module_startup()` signature
- `auto/modules/php:152` — `< 80200` — `php_module_startup()` pre-PHP-8.2 arg

**Quick win:** dropping support for PHP < 7.4 (already EOL) eliminates ~9 of these branches.

---

## Python module

No `TODO/FIXME/XXX/HACK/BUG` comments found in `src/python/**`, its build script, or tests. The Python module is remarkably clean.

### Version-guard hotspots (Python)

- `src/python/nxt_python.c:74` — `≥ 3.8` — `PyConfig` API
- `src/python/nxt_python.c:129` — Py3 fallback for init config
- `src/python/nxt_python.c:235` — `< 3.7` — `PyEval_InitThreads()` (removed in 3.7+)
- `src/python/nxt_python_asgi.c:247` — `< 3.7` — `get_event_loop` vs `get_running_loop`
- `src/python/nxt_python_asgi.c:289` — `< 3.7` — asyncio event loop fallback
- `src/python/nxt_python_asgi_lifespan.c:59` — `≥ 3.7` — `PyMemberDef` initializer syntax
- `src/python/nxt_python_wsgi.c:824` — Py3 — `PyUnicode` vs `PyString`
- `src/python/nxt_python.h:17` — Py3 only
- `src/python/nxt_python.h:36` — `≥ 3.5` — ASGI gate

**Quick win:** dropping Python 3.6 support (EOL 2021) removes most of these guards.

### ASGI/WSGI protocol gaps

- `src/python/nxt_python_asgi.c:1571` — build-stub returns `"ASGI not implemented"` when `NXT_HAVE_ASGI` is undefined.

---

## Ruby module

### Build

- `auto/modules/ruby:75–79` — **PORTABILITY / BUG** — Debian/Ubuntu multiarch: `RbConfig["libdir"]` returns `/usr/lib` but the actual `libruby-X.Y.so` lives in `/usr/lib/<triplet>/`. The two-pass check misses it. Proper fix: probe `dpkg-architecture -q DEB_HOST_MULTIARCH`. *(Also referenced from `.github/workflows/clang-ast.yaml:40`.)*

Source files are free of TODO markers.

---

## Node.js module

- `src/nodejs/unit-http/websocket_request.js:251` — FEATURE — handle extensions
- `src/nodejs/unit-http/websocket_request.js:417` — FEATURE — handle negotiated extensions
- `src/nodejs/unit-http/unit.cpp:964` — UNKNOWN — will work only for utf8 content-type

---

## Java module

- `src/java/nxt_jni_Request.c:374` — UNKNOWN — throw `NumberFormatException.forInputString(value)`
- `src/java/nginx/unit/Request.java:402` — UNKNOWN — bare `TODO`
- `src/java/nginx/unit/Response.java:703` — UNKNOWN — `TODO throw`
- `src/java/nginx/unit/Response.java:712` — UNKNOWN — `TODO throw`
- `src/java/nginx/unit/Context.java:2221` — UNKNOWN — process other cases, throw `IllegalArgumentException`
- `src/java/nginx/unit/Context.java:2307` — UNKNOWN — process other cases, throw `IllegalArgumentException`
- `src/java/nginx/unit/websocket/WsRemoteEndpointImplBase.java:1184` — **BUG** — code should never be called
- `src/java/nginx/unit/websocket/pojo/PojoMessageHandlerBase.java:53` — FEATURE — method should already be accessible here
- `src/java/nginx/unit/websocket/WsFrameBase.java:972` — PERF — masking should move to this method

---

## WebAssembly (wasm-wasi-component)

- `src/wasm-wasi-component/src/lib.rs:65` — UNKNOWN — should this get used?
- `src/wasm-wasi-component/src/lib.rs:382` — FEATURE — convert body into a Stream to become async
- `src/wasm-wasi-component/src/lib.rs:389` — FEATURE — can this perform a partial read?
- `src/wasm-wasi-component/src/lib.rs:390` — FEATURE — how to make this async at the nxt level?
- `src/wasm-wasi-component/src/lib.rs:439` — UNKNOWN — what to do with trailers?
- `src/wasm-wasi-component/src/lib.rs:450` — UNKNOWN — is this actually safe?
- `src/wasm-wasi-component/src/lib.rs:523` — UNKNOWN — handle failure when `amt` is negative

---

## Tools (unitctl — Rust)

- `tools/unitctl/unitctl/src/cmd/instances.rs:114` — UNKNOWN — abstract socket case ruled out previously
- `tools/unitctl/unit-openapi/openapi-templates/request.rs:55` — FEATURE — multiple body params possible technically, not supported

---

## CI / Build

- `.github/workflows/clang-ast.yaml:40` — PORTABILITY — reminder to fix `auto/modules/ruby` multiarch libdir probe

---

## Empty zones

These areas have **zero TODO markers** — either mature, or (more likely) under-annotated:

- Perl module (`src/perl/`)
- Go module (`go/`, `src/nxt_go*`)
- OpenTelemetry (`src/otel/`, `src/nxt_otel*`)
- Test infrastructure (`test/conftest.py`, `test/unit/**`)
- Packaging (`pkg/**`)
- Docs (`docs/**`)
- Fuzzing (`fuzzing/**`)

Absence of TODOs ≠ absence of debt; Perl/Go/OTel modules deserve a separate audit pass.

---

## Patterns worth acting on as groups

### Pattern A — TLS backends rotting
`nxt_gnutls.c`, `nxt_cyassl.c`, `nxt_polarssl.c` each have multiple `CLEANUP` TODOs for missing deinit, credential cleanup, CA handling. Reality: OpenSSL is the only backend anyone uses. Two options:

- **Option 1 (recommended):** delete the alternative TLS backends entirely — reduces surface area, kills 9 TODOs in one PR.
- **Option 2:** mark them `EXPERIMENTAL` in docs and refuse to build by default.

### Pattern B — "move to filter" deferrals
`nxt_http_request.c:701` and `nxt_h1proto.c:2294` both defer work to an unimplemented filter layer. This is a ghost of an abandoned refactor. Decide: finish the filter or remove the TODOs.

### Pattern C — Version-guard debt
PHP has ~12 version guards; Python has ~9. Both modules support officially-EOL language versions (PHP < 7.4 from 2019, Python 3.6 from 2021). A single "raise minimum supported version" PR per language would remove 15+ branches and simplify ongoing maintenance materially.

### Pattern D — Graceful shutdown
`nxt_lib.c:149`, `nxt_main_process.c:841,855`, `nxt_event_engine.c:459`, `nxt_unit.c:6015` all reference an unimplemented graceful-shutdown path. This blocks `unit-roadmap.md` X3 (graceful reload) from being done correctly. Landing router graceful shutdown first removes 5 TODOs and unblocks the reload work.

### Pattern E — Java WebSocket TODOs
Cluster of UNKNOWN/FEATURE/PERF in `src/java/**/websocket/` suggests the Java WebSocket implementation was ported from an external source (Tomcat-flavored WsRemoteEndpointImplBase names are telling) and not fully adapted. Needs an owner review.

### Pattern F — WASM component async gaps
`src/wasm-wasi-component/src/lib.rs` has 3 TODOs explicitly asking "how to make this async at the nxt level?" — all stem from the libunit body-streaming API being sync. Fixing this is effectively `unit-roadmap.md` D3 (body streaming) for the WASM component.

---

## Integration with `unit-roadmap.md`

| Pattern | Roadmap item |
|---|---|
| A. TLS backend cleanup | D4 (TLS modernization) |
| B. HTTP filter chain | D3 (body streaming) + D2 (HTTP/2 requires filter design) |
| C. Version-guard debt | G1 (support matrix publishing forces a decision) |
| D. Graceful shutdown | X3 (graceful reload) — **prerequisite** |
| E. Java WebSocket | Not in roadmap; needs owner — file as separate tracking issue |
| F. WASM async | D3 (body streaming) — rev the libunit body API once, both benefit |

**First three merges to drain this list fast:** (1) drop EOL PHP/Python minors, (2) remove dead TLS backends, (3) land graceful shutdown in core. Each is self-contained and each deletes debt in multiple places.
