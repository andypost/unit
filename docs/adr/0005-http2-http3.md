# 0005. HTTP/2 and HTTP/3 in the router by reusing a protocol engine

- Status: proposed
- Date: 2026-09-23
- Stream: h2/h3 exploration (design only; no `src/` change on this branch)
- Code base: every `file:line` below refers to commit `b3bec257` (the
  integration tip of 2026-09-23). The IPC, router, status and schedules
  streams are editing `src/nxt_router.c` this week, so re-check each line
  before editing.
- Spikes: an nghttp2 server and a quiche HTTP/3 server, both driven from
  a bare epoll loop (see §8); the code is in the history of
  `stream/h2h3-explore`, commit 48cc253d.

## Context and problem statement

FreeUnit speaks HTTP/1.x only. `nxt_http_protocol_t` has had an
`NXT_HTTP_PROTO_H2` slot since 2019 (`src/nxt_http.h:68-72`), and the
`nxt_http_proto[]` table leaves it empty (`src/nxt_h1proto.c:155`). The
"schedules" work (ADR 0004) filled the sibling `NXT_HTTP_PROTO_DEVNULL` slot
with a protocol that has no connection at all (`src/nxt_http_devnull.c`),
which proved that the request path above the protocol layer is already
protocol-neutral: the router, the routes, static `share`, compression, the
access log, OTel and the worker IPC all operate on `nxt_http_request_t` and
never on the connection.

We want HTTP/2 (RFC 9113) and HTTP/3 (RFC 9114, over QUIC, RFC 9000) with
these constraints:

1. h2 and h3 **streams terminate in the router** and become the same
   `nxt_http_request_t` the h1 path produces; `nxt_router_prepare_msg()`
   (`src/nxt_router.c:7627`) then builds the `nxt_unit_request_t` the
   workers read. Workers, libunit and the port protocol do not change; PHP
   keeps seeing `SERVER_PROTOCOL` (`src/nxt_php_sapi.c:1553`), WSGI keeps
   its `SERVER_PROTOCOL` (`src/python/nxt_python_wsgi.c:661`), only its
   value becomes `HTTP/2.0` or `HTTP/3.0`.
2. The router machinery is reused as-is: routes and `match`
   (`src/nxt_http_route.c`), static `share` with `fallback`
   (`src/nxt_http_static.c`), compression (`src/nxt_http_compression.c`),
   the access log (`src/nxt_router_access_log.c`), OTel spans
   (`NXT_OTEL_TRACE()` at `src/nxt_http_request.c:327,600`) and the new
   `schedules`.
3. We **reuse an existing protocol engine** rather than writing HPACK, QPACK
   or a QUIC transport ourselves, and drive it from FreeUnit's own event
   engine (one thread per engine, no library threads).

The question this ADR answers is which engines fit that model, what in the
protocol layer must be generalised first, and in which order to ship.

## Decision drivers

- **The h1 hot path must not slow down.** ADR 0004's rule stands: no NULL
  checks sprinkled into h1 code; the protocol dispatch stays a table lookup
  (`nxt_http_proto[r->protocol]`).
- **One engine thread, one event loop.** Every library we adopt must be
  fed from `nxt_conn_t` read/write handlers or from an `nxt_fd_event_t` on a
  UDP socket, with timers on `nxt_timer_t`. Libraries that own threads or
  an event loop of their own are out.
- **Same TLS configuration.** Certificates come from the cert store
  (`nxt_cert.c`), bundles and SNI from `nxt_openssl_servername()`
  (`src/nxt_openssl.c:1027-1110`), `conf_commands`, `session` cache and
  `tickets` all keep working for h2 and, where the transport allows, for h3.
- **Packaging reality.** The OpenSSL floor is 1.1.1 because of RHEL 8
  (`auto/ssltls:32-47`); the deb/rpm builds depend on the distro `libssl-dev`
  (`pkg/deb/debian/control.in:7`, `pkg/rpm/unit.spec.in:8-10`). h2 must not
  raise that floor; h3 may be an optional `--h3` build.
- **Reviewable in phases.** Each phase must be mergeable and testable on
  its own, with h2spec / curl / pytest evidence.

## The code today

### 1. The event engine

`nxt_event_engine_start()` (`src/nxt_event_engine.c:506-558`) is the loop:
drain the engine work queues, `nxt_timer_find()` for the nearest timer,
`engine->event.poll(engine, timeout)`, then `nxt_timer_expire()`. The
interface a facility implements is `nxt_event_interface_t`
(`src/nxt_event_engine.h:23-167`): `enable_read`/`enable_write`,
`block_read`/`block_write`, `oneshot_*`, `enable_accept`, `poll`, plus an I/O
vtable `nxt_conn_io_t` (`src/nxt_conn.h:39-87`: `read`, `recvbuf`, `recv`,
`write`, `sendbuf`, `writev`, `send`, `shutdown`). Linux uses epoll in
edge-triggered mode (`nxt_epoll_edge_engine`, `src/nxt_epoll_engine.c:121`;
`nxt_epoll_enable()` registers `EPOLLIN|EPOLLOUT|EPOLLET`, `:361-367`);
listeners are registered with `EPOLLEXCLUSIVE` (`:571-584`), so several
engine threads share one listening fd without thundering herd. kqueue
(`src/nxt_kqueue_engine.c`) mirrors this.

An `nxt_conn_t` (`src/nxt_conn.h:125-196`) *is* an `nxt_fd_event_t` (first
member) with read/write buffers, states (`nxt_conn_state_t`, `:15-27`,
including a `timer_value` callback used by `nxt_conn_timer()`), an `io`
vtable and `c->u.tls`. Writing is `nxt_conn_io_write()`
(`src/nxt_conn_write.c:15-130`): it calls `c->io->sendbuf()` in a loop of at
most 10 MB, then `nxt_sendbuf_update()`, and completes buffers through their
`completion_handler` (`nxt_sendbuf_drain()`, `src/nxt_sendbuf.c:397`). That
completion callback is the router's back-pressure: for static files
`nxt_http_static_buf_completion()` (`src/nxt_http_static.c:1795-1875`) reads
the next slice with `nxt_file_read()` only when the previous one is done.

Timers: `nxt_timer_t` (`src/nxt_timer.h:24-41`) lives in an rbtree per
engine, `nxt_timer_add()` (`src/nxt_timer.c:70`) coalesces timers within
`timer->bias` (default 50 ms, `NXT_TIMER_DEFAULT_BIAS`, `src/nxt_timer.h:12`,
set by `nxt_conn_timer_init`, `src/nxt_conn.h:199-204`). QUIC loss-detection
timers need 1 ms granularity, so h3 timers must use `bias = 0`.

### 2. The TLS layer

`--openssl` (`auto/options:116`) sets `NXT_OPENSSL=YES`; `auto/ssltls`
probes `-lssl -lcrypto`, enforces the 1.1.1 floor (`:48-67`) and detects
`SSL_CONF_cmd` and tlsext; `auto/sources:215-223` then compiles
`src/nxt_openssl.c` behind the generic `NXT_TLS`. The generic vtable is
`nxt_tls_lib_t` (`src/nxt_tls.h:35-44`: `library_init`, `server_init`,
`server_free`) and `nxt_tls_conf_t::conn_init` (`:70`).

`nxt_openssl_server_init()` (`src/nxt_openssl.c:216-370`) builds one
`SSL_CTX` per certificate bundle (`SSLv23_server_method()`, options,
`nxt_openssl_chain_file()`, ciphers, `conf_commands` via `SSL_CONF_cmd`,
session cache, ticket keys) and, for the last bundle, sets
`conf->conn_init = nxt_openssl_conn_init` and the SNI callback
`nxt_openssl_servername()` which swaps `SSL_set_SSL_CTX()` per bundle
(`:1096`). **There is no ALPN configuration anywhere** (grep for `alpn` in
`src/` is empty), which is why every client today falls back to h1 over TLS.

Per connection, `nxt_openssl_conn_init()` (`:1158-1212`) does `SSL_new`,
`SSL_set_fd(c->socket.fd)`, `SSL_set_accept_state`, installs
`c->io = &nxt_openssl_conn_io` and forces `c->sendfile = OFF` (`:1207`).
The handshake is driven by the engine: `nxt_openssl_conn_handshake()`
(`:1235-1320`) calls `SSL_do_handshake()`, and on `WANT_READ`/`WANT_WRITE`
`nxt_openssl_conn_test_error()` (`:1531-1600`) toggles the fd's read/write
interest and returns `NXT_AGAIN`. I/O is `SSL_read` in
`nxt_openssl_conn_io_recvbuf()` (`:1324-1352`) and `SSL_write` in
`nxt_openssl_conn_io_send()` (`:1370-1402`). This is exactly the shape a
callback-based h2 library needs: bytes in, bytes out, no ownership of the
socket.

The connection starts in `nxt_http_conn_init()` (`src/nxt_h1proto.c:229-259`):
with TLS configured it peeks 16 bytes (`nxt_http_idle_io_read_handler`,
`:280-322`) and `nxt_http_conn_test()` (`:326-395`) calls
`tls->conn_init()` on a ClientHello; the handshake's `ready_handler` is
`nxt_h1p_conn_proto_init()` (`:465-484`) via `nxt_h1p_idle_state`
(`:399-411`). That is the one place where ALPN can branch to h2.

### 3. The protocol abstraction

`nxt_http_proto_table_t` (`src/nxt_http.h:374-393`) has request-side hooks
`body_read`, `local_addr`, `header_send`, `send`, `body_bytes_sent`,
`discard`, `close`; upstream-side hooks `peer_connect`, `peer_header_send`,
`peer_header_read`, `peer_read`, `peer_close`; and `ws_frame_start`.
`r->proto` is a union `{ void *any; nxt_h1proto_t *h1; }`
(`src/nxt_http.h:90-93`). The table (`src/nxt_h1proto.c:136-167`) fills H1
completely, DEVNULL with the seven request-side hooks, and leaves H2 empty.

Call sites are all through the table and guarded by `r->proto.any != NULL`:
`src/nxt_http_request.c:673` (`local_addr`), `:682` (`body_read`), `:819`
(`header_send`), `:835` (`ws_frame_start`), `:1003` (`send`), `:1088`
(`discard`), `:1142` (`close`); `src/nxt_http_route.c:1665` (`local_addr`
for a `destination` match). `src/nxt_http_variables.c:445` calls
`body_bytes_sent` unguarded, so every protocol must implement it (DEVNULL
does). The proxy path uses `peer_*` through `peer->protocol`
(`src/nxt_http_proxy.c:177-465`), which stays h1: an h2 client request can
be proxied over an h1 upstream because the peer protocol is a separate
field.

**Direct `r->proto.h1` dereferences outside `nxt_h1proto*.c`** (the ones
that would break for a new protocol) are exactly two, both already fenced by
a protocol check: `src/nxt_http_variables.c:536` (`$response_header_connection`,
returns empty unless `r->protocol == NXT_HTTP_PROTO_H1`, `:530-534`) and
`:606` (`$response_header_transfer_encoding`). Everything else passes
`r->proto.any` opaquely as the error/close handler's `data` argument
(`src/nxt_http_error.c:80,117`, `src/nxt_http_static.c:1826,1843`,
`src/nxt_http_websocket.c:80,153`, `src/nxt_router.c:7520`,
`src/nxt_router_access_log.c:384`, `src/nxt_http_request.c:1071`).
`nxt_http_devnull.c:10-14` records the same survey.

What *is* h1-specific but not reachable through the table:

- The request field processing table `nxt_h1p_fields[]` and its hash
  `nxt_h1p_fields_hash` are `static` in `src/nxt_h1proto.c:171-196`. They
  mix h1 framing headers (`Connection`, `Upgrade`, `Sec-WebSocket-*`,
  `Transfer-Encoding`) with protocol-neutral ones (`Host`, `Cookie`,
  `Referer`, `User-Agent`, `Content-Type`, `Content-Length`,
  `Authorization`, and the OTel `Traceparent`/`Tracestate`). A new protocol
  must run `nxt_http_fields_process()` (`src/nxt_http_parse.c:1264`) with
  the neutral part of that table or OTel propagation, cookies and
  `Content-Length` silently stop working.
- `nxt_http_request_chunked_transform()` (`src/nxt_http_request.c:555-586`)
  synthesises `Content-Length` from `r->body->file_end` when `r->chunked`
  and dereferences `r->chunked_field->skip` unconditionally. h2/h3 requests
  without a `content-length` header are the h2 analogue of chunked, so this
  becomes `if (r->chunked_field != NULL)`.
- `nxt_h1p_request_body_read()` (`src/nxt_h1proto.c:906-1137`) mixes the
  neutral part (allocate `r->body` as a memory buffer up to
  `body_buffer_size`, else a `mkstemp` file in `body_temp_path`; enforce
  `max_body_size`, `:1055,1189`) with h1 framing (reads from `c->read`,
  chunk parsing). The allocation and the "ready when complete" contract
  (`r->state->ready_handler`) are what h2/h3 reuse.
- `nxt_h1p_request_header_send()` (`:1383-1595`) serialises `r->status` and
  `r->resp` into a status line and `Name: value\r\n` lines, decides
  chunked vs `Content-Length` vs close, and queues the body handler on the
  fast work queue *before* `nxt_conn_write()`. The generic part happens
  earlier in `nxt_http_request_header_send()` (`src/nxt_http_request.c:688-826`):
  `set_headers`, `Server`, `Date`, `Content-Length` synthesis, Vary
  merging (`:714`). h2/h3 only need to encode `r->resp` as `(name, value)`
  pairs.
- `nxt_h1p_request_send()` (`:1658-1690`) appends to `c->write` with chunk
  framing; `nxt_h1p_request_body_bytes_sent()` (`:1765`) is
  `c->sent - header_size`; `nxt_h1p_request_discard()` (`:1779`) drops
  `c->write` and drains `last`; `nxt_h1p_request_close()` (`:1905-1940`)
  releases the joint and goes to keep-alive (`nxt_h1p_keepalive`,
  `:2012-2060`, which re-zeroes the `nxt_h1proto_t` up to `conn`) or
  shutdown.
- The request's task and timers are the connection's (`:520-548`), and the
  joint reference `joint->count++` (`:530`) pairs with
  `nxt_router_conf_release()` in close (`:1915`). A per-stream request must
  do the same.

### 4. Static serving and sendfile

`share` never uses sendfile. `nxt_http_static_body_handler()`
(`src/nxt_http_static.c:1738-1790`) allocates up to `NXT_HTTP_STATIC_BUF_COUNT`
memory buffers of `NXT_HTTP_STATIC_BUF_SIZE`, and
`nxt_http_static_buf_completion()` (`:1795-1875`) fills each with
`nxt_file_read()` (pread) and calls `nxt_http_request_send()`; the file
descriptor buffer `fb` is only a cursor (`file_pos`/`file_end`, `:1003-1010`).
The only sendfile code, `nxt_conn_io_sendfile()` (`src/nxt_conn_write.c:193-260`),
runs for file buffers on plain connections, which the static path never
produces, and TLS disables it anyway (`c->sendfile = NXT_CONN_SENDFILE_OFF`,
`src/nxt_openssl.c:1207`). Compression of static responses writes a temp
file and serves that (`nxt_http_comp_compress_static_response`,
`src/nxt_http_compression.c:317`). **Consequence: h2 and h3 need no change
in `nxt_http_static.c`**; the pull model ("send one buffer, refill on
completion") maps directly onto nghttp2's `nghttp2_data_provider` and onto
`quiche_h3_send_body()`/`nghttp3` stream capacity.

### 5. Listeners

`nxt_socket_conf_t` (`src/nxt_router.h:263-313`) carries the listener
sockaddr, buffer sizes, timeouts and `tls`. The listening socket is created
in the main process on the router's request (`nxt_router_listen_socket_rpc_create`,
`src/nxt_router.c:3815`) by `nxt_listen_socket_create()`
(`src/nxt_listen_socket.c:50-180`): `SOCK_STREAM` only, `SO_REUSEADDR`
(`:64`), no `SO_REUSEPORT` anywhere in `src/`, then `listen()` (`:158`). No
UDP listener exists: `SOCK_DGRAM` appears only in the port tests. Each engine
thread registers its own `nxt_listen_event_t` on the shared fd
(`nxt_router_listen_socket_create`, `src/nxt_router.c:4773-4803`;
`nxt_listen_event()`, `src/nxt_conn_accept.c:37-75`) and the kernel spreads
accepts with `EPOLLEXCLUSIVE`. `listen_threads` (`src/nxt_router.c:2227`,
default `nxt_ncpu`, `:2629`) sizes the engine set in
`nxt_router_engines_create()` (`:4293-4363`). Accepted connections get
`skcf->listen->handler = nxt_http_conn_init` (`:3065`) and are counted
against `engine->max_connections` (`src/nxt_conn_accept.c:94`).

For QUIC this is the hard part: there is no accept(2); every datagram on the
UDP socket belongs to some connection identified by its Destination
Connection ID, and with N engines the packet must reach the engine that
owns the connection. §7 covers it.

### 6. What the router needs from a request

`nxt_router_prepare_msg()` (`src/nxt_router.c:7627-7900`) reads `r->method`,
`r->version` (must be a string: `HTTP/2.0`), `r->remote`, `r->local`,
`r->server_name` (set from `r->host` in `nxt_http_application_handler`,
`src/nxt_http_request.c:646-668`), `r->target`, `r->path`, `r->args`, the
field list (fields with `skip` set are dropped), `r->content_length_n`,
`r->tls`, `r->websocket_handshake`, and copies `r->body`. Routing
(`src/nxt_http_route.c`) matches on `r->method`, `r->host`, `r->path`,
`r->args`, headers, cookies, `scheme` (`:482-490`, from `r->tls`), `source`
(`r->remote`) and `destination` (`r->local`, via `local_addr`). The access
log variables (`src/nxt_http_variables.c:59-125`) use the same fields plus
`body_bytes_sent`. None of this knows about connections.

## Considered options

### Option 1. HTTP/2 via nghttp2 over the existing TLS connections (chosen for h2)

nghttp2 is callback-based and loop-agnostic: the application feeds bytes with
`nghttp2_session_mem_recv()` and pulls frames with
`nghttp2_session_mem_send()`; HPACK is inside. It ships in every target
distro (Ubuntu 24.04 `libnghttp2-dev` 1.59, Debian 12/13, Fedora, RHEL 8+
`libnghttp2-devel` in AppStream/CRB, Alpine, Amazon Linux 2023), MIT
licence, and libcurl already links it.

**Integration sketch**

- `auto/ssltls`: new `--nghttp2`/`NXT_HAVE_NGHTTP2` probe; `auto/sources`
  adds `src/nxt_h2proto.c`. ALPN needs OpenSSL 1.0.2+, so the 1.1.1 floor
  holds.
- `nxt_openssl_server_init()`: when the listener enables h2,
  `SSL_CTX_set_alpn_select_cb(ctx, nxt_openssl_alpn_select, conf)` with the
  list `\x02h2\x08http/1.1` (spike: `h2_epoll_server.c`,
  `alpn_select_cb`). One more `nxt_tls_conf_t` bit, set from a listener
  option (`"http2": true`, validated in `nxt_conf_validation.c` next to the
  `tls` object at `:587`). Per-bundle contexts inherit the callback because
  `nxt_openssl_servername()` swaps the `SSL_CTX` before ALPN is evaluated
  (OpenSSL runs the servername callback first).
- `nxt_h1p_conn_proto_init()` (`src/nxt_h1proto.c:465`) becomes the
  protocol chooser: if `c->u.tls` and `SSL_get0_alpn_selected()` says `h2`,
  call `nxt_h2p_conn_init(task, c)` instead of allocating an
  `nxt_h1proto_t`. Plain-text h2c (`Upgrade: h2c` / prior knowledge) is out
  of scope: browsers do not use it and it would drag `Upgrade` handling
  into h1.
- `nxt_h2proto_t` (per connection, `c->socket.data`): the
  `nghttp2_session`, the `nxt_conn_t`, a stream list, a small output
  staging buffer, counters. `nxt_h2p_stream_t` (per stream, in the request
  pool): `h2c`, `stream_id`, the `nxt_http_request_t`, the response buffer
  chain and its tail, `deferred`, flow-control state.
- Read side: a connection state `nxt_h2p_read_state` whose
  `io_read_handler` reads into an engine buffer and whose `ready_handler`
  runs `nghttp2_session_mem_recv()` over it, then calls
  `nxt_h2p_conn_flush()`. The buffer is completed immediately: nghttp2
  copies what it needs (headers go to HPACK, DATA to the
  `on_data_chunk_recv` callback).
- Write side: `nxt_h2p_conn_flush()` loops `nghttp2_session_mem_send()`
  into a memory buffer chained on `c->write`, then `nxt_conn_write()`; the
  existing `nxt_openssl_conn_io_sendbuf()` and `WANT_*` handling do the
  rest. Back-pressure: stop calling `mem_send` while `c->write` holds more
  than, say, two 16 KB frames; resume from the buffer completion handler.
- Stream to request: `on_begin_headers` calls `nxt_http_request_create()`
  and sets `r->proto.h2 = stream`, `r->protocol = NXT_HTTP_PROTO_H2`,
  `r->remote = c->remote`, `r->tls = 1`, `r->task = c->task`, `r->conf =
  joint; joint->count++` exactly like `:499-548`. `on_header` appends an
  `nxt_http_field_t` per pair through `nxt_http_req_field_add()`
  (`src/nxt_http.h:285-300`) with name and value copied into
  `r->mem_pool`; `:method`/`:path`/`:authority`/`:scheme` fill `r->method`,
  `r->target` (+ `nxt_http_parse_target`-style split into `r->path`/`r->args`
  and `r->quoted_target`), `r->host`, and are not added to the list.
  `r->version = "HTTP/2.0"`. On `END_HEADERS` run
  `nxt_http_fields_process()` with the shared neutral field table (phase 0)
  so `Content-Length`, `Cookie`, `Traceparent` etc. land where h1 puts
  them; `nxt_http_request_host()` validates `:authority` as it does `Host`.
  Reject `connection`, `transfer-encoding`, `upgrade` (RFC 9113 §8.2.2)
  with `RST_STREAM(PROTOCOL_ERROR)`; reject a `TE` other than `trailers`.
- Body and flow control: `body_read` (`nxt_h2p_request_body_read`) allocates
  `r->body` through the neutral allocator (phase 0) sized by
  `content-length` when present, else grows a temp file like the chunked
  path, and returns; `on_data_chunk_recv` appends and enforces
  `max_body_size`; `END_STREAM` on the request side runs
  `r->state->ready_handler`, so the whole body is buffered before the
  application sees it, which is how h1 works today (`prepare_msg` copies
  `r->body`). Flow control follows the buffer: create the session with
  `nghttp2_option_set_no_auto_window_update(1)`, advertise an initial
  stream window of `body_buffer_size`, and call
  `nghttp2_session_consume()` as bytes are moved into `r->body`. For
  `share` and `return` actions the body is discarded by consuming at once.
  Without a `content-length`, set `r->chunked = 1` so
  `nxt_http_request_chunked_transform()` synthesises `Content-Length` from
  the buffer (after the `chunked_field` NULL guard).
- Response: `header_send` maps `r->status` and every `r->resp` field to
  `nghttp2_nv[]` (lower-case names; drop `Connection`, `Keep-Alive`,
  `Transfer-Encoding`, which `set_headers` may have added) and calls
  `nghttp2_submit_response()` with a data provider whose source is the
  stream. Then, exactly as h1 does (`:1560-1575`), queue `body_handler` on
  the fast work queue. `send` appends `out` to the stream's chain and calls
  `nghttp2_session_resume_data()` if the provider is deferred, then
  `nxt_h2p_conn_flush()`. The provider's `read_callback` copies from the
  head buffer, and when a buffer is drained calls
  `nxt_sendbuf_drain()`-style completion (so static file refills and
  `nxt_http_request_mem_buf_completion` run), returns `NGHTTP2_ERR_DEFERRED`
  when the chain is empty and `EOF` when the sync "last" buffer
  (`nxt_http_buf_last`) is reached; completing that sync buffer is what
  ends the request (`nxt_http_request_done`, `src/nxt_http_request.c:1060`).
  `body_bytes_sent` is a counter in the stream. `discard` drops the chain
  and submits `RST_STREAM(INTERNAL_ERROR)` if headers went out, else a
  `nghttp2_submit_response` with the error status the caller set. `close`
  releases the joint, unlinks the stream, and, if the connection has no
  streams and received GOAWAY, shuts it down; otherwise the connection idles
  on `idle_timeout` (an `nxt_timer_t` on the connection, reusing
  `nxt_h1p_idle_timeout` semantics). `on_stream_close` for a stream whose
  request is still running (client `RST_STREAM`) sets `r->error` and calls
  the request error handler, which is the same path an h1 connection error
  takes (`nxt_h1p_conn_request_error`, `:1804`).
- Websocket: RFC 8441 (extended CONNECT) is not needed for parity; h2
  streams answer `426`/`400` to a websocket handshake and `ws_frame_start`
  is left NULL in the table (the call site is guarded, `:835`).
- Timeouts: `header_read_timeout` per stream between `HEADERS` and
  `END_STREAM`, `send_timeout` on the connection write, `idle_timeout` when
  no stream exists, all `nxt_timer_t`.
- `listen_threads` and `SO_REUSEPORT` are unaffected: h2 rides TCP accept.

**Spike result** (§8.1): 744 lines of C, compiled against the distro
nghttp2 1.59 and OpenSSL 3.0.13, served curl (four parallel streams, a
100 KB POST with window updates) and nghttp, with the response body produced
asynchronously from a timer through a deferred data provider, which is the
shape `nxt_h2p_request_send()` will have. Nothing about nghttp2 fought the
edge-triggered epoll model.

**Pros**: smallest delta; no new listener type; distro packages everywhere;
HPACK, priority, flow control, h2spec conformance are nghttp2's problem;
the same `nxt_conn_t`, TLS, timers and accounting as h1.
**Cons**: one more dependency on the h1+TLS hot path (behind `--nghttp2`);
per-stream requests share one connection so a connection error must fail N
requests; response buffers are copied one more time into nghttp2 frames.

### Option 2. HTTP/3 via OpenSSL's native QUIC server API

OpenSSL 3.5 (April 2025, LTS) added the QUIC server: `OSSL_QUIC_server_method()`,
`SSL_new_listener()`, `SSL_listen()`, `SSL_accept_connection()`,
`SSL_accept_stream()`, non-blocking mode with `SSL_set_blocking_mode(0)`,
readiness through `SSL_get_rpoll_descriptor()`/`SSL_get_wpoll_descriptor()`
plus `SSL_net_read_desired()`/`SSL_net_write_desired()`,
`SSL_get_event_timeout()` for the next timer and `SSL_handle_events()` to
advance the state machine, and `SSL_poll()` (3.4+) to multiplex many
`SSL` objects. OpenSSL does **not** ship an HTTP/3 layer: its own
`demos/http3/ossl-nghttp3.c` puts nghttp3 on top, and so would we (QPACK
and the HTTP/3 framing live in nghttp3).

**What is and is not available here.** The build host has OpenSSL 3.0.13
(`openssl version`), which has no QUIC at all (the client API arrived in
3.2, the server API in 3.5); `/usr/include/openssl/quic.h` does not exist
and no Ubuntu 24.04 package provides a newer libssl. The openssl.org and
GitHub hosts are blocked from this container, so a source build was not
possible and **the OpenSSL-native spike could not be run**; the analysis
below is from the 3.5 documentation and the `ossl-nghttp3` demo. Note that
CI already builds OpenSSL 3.6.2 and 4.0.2 from source into `/opt/openssl-3.6`
and `/opt/openssl-4.0` (`.github/workflows/build-test.yml:26-34,106-127`),
so a `--h3` leg has a QUIC-capable library available today.

**Event-loop integration model.** In the default single-thread mode the
library owns the UDP socket through a `BIO_s_datagram` and does no I/O
outside application calls: the loop registers the rpoll/wpoll descriptors
(the UDP fd) in epoll with the interest `SSL_net_read_desired()` /
`SSL_net_write_desired()` report, calls `SSL_handle_events()` when the fd is
readable/writable or when `SSL_get_event_timeout()` fires, and then calls
`SSL_accept_connection()`/`SSL_accept_stream()`/`SSL_read_ex()`/`SSL_write_ex()`
until they return `WANT_READ`. That maps onto one `nxt_fd_event_t` per
listener per engine plus one `nxt_timer_t` per *listener*: the listener's
`SSL_get_event_timeout()` covers every connection it owns, so the loop
drives all of them from one 0-bias timer re-armed after every
`SSL_handle_events()` rather than from FreeUnit's per-connection timers. `SSL_DOMAIN_FLAG_THREAD_ASSISTED`
spawns an internal thread and is exactly what we do not want; it must be
left off. `SSL_poll()` lets one call check readability of hundreds of
stream `SSL` objects, but it is a level-triggered scan, not an event
source, so per-stream readiness is still discovered by iterating.

**Streams to requests.** Each `SSL_accept_stream()` yields a stream `SSL *`;
bytes read from it go to `nghttp3_conn_read_stream()`, whose callbacks
(`recv_header`, `end_headers`, `recv_data`, `end_stream`) produce the
`nxt_http_request_t` the same way §Option 1 describes for nghttp2. Sending
is `nghttp3_conn_writev_stream()` → `SSL_write_ex()` on that stream's
`SSL`, with `nghttp3_conn_add_write_offset()`/`add_ack_offset()` bookkeeping.
This is more plumbing than nghttp2 because the QUIC stream object and the
HTTP/3 stream state are separate.

**UDP listener, GSO/GRO, threads.** OpenSSL owns the socket, so
`SO_REUSEPORT` per engine means one `SSL` listener per engine on its own
UDP socket, and there is no hook to route a migrated or retried connection
ID to the engine that owns it; the kernel's `SO_REUSEPORT` hash is by
4-tuple, so a client whose source port changes lands on another engine
with a connection ID that engine does not know. That is the general QUIC
multi-thread problem (§7), and with OpenSSL it cannot be solved by the
application because the library, not the application, reads the datagrams.
GSO/GRO: 3.5 does not expose send batching; the datagram BIO sends one
packet per `sendmsg`.

**TLS config reuse.** The `SSL_CTX` is a different method
(`OSSL_QUIC_server_method`), so `nxt_openssl_server_init()` would build a
second context per bundle from the same certificate, ciphers (TLS 1.3 only),
tickets and `conf_commands`; the SNI callback works unchanged. 0-RTT is
built in.

**Packaging.** OpenSSL ≥ 3.5 is in Ubuntu 25.10+, Fedora 42+, Debian 13
(3.5.x), Alpine 3.22+; RHEL 8/9 will never have it, RHEL 10 ships 3.5. The
OpenSSL floor stays 1.1.1 for h1/h2; `--h3` would require `>= 3.5` plus
`libnghttp3` (Debian 13, Fedora, Alpine, Ubuntu 24.04 0.8 which is too old:
nghttp3 1.x is needed for the `_versioned` API used by 3.5's demo).

**Pros**: no second TLS stack; one dependency (`nghttp3`) on top of the
library we already require; OpenSSL maintains the transport, congestion
control and the security fixes; tickets/SNI reuse.
**Cons**: youngest server API of the four (3.5 was its first release; the
QUIC client in 3.2–3.4 had notable performance gaps and 3.5's server
throughput is behind ngtcp2 and quiche in published comparisons); library
owns the socket, which forecloses application-level CID routing across
engines and GSO; a hard version floor that excludes RHEL 8/9 and every
distro older than 2025; two stream state machines to keep in step.

### Option 3. HTTP/3 via ngtcp2 + nghttp3 with an OpenSSL crypto backend

ngtcp2 is the reference-quality QUIC transport behind curl's `--http3`; it
has no I/O of its own: the application owns the UDP socket, calls
`ngtcp2_conn_read_pkt()` per datagram, `ngtcp2_conn_writev_stream()` to
produce packets (with `ngtcp2_conn_get_send_quantum()` for GSO batches), and
`ngtcp2_conn_get_expiry()`/`ngtcp2_conn_handle_expiry()` for the single
per-connection timer. nghttp3 sits on top with the same callback style as
nghttp2 (`nghttp3_conn_read_stream`, `nghttp3_conn_writev_stream`,
`nghttp3_conn_submit_response`). The crypto backend is pluggable:
`ngtcp2_crypto_ossl` for OpenSSL ≥ 3.5, `ngtcp2_crypto_quictls` for the
quictls fork, BoringSSL, wolfSSL, GnuTLS.

**Event-loop integration model.** Identical to the quiche spike (§8.2): one
`nxt_fd_event_t` on the UDP socket, a connection table keyed by CID hung
off the listen event, `nxt_timer_t` (bias 0) per connection re-armed from
`ngtcp2_conn_get_expiry()`. Everything is synchronous, no threads.

**Streams to requests.** nghttp3 callbacks are the h3 mirror of the
nghttp2 ones, so the `nxt_h2p_stream_t` design carries over: `recv_header`
→ `nxt_http_req_field_add`, `end_headers` → fields process, `recv_data` →
`r->body`, `end_stream` → ready handler; response headers via
`nghttp3_conn_submit_response()` with an `nghttp3_data_reader` that pulls
from the stream's buffer chain; `nghttp3_conn_block_stream()`/`unblock` for
QUIC-level flow control, `nghttp3_conn_add_ack_offset()` when ngtcp2
reports acked stream data (that is when the router buffers may be
completed, not on send).

**UDP listener needs.** New listener type: `nxt_listen_socket_create()`
grows a `SOCK_DGRAM` branch (`sa->type`, no `listen()`), `IP_PKTINFO`/
`IPV6_RECVPKTINFO` to learn the local address for `r->local` and for the
`ngtcp2_path`, `SO_REUSEPORT` when there is one socket per engine, and
optionally `UDP_GRO`/`UDP_SEGMENT` for GRO/GSO. Address validation (Retry
tokens, `ngtcp2_crypto_generate_retry_token`) and stateless reset tokens
need a per-router secret. Per-engine ownership and CID routing: §7.

**TLS config reuse.** With `ngtcp2_crypto_ossl` the `SSL_CTX` is a normal
TLS 1.3 server context created by `nxt_openssl_server_init()` plus
`ngtcp2_crypto_ossl_configure_server_context()`; certificates, SNI callback,
tickets and `conf_commands` work as they do for TCP. That is the best TLS
reuse of the three h3 options.

**Packaging.** Ubuntu 24.04 has `libngtcp2-dev` 0.12.1 and `libnghttp3-dev`
0.8.0 (installed here for the header check): both predate the 1.0 API
freeze and ship only the GnuTLS crypto backend
(`libngtcp2-crypto-gnutls-dev`), because the distro OpenSSL 3.0 has no QUIC
TLS API. Debian 13 has ngtcp2 1.11 / nghttp3 1.8 with the `quictls` and
`ossl` backends; Fedora 41+ has 1.x; Alpine 3.20+ has 1.x; RHEL/EPEL none.
Realistically `--h3` builds ngtcp2 + nghttp3 as vendored sources or as
`pkg/contrib` tarballs the way CI builds OpenSSL 3.6, and needs OpenSSL
≥ 3.5 for `ngtcp2_crypto_ossl` (or quictls, which is discontinued after
3.3 and not an option). MIT licences.

**Pros**: the model FreeUnit's engine wants (own socket, own timer, no
threads); the most complete conformance story (interop matrix, curl's
production stack); CID routing and GSO stay in our hands; the best TLS
reuse; nghttp3's callbacks mirror nghttp2's so h2 and h3 share a stream
design.
**Cons**: the most code to write (packet dispatch, CID table, Retry,
stateless reset, path validation policy, migration off); three libraries
(ngtcp2, ngtcp2_crypto_ossl, nghttp3) to vendor on most targets; an
OpenSSL ≥ 3.5 floor for `--h3` all the same.

### Option 4. HTTP/3 via quiche (Rust, C FFI)

quiche (Cloudflare, BSD-2) bundles QUIC, HTTP/3 and QPACK behind a C API
(`quiche.h`) and vendors BoringSSL for the TLS handshake. The I/O model is
the same as ngtcp2's: the application owns the UDP socket and calls
`quiche_conn_recv()`/`quiche_conn_send()`, `quiche_conn_timeout_as_millis()`
/`quiche_conn_on_timeout()`, and `quiche_h3_conn_poll()` for HTTP/3 events.

**Spike result** (§8.2): `cargo build --release` of quiche 0.30 with the
`ffi` feature took 2 m 25 s wall on the two shared cores (BoringSSL is built
by cmake inside), producing a 178 MB `libquiche.a` (3.5 MB `.so`) that must
be linked with `-lstdc++`; the 403-line C server driven from epoll served
three concurrent HTTP/3 GETs from an aioquic client (ALPN `h3`, QUIC v1),
with the per-connection timeout folded into `epoll_wait()`. It worked on
the first run, which speaks for the API.

**Build and packaging.** FreeUnit already builds a Rust component for OTel
(`src/otel/`, `auto/otel` checks `rustc`/`cargo`, deb/rpm builds carry
`cargo rustc` in their build deps, `pkg/deb/Makefile:49`), so a second
crate is not a new kind of dependency. But it is a different scale: the
OTel crate is a thin OTLP exporter; quiche pulls BoringSSL, `cmake`, a C++
toolchain and about 340 MB of build tree, and the resulting static archive
is 178 MB. No distro packages `libquiche` (only Cloudflare's own builds and
a few community packages), so every deb/rpm/Alpine build would compile it,
and crates.io access becomes a build-time requirement (it is reachable from
this container; `index.crates.io` returns 200, `crates.io/api` 403). Two TLS
stacks in one binary (OpenSSL for h1/h2, BoringSSL for h3) doubles the
CVE surface and breaks the "certificates from the existing cert store"
promise only cosmetically (PEM files load into both) but really for
`conf_commands`, `tickets` (quiche has its own session/0-RTT ticketing) and
the SNI bundle callback, which would have to be re-implemented on
quiche's `quiche_conn_set_session()`/per-SNI config selection. quiche does
support GSO-sized `send_quantum` and pacing hints (`quiche_send_info.at`).

**Pros**: everything in one library, the smoothest API, proven at
Cloudflare's scale, spike worked immediately.
**Cons**: a second TLS stack; heavy Rust/C++ build; nothing from distros;
SNI/tickets/conf_commands reimplemented; version churn (0.22 → 0.30 in a
year, no stable 1.0).

### Option 5. lsquic, msquic, picoquic (briefly)

- **msquic** (Microsoft, MIT): owns its worker threads and its own
  platform abstraction; callbacks arrive on msquic threads and the
  application must marshal back to the engine. Excellent performance,
  wrong model for one-thread-per-engine.
- **lsquic** (LiteSpeed, MIT): loop-agnostic like ngtcp2 and includes
  HTTP/3, but its engine is process-wide ("one `lsquic_engine` handles all
  connections"), timers are engine-level, and it needs BoringSSL; the API
  is older and has fewer eyes on it than ngtcp2's.
- **picoquic** (Christian Huitema, MIT): research-grade, loop-agnostic,
  its own h3 (`h3zero`), depends on picotls; little distro presence, small
  community. A fine reference, not a dependency for a server.
None is attractive against options 3 and 4.

## 7. Cross-cutting: QUIC and `listen_threads`

FreeUnit runs `listen_threads` engine threads, each with its own epoll and
its own `nxt_listen_event_t` on the shared TCP socket. For QUIC three layouts
are possible:

1. **One UDP socket, one owner engine.** Simplest and correct: all QUIC
   traffic for a listener is handled by engine 0; requests are still
   created there and dispatched to application ports through the normal
   per-engine port (`engine->port`). Other engines keep serving h1/h2.
   This is the phase-2 shape.
2. **One socket per engine with `SO_REUSEPORT` and eBPF steering.** Linux
   hashes the 4-tuple, so connection migration or NAT rebinding sends a
   known connection to the wrong engine. nginx solves it with an
   `SO_ATTACH_REUSEPORT_EBPF` program that maps a worker id encoded in the
   server-chosen CID to the socket index (`ngx_event_quic_bpf.c`). Doable
   later: ngtcp2 and quiche let us choose CIDs (`get_new_connection_id`
   callback; `quiche_accept` scid), OpenSSL does not expose enough.
3. **One socket, packets forwarded between engines.** The reading engine
   looks up the CID, and if the connection belongs to another engine posts
   the datagram with `nxt_event_engine_post()` (the locked work queue,
   `src/nxt_event_engine.c:235`). A copy per packet; fine for migration
   handling in layout 2, not as the main path.

All three keep the library inside one engine thread at a time; a QUIC
connection never migrates between engines.

## 8. Spikes

The spike code is not kept in the tree; it is in commit 48cc253d
(`docs/adr/0005-spikes/`, with the exact build and test commands).
Neither touches `src/`.

### 8.1 `h2_epoll_server.c` (nghttp2 + OpenSSL ALPN, epoll)

- Built with the distro `libnghttp2-dev` 1.59 and OpenSSL 3.0.13
  (`cc ... -lssl -lcrypto -lnghttp2`), no warnings that matter.
- ALPN via `SSL_CTX_set_alpn_select_cb()` selects `h2`;
  `SSL_get0_alpn_selected()` after the handshake decides the protocol.
- `curl --http2` got `HTTP/2 200`; `curl --parallel` with four URLs ran four
  streams (ids 1, 3, 5, 7) on one connection; a 100 KB `--data-binary` POST
  arrived complete (`body=100000`), exercising `WINDOW_UPDATE`; `nghttp -nv`
  showed SETTINGS, HEADERS and three DATA frames with END_STREAM.
- The body is produced asynchronously by a 100 ms timer through
  `NGHTTP2_ERR_DEFERRED` + `nghttp2_session_resume_data()`, which is the
  `nxt_h2p_request_send()` model. `h2load -n 4000 -c 8 -m 16` completed
  4000/4000 (the 419 req/s figure is the spike's deliberate 300 ms body
  timer, not a throughput number).

### 8.2 `h3_epoll_server.c` (quiche 0.30 FFI, epoll, UDP)

- `cargo build --release` with `features = ["ffi"]`: 2 m 25 s wall,
  341 MB target dir, 178 MB `libquiche.a`, link needs `-lstdc++`.
- One non-blocking UDP socket in epoll, `quiche_header_info()` → CID
  lookup → `quiche_accept()`/`quiche_conn_recv()`; `quiche_conn_send()` loop
  → `sendto()`; per-connection `quiche_conn_timeout_as_millis()` folded
  into `epoll_wait()`; `quiche_conn_on_timeout()` and closed-connection
  reaping after every poll.
- Test client: `h3_client.py` on aioquic 1.3.0 (pip; curl 8.5 here has no
  HTTP/3, and `gtlsclient` from `ngtcp2-client` needs a GnuTLS build with
  QUIC). Three GETs on streams 0, 4, 8 returned `:status 200` with bodies;
  the connection closed cleanly (`recv=10 sent=10 lost=0`).
- What the spike does not do: Retry/address validation, stateless reset,
  multiple sockets, GSO. All are application-side with quiche and ngtcp2.

### 8.3 Not run

The OpenSSL 3.5 native QUIC server could not be built here (see Option 2);
the ngtcp2 0.12 packages on this host predate the 1.0 API and only have the
GnuTLS backend, so a spike would not have represented the `ngtcp2_crypto_ossl`
integration and was skipped in favour of the quiche one, whose loop model
is the same.

## Decision outcome

**h2: option 1, nghttp2 over the existing TLS connections, after a phase 0
that generalises the protocol layer.** It is the smallest change, uses
distro packages on every target, keeps the OpenSSL floor, and the spike
confirmed the integration model end to end.

**h3: option 3, ngtcp2 + nghttp3 with `ngtcp2_crypto_ossl`, as an optional
`--h3` build requiring OpenSSL ≥ 3.5.** It keeps one TLS stack, reuses the
cert store, SNI, tickets and `conf_commands` unchanged, keeps the UDP socket
and the timers in FreeUnit's engine (which is what makes CID routing across
`listen_threads` and GSO possible later), and nghttp3's callbacks mirror
nghttp2's so the stream-to-request code from phase 1 is reused. quiche
(option 4) is the fallback if vendoring ngtcp2 proves too heavy: its FFI
worked first time, but a second TLS stack and a 178 MB Rust/BoringSSL build
per package are a poor fit for a project whose TLS story is "OpenSSL from
the distro". OpenSSL's own QUIC server (option 2) is re-evaluated when 3.5+
is the floor on every supported OS (RHEL 8 leaves the matrix in 2032, so not
soon), or if it gains an application-driven datagram path.

### Consequences

- A per-request protocol vtable already exists (`nxt_http_proto[]`); we
  add the H2 and H3 entries and move three things out of `nxt_h1proto.c`
  so they are shareable (§9, phase 0). No h1 behaviour changes.
- Workers see `HTTP/2.0`/`HTTP/3.0` in `SERVER_PROTOCOL`; every other field
  of `nxt_unit_request_t` is filled the same way. Header names arrive
  lower-case, which they may already (h1 clients do send lower-case), and
  `prepare_msg` upper-cases them for CGI names anyway
  (`src/nxt_router.c:7790-7806`).
- The access log `$response_header_connection` and
  `$response_header_transfer_encoding` are empty for h2/h3, which is
  correct (the fields do not exist on the wire).
- Security surface: nghttp2 has a long CVE history now largely closed
  (Rapid Reset CVE-2023-44487 is mitigated inside nghttp2 ≥ 1.57 by the
  per-connection RST_STREAM rate limit; we still cap
  `MAX_CONCURRENT_STREAMS` and stream creation rate ourselves). QUIC adds
  amplification (Retry before 3× reflection), address validation, stateless
  reset secrets, and UDP flood exposure; h3 stays opt-in and off by default.
  Two new parsers (HPACK/QPACK) run before routing: fuzz targets in
  `fuzzing/` must gain an h2 frame corpus.

## 9. Phased plan

### Phase 0: generalise the protocol layer (no behaviour change)

| Change | Files and functions |
|---|---|
| Split `nxt_h1p_fields[]` into `nxt_http_request_fields[]` (Host, Cookie, Referer, User-Agent, Content-Type, Content-Length, Authorization, Traceparent, Tracestate) exported from `nxt_http_request.c`, and an h1-only list (Connection, Upgrade, Sec-WebSocket-*, Transfer-Encoding) that chains to it | `src/nxt_h1proto.c:171-211` (`nxt_h1p_fields`, `nxt_h1p_init`), `src/nxt_http_request.c`, `src/nxt_http.h:430-458` |
| Factor `nxt_http_request_body_alloc(task, r, size)` out of `nxt_h1p_request_body_read()` (memory buffer vs temp file, `max_body_size`) | `src/nxt_h1proto.c:906-1137`, new function in `src/nxt_http_request.c` |
| Guard `r->chunked_field` in `nxt_http_request_chunked_transform()` | `src/nxt_http_request.c:555-586` |
| Add `nxt_h2proto_t *h2` / `nxt_h3proto_t *h3` to the `nxt_http_proto_t` union; add `NXT_HTTP_PROTO_H3`; size the table by the enum | `src/nxt_http.h:68-93`, `src/nxt_h1proto.c:136` (`nxt_http_proto[3]` → `[NXT_HTTP_PROTO_MAX]`) |
| Make `nxt_h1p_conn_proto_init()` the ALPN dispatcher (calls into h2 when built, else h1) | `src/nxt_h1proto.c:465-484` |
| Export the response status text tables used by `nxt_h1p_request_header_send()` (h2/h3 need only the number, but `nxt_http_request_error` pages reuse them) | `src/nxt_h1proto.c:1383-1440` |
| Unit tests in the `src/test/nxt_router_*_test.c` style for the field split and the body allocator | `src/test/` |

Effort: 3–5 engineer-days (1–2 agent-days). Tests: existing pytest suite
must stay green (`test/test_chunked.py`, `test/test_tls*.py`,
`test/test_access_log.py`, the OTel `traceparent` e2e tests).

### Phase 1: HTTP/2 via nghttp2 over TLS

| Change | Files and functions |
|---|---|
| `--nghttp2` option, probe, `NXT_HAVE_NGHTTP2`, `src/nxt_h2proto.c` in sources | `auto/options`, `auto/ssltls` (or a new `auto/nghttp2`), `auto/sources`, `auto/summary`, `auto/help` |
| Listener option `"http2": true` (only with `tls`), validation | `src/nxt_conf_validation.c:587-690`, `docs/unit-openapi.yaml` |
| `nxt_tls_conf_t::alpn_h2` bit; `SSL_CTX_set_alpn_select_cb()` in server init; protocol selection after handshake | `src/nxt_tls.h:64-82`, `src/nxt_openssl.c:216-370`, `:1158-1212`, `src/nxt_router.c:3005-3060` (`nxt_router_conf_tls_insert` caller) |
| `src/nxt_h2proto.c`, `src/nxt_h2proto.h`: connection init, read state, flush, nghttp2 callbacks, the seven request hooks, timers, GOAWAY/shutdown, connection error → fail all streams | new files; table entry in `src/nxt_h1proto.c:136-167` |
| `body_bytes_sent` counter; `$response_header_*` variables unchanged | `src/nxt_http_variables.c:431-454` |
| Access log `$request_line` for h2 (`METHOD target HTTP/2.0`) | `src/nxt_h2proto.c` sets `r->request_line` |
| `/status` connection counters per protocol (optional) | `src/nxt_status.c` |
| Tests: `test/test_http2.py` (Python `h2`/`httpx[http2]` client or `curl --http2` via subprocess, reusing `test/unit/applications/tls.py` certificates); h2spec in CI (`h2spec -t -k -h 127.0.0.1 -p PORT`, strict mode, allow-list of known generic failures); h2load smoke; fuzz target feeding `nghttp2_session_mem_recv` | `test/`, `.github/workflows/build-test.yml`, `fuzzing/` |

Effort: 15–25 engineer-days (5–8 agent-days), of which h2spec conformance
and the error/close matrix (client RST mid-body, GOAWAY with streams in
flight, app crash with N streams) are half.

### Phase 2: HTTP/3 via ngtcp2 + nghttp3 behind `--h3`

| Change | Files and functions |
|---|---|
| `--h3` option: probes for OpenSSL ≥ 3.5, `libngtcp2`, `libngtcp2_crypto_ossl`, `libnghttp3` (pkg-config), or `pkg/contrib` tarballs like OpenSSL in CI | `auto/options`, new `auto/h3`, `auto/sources`, `pkg/contrib/`, `.github/workflows/build-test.yml` (an `h3` leg on `/opt/openssl-3.6`) |
| UDP listener: `sa->type = SOCK_DGRAM` branch, `IP_PKTINFO`, no `listen()`; a `quic` bit on `nxt_listen_socket_t`; the main process creates it like the TCP one | `src/nxt_listen_socket.c:50-180`, `src/nxt_listen_socket.h:11-31`, `src/nxt_sockaddr.c`, `src/nxt_main_process.c` (socket RPC) |
| Listener option `"http3": true` with `tls`; `Alt-Svc: h3=":port"` injected on h1/h2 responses of the paired TCP listener (`nxt_http_request_header_send`) | `src/nxt_conf_validation.c`, `src/nxt_router.c:2980-3070`, `src/nxt_http_request.c:688-826` |
| Per-engine ownership: layout 1 (§7) first, the UDP `nxt_listen_event_t` registered on engine 0 only | `src/nxt_router.c:4773-4803` (`nxt_router_listen_socket_create`), `:4293` |
| `src/nxt_h3proto.c`: datagram read loop (`recvmmsg`), CID table (`nxt_lvlhsh_t`), Retry and stateless reset secrets, `ngtcp2_conn_server_new`, per-connection `nxt_timer_t` bias 0 from `ngtcp2_conn_get_expiry()`, send loop with `ngtcp2_conn_writev_stream()` and a pending datagram on `EAGAIN` (enable write on the listener fd), nghttp3 callbacks reusing the phase-1 stream-to-request code, request hooks, ack-driven buffer completion, idle/close/draining | new file; table entry |
| TLS: `ngtcp2_crypto_ossl_configure_server_context()` on the bundle `SSL_CTX` built by `nxt_openssl_server_init()`; SNI callback unchanged; 0-RTT off in this phase | `src/nxt_openssl.c:216-370` |
| `r->local` from `IP_PKTINFO`; `r->remote` from the datagram source; `r->tls = 1`; `r->version = "HTTP/3.0"` | `src/nxt_h3proto.c` |
| Tests: `test/test_http3.py` with aioquic (`pip install aioquic`, IPv4 endpoint as in `h3_client.py`) and `curl --http3` where available; the ngtcp2 `examples/client` in CI's h3 leg; existing routing/static/compression/access-log tests parametrised over `http3`; interop run against the QUIC interop runner images when a second stack is on hand | `test/`, `test/requirements.txt`, CI |

Effort: 25–40 engineer-days (10–15 agent-days). The transport plumbing
(Retry, CID table, migration policy, close/draining) is the bulk; the
HTTP/3-to-request part is phase-1 code with nghttp3 names.

### Phase 3: performance and completeness

- GSO (`UDP_SEGMENT`) with `ngtcp2_conn_get_send_quantum()` and GRO
  (`UDP_GRO`) on receive; `recvmmsg`/`sendmmsg` batching.
- `SO_REUSEPORT` per engine with CID-encoded engine id and an
  `SO_ATTACH_REUSEPORT_EBPF` steering program (layout 2, §7), with the
  forward-on-mismatch fallback (layout 3).
- 0-RTT with anti-replay policy (only idempotent methods before the
  handshake confirms), session tickets from the existing `tickets` keys.
- h2: `nghttp2_option_set_no_auto_window_update` tuning against
  `body_buffer_size`, priority hints, `MAX_CONCURRENT_STREAMS` per
  listener option.
- Fuzz corpora for HPACK/QPACK inputs; `/status` per-protocol counters;
  perf gates (`.github/workflows/perf-gates.yml`) extended with an
  h2load/ngtcp2 client run.

Effort: 10–15 engineer-days (4–6 agent-days), spread as needed.

## Pros and cons of the options

| | Loop model | Threads | Deps (Ubuntu 24.04 / Debian 13 / RHEL) | TLS reuse | Effort |
|---|---|---|---|---|---|
| 1. nghttp2 (h2) | bytes in/out on existing `nxt_conn_t` | none | pkg / pkg / pkg | full | 15–25 d |
| 2. OpenSSL QUIC + nghttp3 | lib owns UDP BIO; rpoll/wpoll fds + `SSL_get_event_timeout` | none if thread-assisted mode is off | needs OpenSSL ≥ 3.5: none / pkg / RHEL 10 only; nghttp3 1.x: none / pkg / none | full (second `SSL_CTX` per bundle) | 20–30 d |
| 3. ngtcp2 + nghttp3 (ossl) | app owns UDP socket and timers | none | 0.12 (unusable) / pkg 1.x / none; OpenSSL ≥ 3.5 | full (same `SSL_CTX`) | 25–40 d |
| 4. quiche | app owns UDP socket and timers | none | none / none / none; cargo + cmake + C++ | second TLS stack; SNI/tickets reimplemented | 20–30 d |
| 5. msquic / lsquic / picoquic | own threads / process-wide engine / research | msquic yes | none | partial | n/a |

## Top risks

1. **OpenSSL ≥ 3.5 as the h3 floor** leaves RHEL 8/9, Ubuntu 22.04/24.04 and
   Debian 12 without h3 unless we ship OpenSSL ourselves (CI already does
   for 3.6/4.0; packages do not). Mitigation: `--h3` optional; document;
   revisit quiche if a self-contained build matters more than one TLS stack.
2. **Vendoring ngtcp2/nghttp3** on most targets adds two moving upstreams to
   the security-update burden. Mitigation: pin in `pkg/contrib` with
   checksums like the other contrib tarballs; track their advisories.
3. **Multiplexed error semantics** (one connection, many requests) are new
   to the router: a TLS error must fail every in-flight request and every
   port RPC; the app queue and `nxt_request_rpc_data_t` lifetime
   (`src/nxt_router.c:7428-7470`) were designed for one request per
   connection. Mitigation: phase 1 spends its review time here, with an
   explicit test matrix.
4. **QUIC on multiple engines** is not solved by any library; layout 1
   serialises h3 on one engine per listener until phase 3.
5. **`nxt_router.c` churn** from the IPC/status/schedules streams this week:
   all listener and `prepare_msg` line numbers above will move.
6. **Amplification and resource exhaustion** on UDP (no SYN cookies):
   Retry-always under load, connection and stream caps per listener, and a
   per-source rate limit are required before h3 is on by default anywhere.

## Open questions

- Should h2 be on by default whenever a listener has `tls` (as nginx does
  not, and Caddy does)? Proposal: off by default in phase 1, default on
  once h2spec is in CI.
- Do we want plaintext h2c for internal proxies (`nxt_http_proxy.c` peers)?
  Not in this plan; the peer protocol field makes it possible later.
- Alt-Svc on the TCP listener vs a single listener object with both
  transports: the JSON shape (`"listeners": {"*:443": {"http3": true}}`)
  implies one config entry creating a TCP and a UDP socket on the same
  port; the main-process socket RPC would carry two fds. To be decided in
  phase 2 design.
