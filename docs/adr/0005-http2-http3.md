# 0005. HTTP/2 in the router with nghttp2; HTTP/3 deferred

- Status: proposed (h2 decided; h3 analysed and deferred, see §Decision)
- Date: 2026-09-23, revised 2026-09-24 after review
  (`docs/adr/0005-REVIEW.md`)
- Stream: h2/h3 exploration (design only; no `src/` change on this branch)
- Code base: every `file:line` below refers to the tree at the tip of
  `proto/h2h3-review` (based on `ddf19bc3`, integration of 2026-09-23). The
  IPC, router, status and schedules streams are editing `src/nxt_router.c`,
  so re-check `nxt_router.c` lines before editing; the other files have
  been stable.
- Spikes: `docs/adr/0005-spikes/` (an nghttp2 server and a quiche HTTP/3
  server, both driven from a bare epoll loop; see §8 and the README there
  for the build and test commands).

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

We want HTTP/2 (RFC 9113) and, later, HTTP/3 (RFC 9114 over QUIC, RFC 9000)
with these constraints:

1. h2 and h3 **streams terminate in the router** and become the same
   `nxt_http_request_t` the h1 path produces; `nxt_router_prepare_msg()`
   (`src/nxt_router.c:7737`) then builds the `nxt_unit_request_t` the
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
protocol layer must be generalised first, and what a first h2 release
contains. The owner's standing rule applies: less code; nothing that a
first h2 release does not need.

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
  (`src/nxt_openssl.c:1028-1110`), `conf_commands`, `session` cache and
  `tickets` all keep working for h2 and, where the transport allows, for h3.
- **Packaging reality.** The OpenSSL floor is 1.1.1 because of RHEL 8
  (`auto/ssltls:38-53`); the deb/rpm builds depend on the distro `libssl-dev`
  (`pkg/deb/debian/control.in:7`, `pkg/rpm/unit.spec.in:8-10`). h2 must not
  raise that floor; h3 may be an optional `--h3` build.
- **Reviewable and mergeable in small steps**, each with h2spec / curl /
  pytest evidence.
- **Secure by construction.** A multiplexed protocol multiplies every
  per-request cost by the stream count; the known h2 attack classes must be
  acceptance criteria, not follow-ups (§Security requirements).

## The code today

### 1. The event engine

`nxt_event_engine_start()` (`src/nxt_event_engine.c:507-558`) is the loop:
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
(`src/nxt_conn_write.c:17-130`): it calls `c->io->sendbuf()` in a loop of at
most 10 MB (`:53`), then `nxt_sendbuf_update()`, and completes buffers
through their `completion_handler` (`nxt_sendbuf_drain()`,
`src/nxt_sendbuf.c:397`). That completion callback is the router's
back-pressure: for static files `nxt_http_static_buf_completion()`
(`src/nxt_http_static.c:1800-1875`) reads the next slice with
`nxt_file_read()` only when the previous one is done.

Idle connections sit on `engine->idle_connections`
(`src/nxt_event_engine.c:132`, `nxt_conn_idle()` from
`nxt_h1p_keepalive()`, `src/nxt_h1proto.c:2034`); process shutdown closes
them from `nxt_runtime_close_idle_connections()` (`src/nxt_runtime.c:484`).

Timers: `nxt_timer_t` (`src/nxt_timer.h:24-41`) lives in an rbtree per
engine, `nxt_timer_add()` (`src/nxt_timer.c:70`) coalesces timers within
`timer->bias` (default 50 ms, `NXT_TIMER_DEFAULT_BIAS`, `src/nxt_timer.h:12`,
set by `nxt_conn_timer_init`, `src/nxt_conn.h:199-204`). h2 keeps the
connection timers as they are; QUIC loss-detection timers need 1 ms
granularity, so h3 timers would use `bias = 0`.

### 2. The TLS layer

`--openssl` (`auto/options:116`) sets `NXT_OPENSSL=YES`; `auto/ssltls`
probes `-lssl -lcrypto`, enforces the 1.1.1 floor (`:38-53`) and detects
`SSL_CONF_cmd` and tlsext; `auto/sources:116,219-220` then compiles
`src/nxt_openssl.c` behind the generic `NXT_TLS`. The generic vtable is
`nxt_tls_lib_t` (`src/nxt_tls.h:35-44`: `library_init`, `server_init`,
`server_free`) and `nxt_tls_conf_t::conn_init` (`:70`).

`nxt_openssl_server_init()` (`src/nxt_openssl.c:217-370`) builds one
`SSL_CTX` per certificate bundle (`SSLv23_server_method()`, options
`:239-254`, `nxt_openssl_chain_file()`, ciphers, `conf_commands` via
`SSL_CONF_cmd`, session cache, ticket keys) and, for the last bundle, sets
`conf->conn_init = nxt_openssl_conn_init` and the SNI callback
`nxt_openssl_servername()` which swaps `SSL_set_SSL_CTX()` per bundle
(`:1096`). **There is no ALPN configuration anywhere** (grep for `alpn` in
`src/` is empty), which is why every client today falls back to h1 over TLS.

Per connection, `nxt_openssl_conn_init()` (`:1159-1212`) does `SSL_new`,
`SSL_set_fd(c->socket.fd)`, `SSL_set_accept_state`, installs
`c->io = &nxt_openssl_conn_io` and forces `c->sendfile = OFF` (`:1205`).
The handshake is driven by the engine: `nxt_openssl_conn_handshake()`
(`:1236-1320`) calls `SSL_do_handshake()`, and on `WANT_READ`/`WANT_WRITE`
`nxt_openssl_conn_test_error()` (`:1542-1600`) toggles the fd's read/write
interest and returns `NXT_AGAIN`. I/O is `SSL_read` in
`nxt_openssl_conn_io_recvbuf()` (`:1330-1352`) and `SSL_write` in
`nxt_openssl_conn_io_sendbuf()`/`_send()` (`:1365-1402`). This is exactly
the shape a callback-based h2 library needs: bytes in, bytes out, no
ownership of the socket.

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
`src/nxt_http_request.c:673` (`local_addr`), `:682` (`body_read`), `:820`
(`header_send`), `:836` (`ws_frame_start`), `:1004` (`send`), `:1089`
(`discard`), `:1142` (`close`); `src/nxt_http_route.c:1666` (`local_addr`
for a `destination` match). `src/nxt_http_variables.c:445` calls
`body_bytes_sent` unguarded, so every protocol must implement it (DEVNULL
does). The proxy path uses `peer_*` through `peer->protocol`
(`src/nxt_http_proxy.c:177,212,314,465`), which stays h1: an h2 client
request can be proxied over an h1 upstream because the peer protocol is a
separate field.

**Direct `r->proto.h1` dereferences outside `nxt_h1proto*.c`** (the ones
that would break for a new protocol) are exactly two, both already fenced by
a protocol check: `src/nxt_http_variables.c:536` (`$response_header_connection`,
returns empty unless `r->protocol == NXT_HTTP_PROTO_H1`, `:531`) and
`:606` (`$response_header_transfer_encoding`). Everything else passes
`r->proto.any` opaquely as the error/close handler's `data` argument
(`src/nxt_http_error.c:80,117`, `src/nxt_http_static.c:1826,1843`,
`src/nxt_http_websocket.c:80,153`, `src/nxt_router.c:7604`,
`src/nxt_router_access_log.c:384`, `src/nxt_http_request.c:1072`).
`nxt_http_devnull.c:7-14` records the same survey.

What *is* h1-specific but not reachable through the table:

- The request field processing table `nxt_h1p_fields[]` and its hash
  `nxt_h1p_fields_hash` are `static` in `src/nxt_h1proto.c:170-196`, built
  by `nxt_h1p_init()` (`:211`) with `nxt_http_fields_hash()`
  (`src/nxt_http_parse.c:1183`). The array is ordered: five h1 framing
  entries first (`Connection`, `Upgrade`, `Sec-WebSocket-Key`,
  `Sec-WebSocket-Version`, `Transfer-Encoding`), then the protocol-neutral
  ones (`Host`, `Cookie`, `Referer`, `User-Agent`, `Content-Type`,
  `Content-Length`, `Authorization`, and the OTel
  `Traceparent`/`Tracestate`). `nxt_http_fields_process()`
  (`:1264`) takes exactly one `nxt_lvlhsh_t`; lookups are by `field->hash`
  and a case-insensitive compare (`nxt_http_field_hash_test`, `:1161`), and
  the hash is the per-character `nxt_http_field_hash_char()` over the
  lower-cased name (`src/nxt_http_parse.h:120-122`). A new protocol must
  run `nxt_http_fields_process()` with a hash over the neutral tail of that
  array, and must compute `field->hash` itself, or OTel propagation,
  cookies and `Content-Length` silently stop working.
- `nxt_http_request_chunked_transform()` (`src/nxt_http_request.c:557-586`)
  synthesises `Content-Length` from `r->body->file_end` when `r->chunked`
  and dereferences `r->chunked_field->skip` unconditionally (`:563`). h2/h3
  requests without a `content-length` header are the h2 analogue of
  chunked, so this becomes `if (r->chunked_field != NULL)`.
- `nxt_h1p_request_body_read()` (`src/nxt_h1proto.c:906-1137`) mixes the
  neutral part (`:967-1030`: allocate `r->body` as a memory buffer up to
  `body_buffer_size`, else a `mkstemp` file in `body_temp_path`; enforce
  `max_body_size`, `:1055,1189`) with h1 framing (reads from `c->read`,
  chunk parsing). The allocation and the "ready when complete" contract
  (`r->state->ready_handler`) are what h2/h3 reuse.
- `nxt_h1p_request_header_send()` (`:1383-1595`) serialises `r->status` and
  `r->resp` into a status line and `Name: value\r\n` lines, decides
  chunked vs `Content-Length` vs close, and queues the body handler on the
  fast work queue *before* `nxt_conn_write()` (`:1575`). The generic part
  happens earlier in `nxt_http_request_header_send()`
  (`src/nxt_http_request.c:689-826`): `set_headers`, `Server`, `Date`,
  `Content-Length` synthesis, Vary merging. h2/h3 only need to encode
  `r->resp` as `(name, value)` pairs; the error page body
  (`src/nxt_http_error.c:20-24`) uses only the numeric status, so the h1
  status-line tables (`src/nxt_h1proto.c:1285-1400`) are not needed.
- `nxt_h1p_request_send()` (`:1658-1690`) appends to `c->write` with chunk
  framing; `nxt_h1p_request_body_bytes_sent()` (`:1765`) is
  `c->sent - header_size`; `nxt_h1p_request_discard()` (`:1779`) drops
  `c->write` and drains `last`; `nxt_h1p_request_close()` (`:1905-1940`)
  releases the joint (`:1917`) and goes to keep-alive (`nxt_h1p_keepalive`,
  `:2012-2060`, which re-zeroes the `nxt_h1proto_t` up to `conn`) or
  shutdown (`:2230`).
- The request's task and timers are the connection's (`:520-548`), and the
  joint reference `joint->count++` (`:546`) pairs with
  `nxt_router_conf_release()` in close (`:1917`). A per-stream request must
  do the same.
- Graceful reload: the listener drains in two phases
  (`nxt_router_listen_socket_close`, `src/nxt_router.c:5012-5100`); an idle
  h1 connection notices `c->listen->draining` or `joint == NULL` in
  `nxt_h1p_idle_io_read_handler()` (`:420-431`) and switches to
  `nxt_h1p_idle_close_state` (`:2072-2090`). The h1 `Upgrade` callback is
  the only thing that sets `r->websocket_handshake` (`:757`).

### 4. Static serving and sendfile

`share` never uses sendfile. `nxt_http_static_body_handler()`
(`src/nxt_http_static.c:1738-1790`) allocates up to `NXT_HTTP_STATIC_BUF_COUNT`
(`:32`, two) memory buffers of `NXT_HTTP_STATIC_BUF_SIZE`, and
`nxt_http_static_buf_completion()` (`:1800-1875`) fills each with
`nxt_file_read()` (pread) and calls `nxt_http_request_send()`; the file
descriptor buffer `fb` is only a cursor (`file_pos`/`file_end`, `:1007-1010`).
The only sendfile code, `nxt_conn_io_sendfile()` (`src/nxt_conn_write.c:202-260`),
runs for file buffers on plain connections, which the static path never
produces, and TLS disables it anyway (`c->sendfile = NXT_CONN_SENDFILE_OFF`,
`src/nxt_openssl.c:1205`). Compression of static responses writes a temp
file and serves that (`nxt_http_comp_compress_static_response`,
`src/nxt_http_compression.c:317`). **Consequence: h2 and h3 need no change
in `nxt_http_static.c`**; the pull model ("send one buffer, refill on
completion") maps directly onto nghttp2's `nghttp2_data_provider` and onto
`nghttp3`'s data reader.

### 5. Listeners

`nxt_socket_conf_t` (`src/nxt_router.h:263-313`) carries the listener
sockaddr, buffer sizes, timeouts and `tls`; the defaults are set in
`nxt_router_conf_create()` (`src/nxt_router.c:3019-3035`: `header_buffer_size`
2048, `large_header_buffer_size` 8192 × `large_header_buffers` 4,
`body_buffer_size` 16 KiB, `max_body_size` 8 MiB, `idle_timeout` and the
read/send timeouts 30 s). The listening socket is created in the main
process on the router's request (`nxt_router_listen_socket_rpc_create`,
`src/nxt_router.c:3887`) by `nxt_listen_socket_create()`
(`src/nxt_listen_socket.c:30-160`): `SOCK_STREAM` only (`:96,106`),
`SO_REUSEADDR` (`:64`), no `SO_REUSEPORT` anywhere in `src/`, then
`listen()` (`:151`). No UDP listener exists: `SOCK_DGRAM` appears only in
the port tests. Each engine thread registers its own `nxt_listen_event_t`
on the shared fd (`nxt_router_listen_socket_create`, `src/nxt_router.c:4845`;
`nxt_listen_event()`, `src/nxt_conn_accept.c:38-75`) and the kernel spreads
accepts with `EPOLLEXCLUSIVE`. `listen_threads` (`src/nxt_router.c:2299`,
default `nxt_ncpu`, `:2702`) sizes the engine set in
`nxt_router_engines_create()` (`:4365`). Accepted connections get
`skcf->listen->handler = nxt_http_conn_init` (`:3137`) and are counted
against `engine->max_connections` (`src/nxt_conn_accept.c:94`).

For QUIC this is the hard part: there is no accept(2); every datagram on the
UDP socket belongs to some connection identified by its Destination
Connection ID, and with N engines the packet must reach the engine that
owns the connection. §7 covers it.

### 6. What the router needs from a request

`nxt_router_prepare_msg()` (`src/nxt_router.c:7715-7900`) reads `r->method`,
`r->version` (must be a string: `HTTP/2.0`), `r->remote`, `r->local`,
`r->server_name` (set from `r->host` in `nxt_http_application_handler`,
`src/nxt_http_request.c:646-668`), `r->target`, `r->path`, `r->args`, the
field list (fields with `skip` set are dropped), `r->content_length_n`,
`r->tls`, `r->websocket_handshake`, and copies `r->body`. It now refuses,
before allocating, what the libunit protocol cannot carry: a method longer
than 255 bytes is answered 501 (`:7759`), a field name that with the
CGI prefix (`HTTP_`, `:426`) exceeds 255 bytes is answered 431 (`:7815`),
and the whole request must fit `PORT_MMAP_DATA_SIZE`. The h1 parser
already caps a name at `NXT_HTTP_MAX_FIELD_NAME` = 255
(`src/nxt_http_parse.c:28,574,594`) while `nxt_http_field_t.name_length` is
a `uint16_t` (`src/nxt_http.h:319`): an HPACK-decoded name must be capped
by the h2 layer itself. Routing (`src/nxt_http_route.c`) matches on
`r->method`, `r->host`, `r->path`, `r->args`, headers, cookies, `scheme`
(`:2045-2053`, from `r->tls`), `source` (`r->remote`) and `destination`
(`r->local`, via `local_addr`). The access log variables
(`src/nxt_http_variables.c:57-125`) use the same fields plus
`body_bytes_sent` and `$request_line` (`:91`, set by h1 at
`src/nxt_h1proto.c:623-629`). None of this knows about connections.

## Considered options

### Option 1. HTTP/2 via nghttp2 over the existing TLS connections (chosen)

nghttp2 is callback-based and loop-agnostic: the application feeds bytes with
`nghttp2_session_mem_recv()` and pulls frames with
`nghttp2_session_mem_send()`; HPACK is inside. It ships in every target
distro (Ubuntu 22.04 1.43, 24.04 1.59, Debian 12 1.52, 13 1.64, Fedora 1.6x,
RHEL 8/9 `libnghttp2-devel` in AppStream/CRB, Alpine, Amazon Linux 2023),
MIT licence, and libcurl already links it. The distros backport security
fixes without bumping the version (Ubuntu 24.04's 1.59 has
`nghttp2_option_set_max_continuations` and
`nghttp2_option_set_stream_reset_rate_limit`, upstream 1.61 and 1.57), and
the `nghttp2_ssize`/`*2` API (`mem_send2`, `data_provider2`, upstream 1.62)
is absent below Debian 13, so **the code uses the classic `ssize_t` API,
never defines `NGHTTP2_NO_SSIZE_T`, and the configure probe tests the two
mitigation symbols by linking, not by version.**

**Integration sketch** (revised after review; the lifetimes and the body
path are the parts that were wrong in the first draft).

- `auto/nghttp2`: `--nghttp2` option, `NXT_HAVE_NGHTTP2`, link tests for
  `nghttp2_session_server_new2`, `nghttp2_option_set_stream_reset_rate_limit`
  and `nghttp2_option_set_max_continuations` (the floor); `auto/sources`
  adds `src/nxt_h2proto.c`. ALPN needs OpenSSL 1.0.2+, so the 1.1.1 floor
  holds.
- `nxt_openssl_server_init()`: when the listener enables h2, call
  `SSL_CTX_set_alpn_select_cb(ctx, nxt_openssl_alpn_select, conf)` on
  **every** bundle context in the existing per-bundle loop, with the list
  `\x02h2\x08http/1.1` (spike: `alpn_select_cb`). No assumption about the
  order of the servername and ALPN callbacks is then needed. One more
  `nxt_tls_conf_t` bit, set from a listener option (`"http2": true` inside
  the `tls` object, validated in `nxt_conf_validation.c:640`
  `nxt_conf_vldt_tls_members`).
- `nxt_h1p_conn_proto_init()` (`src/nxt_h1proto.c:465`) becomes the
  protocol chooser: under `#if (NXT_HAVE_NGHTTP2)`, if `c->u.tls` and
  `SSL_get0_alpn_selected()` says `h2`, call `nxt_h2p_conn_init(task, c)`
  instead of allocating an `nxt_h1proto_t`. That is the only h1 line that
  changes. Plain-text h2c (`Upgrade: h2c` / prior knowledge) is out of
  scope: browsers do not use it and it would drag `Upgrade` handling into
  h1.
- **Two objects, two lifetimes.** `nxt_h2proto_t` (per connection,
  `c->socket.data`, in `c->mem_pool`): the `nghttp2_session`, the
  `nxt_conn_t`, `stream_count`, `requests_total`, a `goaway_sent` bit, the
  output staging buffer. `nxt_h2p_stream_t` (per stream, **allocated from
  `c->mem_pool`, not from the request pool**): `h2c`, `stream_id`, `r`
  (may be NULL), the response buffer chain and tail, `deferred`,
  `body_bytes_sent`. The request's `close` hook sets `stream->r = NULL`
  and `nghttp2_session_set_stream_user_data(session, id, NULL)`; the
  stream object is freed only in `on_stream_close`, which nghttp2 calls
  after the last frame of the stream has been serialised (or on
  `RST_STREAM`/GOAWAY). This is what makes "request done" (the sync
  `nxt_http_buf_last` completing, `src/nxt_http_request.c:1049,1061`) safe
  while nghttp2 still references the stream. `r->proto.h2` is the stream.
- Read side: a connection state `nxt_h2p_read_state` whose
  `io_read_handler` reads into an engine buffer and whose `ready_handler`
  runs `nghttp2_session_mem_recv()` over it, then `nxt_h2p_conn_flush()`.
  The buffer is completed immediately: nghttp2 copies what it needs
  (headers go to HPACK, DATA to the `on_data_chunk_recv` callback, which
  is the only chance to store the bytes: **nghttp2 does not buffer DATA**).
- Write side: `nxt_h2p_conn_flush()` loops `nghttp2_session_mem_send()`
  (one frame per call, at most 16 KiB + 9 with the default
  `SETTINGS_MAX_FRAME_SIZE`) into a memory buffer chained on `c->write`,
  then `nxt_conn_write()`; the existing `nxt_openssl_conn_io_sendbuf()` and
  `WANT_*` handling do the rest. Back-pressure: stop calling `mem_send`
  while `c->write` holds more than two frames; resume from the buffer
  completion handler. One copy of the response bytes into the frame is
  accepted for v1 (`NGHTTP2_DATA_FLAG_NO_COPY` + `send_data_callback` would
  remove it, but it bypasses the `c->write` path and is not worth it yet).
- Stream to request: `on_begin_headers` (request category only) checks
  `requests_total < H2_MAX_REQUESTS` and `!goaway_sent`, allocates the
  stream and calls `nxt_http_request_create()`, sets
  `r->proto.h2 = stream`, `r->protocol = NXT_HTTP_PROTO_H2`,
  `r->remote = c->remote`, `r->tls = 1`, `r->task = c->task`, `r->conf =
  joint; joint->count++` exactly like `src/nxt_h1proto.c:499-548`.
  `on_header` appends an `nxt_http_field_t` per pair through
  `nxt_http_req_field_add()` (`src/nxt_http.h:286`) with name and value
  copied into `r->mem_pool` and `field->hash` computed with
  `nxt_http_field_hash_char()`/`_end()`; a name longer than
  `NXT_HTTP_MAX_FIELD_NAME` is a stream error (431). Pseudo-headers fill
  `r->method`, `r->target` (+ `nxt_http_parse_target`-style split into
  `r->path`/`r->args` and `r->quoted_target`), `r->host`, and are not
  added to the list; `r->version = "HTTP/2.0"`, `r->request_line` is
  synthesised as `METHOD target HTTP/2.0` for `$request_line`. On
  `END_HEADERS` run `nxt_http_fields_process()` with the neutral field
  hash (phase 0) so `Content-Length`, `Cookie`, `Traceparent` etc. land
  where h1 puts them.
- **Header validation, split between nghttp2 and us.** nghttp2's HTTP
  messaging layer stays on (never
  `nghttp2_option_set_no_http_messaging`): it already rejects duplicate,
  misordered or unknown pseudo-headers, upper-case names, `connection`,
  `keep-alive`, `proxy-connection`, `transfer-encoding`, `upgrade`, a `te`
  other than `trailers`, and a `content-length` that does not match the
  DATA received, with `RST_STREAM(PROTOCOL_ERROR)`; we log them from
  `on_invalid_header_callback`. Ours: `:path` non-empty and starting with
  `/` (or `*` for OPTIONS); `:authority` validated with
  `nxt_http_validate_host()` (`src/nxt_http_request.c:112`, today a
  `static` behind the `Host` callback `nxt_http_request_host()`, `:87`,
  exported in phase 0) and stored in `r->host`. A `host` field must be
  intercepted in `on_header` because `nxt_http_request_host()` answers 400
  to any second host (`:96-98`): with `:authority` present, an equal
  `host` is dropped (not added to the list), a different one is
  `RST_STREAM(PROTOCOL_ERROR)` (RFC 9113 §8.3.1); without `:authority`
  the `host` field goes through the neutral hash and sets `r->host` as in
  h1. `:scheme` is recorded but `scheme` routing keeps using `r->tls`;
  plain `CONNECT` (no `:scheme`/`:path`) → 405, there is no tunnel.
- **Body and flow control.** Because DATA can arrive before, or without,
  the application handler ever calling `body_read` (`share`, `return` and
  `proxy` never do; `nxt_http_request_read_body`,
  `src/nxt_http_request.c:680`, runs after routing), the body buffer is
  allocated at `END_HEADERS` through the neutral allocator (phase 0), sized
  by `content-length` when present and otherwise growing into a temp file
  like the chunked path, with `max_body_size` enforced in
  `on_data_chunk_recv`, which copies every chunk into `r->body` at once
  (memory up to `body_buffer_size`, then the temp file, exactly h1's
  bound). Since the bytes never wait in our memory, nghttp2's **automatic
  window update stays on**: the windows are throughput knobs, not memory
  bounds. `SETTINGS_INITIAL_WINDOW_SIZE` = 256 KiB (listener option
  `http2.stream_window`) and the connection window 1 MiB
  (`nghttp2_session_set_local_window_size()` on stream 0) give an upload
  a sane bandwidth-delay product; memory per connection is bounded by
  `MAX_CONCURRENT_STREAMS × body_buffer_size` plus the engine read buffer,
  disk by `max_body_size` per stream as for h1. The review's first draft
  had manual window updates with two consume points; they add code and
  bound nothing extra, so they are out (the spike still exercises that
  mode, §8.1, should a temp-file write ever need pacing). `body_read`
  (`nxt_h2p_request_body_read`) only registers interest: if `END_STREAM`
  already arrived it queues `r->state->ready_handler`, else it sets a flag
  that `on_frame_recv(END_STREAM)` acts on. The whole body is buffered
  before the application sees it, which is how h1 works today
  (`prepare_msg` copies `r->body`). For a stream whose action never reads
  the body nothing changes: DATA after the response has been submitted is
  dropped, and nghttp2 is expected to send `RST_STREAM(NO_ERROR)` itself
  when our `END_STREAM` goes out before the client's (a phase-1 test
  checks this; if it does not, `close` submits it). Without a
  `content-length`, set `r->chunked = 1` so
  `nxt_http_request_chunked_transform()` synthesises `Content-Length` from
  the buffer (after the `chunked_field` NULL guard).
- Response: `header_send` maps `r->status` and every `r->resp` field to
  `nghttp2_nv[]` (lower-case names; drop `Connection`, `Keep-Alive`,
  `Transfer-Encoding`, `Upgrade`, which `set_headers` may have added; RFC
  9113 §8.2.2) and calls `nghttp2_submit_response()` with a data provider
  whose source is the stream. Then, exactly as h1 does (`:1575`), queue
  `body_handler` on the fast work queue. `send` appends `out` to the
  stream's chain and calls `nghttp2_session_resume_data()` if the provider
  is deferred, then `nxt_h2p_conn_flush()`. The provider's `read_callback`
  copies from the head buffer, and when a buffer is drained calls
  `nxt_sendbuf_drain()`-style completion (so static file refills and
  `nxt_http_request_mem_buf_completion` run), returns `NGHTTP2_ERR_DEFERRED`
  when the chain is empty and `EOF` when the sync "last" buffer
  (`nxt_http_buf_last`) is reached; completing that sync buffer ends the
  request (`nxt_http_request_done`, `src/nxt_http_request.c:1061`) and,
  through `close`, detaches `r` from the stream as described above.
  `body_bytes_sent` is the stream counter. `discard` drops the chain and
  submits `RST_STREAM(INTERNAL_ERROR)` if headers went out, else a
  `nghttp2_submit_response` with the error status the caller set.
  `close` releases the joint, detaches the stream, decrements
  `stream_count`, and, when it reaches zero, either shuts the connection
  down (`goaway_sent`, or `c->listen->draining`, or `joint == NULL`) or
  puts it on `nxt_conn_idle()` with `idle_timeout` armed, like h1.
- **Errors, two directions.** Stream → request: `on_stream_close` for a
  stream whose `r` is still set (client `RST_STREAM`, or GOAWAY from the
  client) sets `r->error` and calls `nxt_http_request_error_handler(task,
  r, stream)`, which is the same path an h1 connection error takes
  (`nxt_h1p_conn_request_error`, `src/nxt_h1proto.c:1804`); the request's
  own `close` then detaches it. Connection → streams: a TLS/socket error,
  `send_timeout`, or a fatal `nghttp2_session_mem_recv()` result calls
  `nghttp2_session_terminate_session()` and walks the stream list with the
  same error handler; the `nxt_conn_t` and `nxt_h2proto_t` are released
  only when `stream_count == 0` (the app path may still deliver a response
  to a request whose stream is gone; its `send` becomes a no-op and its
  `close` completes the count). Request RPC data (`nxt_request_rpc_data_t`,
  `src/nxt_router.c:7549-7604`) is per request and needs no change.
- **GOAWAY and graceful reload.** When the connection sees
  `c->listen->draining` or `joint == NULL` (checked at idle, like h1, and
  in `on_begin_headers`), or on `H2_MAX_REQUESTS`, it sends
  `nghttp2_submit_shutdown_notice()` (GOAWAY with `last_stream_id`
  2^31 − 1, so the client opens no new streams), sets `goaway_sent`, and
  when the in-flight streams finish, or `send_timeout` later, sends
  `nghttp2_submit_goaway()` with the real `last_stream_id` and shuts down.
  Process shutdown reaches idle h2 connections through
  `engine->idle_connections` as for h1. A client GOAWAY is handled by
  nghttp2 (no new streams accepted); the connection closes when
  `stream_count` reaches zero.
- WebSocket and CONNECT: out of scope. `ws_frame_start` stays NULL in the
  table (the call site is guarded, `src/nxt_http_request.c:836`),
  `SETTINGS_ENABLE_CONNECT_PROTOCOL` is never advertised (RFC 8441 §3, so
  browsers keep opening WebSockets over h1), `r->websocket_handshake`
  stays 0, plain CONNECT is answered 405.
- Timeouts, all `nxt_timer_t` on the connection with the default bias, no
  per-stream timers: `header_read_timeout` is a *progress* timer, armed
  while any stream has an incomplete request and reset by every frame
  that advances one of them (`body_read_timeout` semantics fold into it);
  `send_timeout` on the connection write; `idle_timeout` when no stream
  exists. Process shutdown closes an idle h2 connection with
  `nxt_conn_close()` directly (`src/nxt_runtime.c:507-508`), so the idle
  state's `close_handler` must delete the nghttp2 session.
- `listen_threads` and `SO_REUSEPORT` are unaffected: h2 rides TCP accept.

**Spike result** (§8.1): 810 lines of C against the distro nghttp2 1.59 and
OpenSSL 3.0.13; curl (four parallel streams, 100 KB and 5 MB POSTs under
manual flow control), nghttp, h2load 2000/2000 and 1000/1000 above the
advertised concurrency, ALPN `http/1.1` refused, a 70 KB header refused.
The response body is produced asynchronously from a timer through a
deferred data provider, which is the shape `nxt_h2p_request_send()` will
have. Nothing about nghttp2 fought the edge-triggered epoll model.

**Pros**: smallest delta; no new listener type; distro packages everywhere;
HPACK, flow control, h2spec conformance, the messaging checks and the
rapid-reset/CONTINUATION mitigations are nghttp2's problem; the same
`nxt_conn_t`, TLS, timers and accounting as h1.
**Cons**: one more dependency on the h1+TLS hot path (behind `--nghttp2`);
per-stream requests share one connection so a connection error must fail N
requests; response buffers are copied one more time into nghttp2 frames.

### Option 2. HTTP/3 via OpenSSL's native QUIC server API (analysed, not chosen)

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

**What is and is not available.** The build host has OpenSSL 3.0.13, which
has no QUIC at all (the client API arrived in 3.2, the server API in 3.5);
no Ubuntu 24.04 package provides a newer libssl and the openssl.org and
GitHub hosts are blocked from this container, so **the OpenSSL-native spike
could not be run**; the analysis is from the 3.5 documentation and the
`ossl-nghttp3` demo. CI already builds OpenSSL 3.6.2 and 4.0.2 from source
into `/opt/openssl-3.6` and `/opt/openssl-4.0`
(`.github/workflows/build-test.yml:26-34,106-130`), so a `--h3` leg would
have a QUIC-capable library.

**Event-loop integration model.** In the default single-thread mode the
library owns the UDP socket through a `BIO_s_datagram` and does no I/O
outside application calls: the loop registers the rpoll/wpoll descriptors
in epoll with the interest `SSL_net_read_desired()`/`SSL_net_write_desired()`
report, calls `SSL_handle_events()` when the fd is ready or when
`SSL_get_event_timeout()` fires, and then calls
`SSL_accept_connection()`/`SSL_accept_stream()`/`SSL_read_ex()`/`SSL_write_ex()`
until they return `WANT_READ`. That maps onto one `nxt_fd_event_t` per
listener per engine plus one 0-bias `nxt_timer_t` per listener.
`SSL_DOMAIN_FLAG_THREAD_ASSISTED` spawns an internal thread and must stay
off. `SSL_poll()` is a level-triggered scan, not an event source.

**Multi-engine.** OpenSSL owns the socket, so one `SSL` listener per engine
on its own `SO_REUSEPORT` socket; there is no hook to route a migrated or
retried connection ID to the engine that owns it, and the kernel hashes by
4-tuple. With OpenSSL the application cannot solve that (§7). No GSO.

**TLS config reuse.** A second `SSL_CTX` per bundle from
`OSSL_QUIC_server_method()` with the same certificate, ciphers (TLS 1.3
only), tickets and `conf_commands`; SNI callback unchanged.

**Packaging.** OpenSSL ≥ 3.5 is in Ubuntu 25.10+, Fedora 42+, Debian 13,
Alpine 3.22+; RHEL 8/9 never, RHEL 10 ships 3.5. `--h3` would need
`libnghttp3` 1.x (Debian 13, Fedora, Alpine; Ubuntu 24.04's 0.8 is too old).

**Pros**: no second TLS stack; one extra dependency (`nghttp3`); OpenSSL
maintains the transport. **Cons**: youngest server API; library owns the
socket, foreclosing CID routing across engines and GSO; hard version floor
excluding RHEL 8/9 and everything older than 2025; two stream state
machines to keep in step.

### Option 3. HTTP/3 via ngtcp2 + nghttp3 with an OpenSSL crypto backend (preferred when h3 is taken up)

ngtcp2 is the reference-quality QUIC transport behind curl's `--http3`; it
has no I/O of its own: the application owns the UDP socket, calls
`ngtcp2_conn_read_pkt()` per datagram, `ngtcp2_conn_writev_stream()` to
produce packets (with `ngtcp2_conn_get_send_quantum()` for GSO batches), and
`ngtcp2_conn_get_expiry()`/`ngtcp2_conn_handle_expiry()` for the single
per-connection timer. nghttp3 sits on top with the same callback style as
nghttp2. The crypto backend is pluggable: `ngtcp2_crypto_ossl` for
OpenSSL ≥ 3.5, `ngtcp2_crypto_quictls` for the (discontinued) quictls fork,
BoringSSL, wolfSSL, GnuTLS.

**Event-loop integration model.** Identical to the quiche spike (§8.2): one
`nxt_fd_event_t` on the UDP socket, a connection table keyed by CID hung
off the listen event, `nxt_timer_t` (bias 0) per connection re-armed from
`ngtcp2_conn_get_expiry()`. Everything is synchronous, no threads.

**Streams to requests.** nghttp3's callbacks are the h3 mirror of the
nghttp2 ones, so the `nxt_h2p_stream_t` design carries over: `recv_header`
→ `nxt_http_req_field_add`, `end_headers` → fields process, `recv_data` →
`r->body`, `end_stream` → ready handler; response headers via
`nghttp3_conn_submit_response()` with an `nghttp3_data_reader` that pulls
from the stream's buffer chain; `nghttp3_conn_block_stream()`/`unblock` for
QUIC-level flow control, `nghttp3_conn_add_ack_offset()` when ngtcp2
reports acked stream data (that is when the router buffers may be
completed, not on send).

**UDP listener needs.** New listener type: `nxt_listen_socket_create()`
grows a `SOCK_DGRAM` branch (no `listen()`), `IP_PKTINFO`/
`IPV6_RECVPKTINFO` to learn the local address for `r->local` and for the
`ngtcp2_path`, `SO_REUSEPORT` when there is one socket per engine, and
optionally `UDP_GRO`/`UDP_SEGMENT`. Address validation (Retry tokens,
`ngtcp2_crypto_generate_retry_token`) and stateless reset tokens need a
per-router secret. Per-engine ownership and CID routing: §7.

**TLS config reuse.** With `ngtcp2_crypto_ossl` the `SSL_CTX` is a normal
TLS 1.3 server context created by `nxt_openssl_server_init()` plus
`ngtcp2_crypto_ossl_configure_server_context()`; certificates, SNI callback,
tickets and `conf_commands` work as they do for TCP. That is the best TLS
reuse of the three h3 options.

**Packaging.** Ubuntu 24.04 has `libngtcp2-dev` 0.12.1 and `libnghttp3-dev`
0.8.0 (installed here): both predate the 1.0 API freeze and ship only the
GnuTLS crypto backend, because the distro OpenSSL 3.0 has no QUIC TLS API.
Debian 13 has ngtcp2 1.11 / nghttp3 1.8 with the `quictls` and `ossl`
backends; Fedora 41+ and Alpine 3.20+ have 1.x; RHEL/EPEL none.
Realistically `--h3` builds ngtcp2 + nghttp3 as `pkg/contrib` tarballs the
way CI builds OpenSSL 3.6, and needs OpenSSL ≥ 3.5 for `ngtcp2_crypto_ossl`.
MIT licences.

**Pros**: the model FreeUnit's engine wants; the most complete conformance
story (interop matrix, curl's production stack); CID routing and GSO stay
in our hands; the best TLS reuse; nghttp3's callbacks mirror nghttp2's.
**Cons**: the most code to write (packet dispatch, CID table, Retry,
stateless reset, path validation policy, migration off); three libraries
to vendor on most targets; an OpenSSL ≥ 3.5 floor for `--h3` all the same.

### Option 4. HTTP/3 via quiche (Rust, C FFI) (fallback)

quiche (Cloudflare, BSD-2) bundles QUIC, HTTP/3 and QPACK behind a C API
and vendors BoringSSL for the TLS handshake. The I/O model is the same as
ngtcp2's: the application owns the UDP socket and calls
`quiche_conn_recv()`/`quiche_conn_send()`, `quiche_conn_timeout_as_millis()`
/`quiche_conn_on_timeout()`, and `quiche_h3_conn_poll()` for HTTP/3 events.

**Spike result** (§8.2): `cargo build --release` of quiche 0.30 with the
`ffi` feature took 2 m 25 s wall on two shared cores (BoringSSL is built
by cmake inside), producing a 178 MB `libquiche.a` that must be linked
with `-lstdc++`; the 424-line C server driven from epoll served three
concurrent HTTP/3 GETs from an aioquic client (ALPN `h3`, QUIC v1).

**Build and packaging.** FreeUnit already builds a Rust component for OTel
(`src/otel/`, `pkg/deb/Makefile:49` carries `cargo` in the build deps), but
quiche is a different scale: BoringSSL, `cmake`, a C++ toolchain, ~340 MB
of build tree; no distro packages it, so every deb/rpm/Alpine build compiles
it and crates.io becomes a build-time requirement. Two TLS stacks in one
binary double the CVE surface; `conf_commands`, `tickets` and the SNI
bundle callback would be reimplemented on quiche's per-SNI config
selection.

**Pros**: everything in one library, the smoothest API, proven at
Cloudflare's scale, spike worked immediately.
**Cons**: a second TLS stack; heavy Rust/C++ build; nothing from distros;
SNI/tickets/conf_commands reimplemented; version churn (no stable 1.0).

### Option 5. lsquic, msquic, picoquic (briefly)

msquic owns its worker threads (callbacks arrive on its threads); lsquic's
engine is process-wide with engine-level timers and needs BoringSSL;
picoquic is research-grade with little distro presence. None is attractive
against options 3 and 4.

## 7. Cross-cutting: QUIC and `listen_threads`

FreeUnit runs `listen_threads` engine threads, each with its own epoll and
its own `nxt_listen_event_t` on the shared TCP socket. For QUIC three layouts
are possible:

1. **One UDP socket, one owner engine.** Simplest and correct: all QUIC
   traffic for a listener is handled by engine 0; requests are still
   created there and dispatched to application ports through the normal
   per-engine port (`engine->port`). Other engines keep serving h1/h2.
   This would be the first h3 shape.
2. **One socket per engine with `SO_REUSEPORT` and eBPF steering.** Linux
   hashes the 4-tuple, so connection migration or NAT rebinding sends a
   known connection to the wrong engine. nginx solves it with an
   `SO_ATTACH_REUSEPORT_EBPF` program that maps a worker id encoded in the
   server-chosen CID to the socket index (`ngx_event_quic_bpf.c`). Doable
   later: ngtcp2 and quiche let us choose CIDs, OpenSSL does not expose
   enough.
3. **One socket, packets forwarded between engines.** The reading engine
   looks up the CID, and if the connection belongs to another engine posts
   the datagram with `nxt_event_engine_post()` (the locked work queue,
   `src/nxt_event_engine.c:236`). A copy per packet; fine for migration
   handling in layout 2, not as the main path.

All three keep the library inside one engine thread at a time; a QUIC
connection never migrates between engines.

## 8. Spikes

The spike code is in `docs/adr/0005-spikes/` with its README (build and
test commands). Neither touches `src/`. Results below are from the
re-run of 2026-09-24 after the review fixes (`0005-REVIEW.md`, R1-21 to
R1-24), on Ubuntu 24.04 in `unshare -n`.

### 8.1 `h2_epoll_server.c` (nghttp2 + OpenSSL ALPN, epoll)

- Built with the distro `libnghttp2-dev` 1.59.0-1ubuntu0.4 and OpenSSL
  3.0.13, `-Wall -Wextra` clean.
- Sets the §Security requirements values: `stream_reset_rate_limit(1000,
  33)`, `max_continuations(8)`, `max_settings(32)`,
  `MAX_CONCURRENT_STREAMS` 128, stream window 256 KiB, connection window
  512 KiB, `MAX_HEADER_LIST_SIZE` 32 KiB; the build proves the 1.59 header
  carries the backported symbols. It also runs with
  `no_auto_window_update` and explicit consume calls, the manual mode the
  design ended up not needing (Option 1, body paragraph); it is kept in the
  spike because it is the harder mode and shows that the stream window can
  be released from an asynchronous point without stalling the connection.
- ALPN via `SSL_CTX_set_alpn_select_cb()` selects `h2`;
  `SSL_get0_alpn_selected()` after the handshake decides the protocol; an
  `http/1.1`-only client is closed at once.
- `curl --http2` got `HTTP 2 200`; `curl --parallel` with four URLs ran four
  streams on one connection; a 100 KB and a 5 MB `--data-binary` POST
  arrived complete (`up=5000000`) with the stream window released only
  from the 100 ms timer tick (the router's "moved into `r->body`"
  moment), i.e. manual flow control; `nghttp -nv` showed SETTINGS, the
  stream-0 WINDOW_UPDATE, HEADERS and DATA with END_STREAM.
- The body is produced asynchronously by the timer through
  `NGHTTP2_ERR_DEFERRED` + `nghttp2_session_resume_data()`, which is the
  `nxt_h2p_request_send()` model. `h2load -n 2000 -c 8 -m 16` completed
  2000/2000 (416 req/s is the spike's body timer, not a throughput
  number); `h2load -c 2 -m 200` above the advertised 128 streams completed
  1000/1000 (the client honours the setting).
- A 70 KB request header was refused (nghttp2's 64 KiB inbound header
  block cap), connection closed, server unaffected.

### 8.2 `h3_epoll_server.c` (quiche 0.30 FFI, epoll, UDP)

- Rebuilt from the cached Cargo build (32 MB binary), `-Wextra` clean
  after checking `getrandom()` and the `quiche_h3_send_*` return values.
- One non-blocking UDP socket in epoll, `quiche_header_info()` → CID
  lookup → `quiche_accept()`/`quiche_conn_recv()`; `quiche_conn_send()` loop
  → `sendto()`; per-connection `quiche_conn_timeout_as_millis()` folded
  into `epoll_wait()`; `quiche_conn_on_timeout()` and closed-connection
  reaping after every poll.
- Test client: `h3_client.py` on aioquic 1.3.0. Three GETs on streams 0, 4,
  8 returned `:status 200` with bodies; the connection closed cleanly
  (`recv=10 sent=10 lost=0`).
- Not done: Retry/address validation, stateless reset, multiple sockets,
  GSO, stream-writable resumption. All are application-side with quiche and
  ngtcp2.

### 8.3 Not run

The OpenSSL 3.5 native QUIC server could not be built here; the ngtcp2 0.12
packages on this host predate the 1.0 API and only have the GnuTLS backend,
so a spike would not have represented `ngtcp2_crypto_ossl` and was skipped
in favour of the quiche one, whose loop model is the same. h2spec could not
be installed either (its current module needs Go 1.26; the release binary
is on GitHub): the conformance run is a CI task, see phase 1.

## Decision outcome

**h2: option 1, nghttp2 over the existing TLS connections, after a phase 0
that generalises four things in the protocol layer.** It is the smallest
change, uses distro packages on every target, keeps the OpenSSL floor, and
the spike confirmed the integration model end to end, including the
mitigation settings and flow control in the harder, manual mode.

**h3: deferred.** The analysis stands and is kept here so it is not redone:
when h3 is taken up, option 3 (ngtcp2 + nghttp3 with `ngtcp2_crypto_ossl`,
optional `--h3` build requiring OpenSSL ≥ 3.5) is preferred because it keeps
one TLS stack, reuses the cert store, SNI, tickets and `conf_commands`,
keeps the UDP socket and the timers in FreeUnit's engine (which is what
makes CID routing across `listen_threads` and GSO possible), and nghttp3's
callbacks mirror nghttp2's so the phase-1 stream-to-request code is reused.
quiche (option 4) is the fallback if vendoring ngtcp2 proves too heavy.
OpenSSL's own QUIC server (option 2) is re-evaluated when 3.5+ is the floor
on every supported OS or if it gains an application-driven datagram path.
The trigger for taking h3 up is a product need, not the completion of h2;
the requirements it must meet are listed under "Deferred: HTTP/3" so they
are not rediscovered.

### Consequences

- A per-request protocol vtable already exists (`nxt_http_proto[]`); we
  fill the H2 entry and change four things in `nxt_h1proto.c`/
  `nxt_http_request.c` so they are shareable (phase 0). No h1 behaviour
  changes.
- Workers see `HTTP/2.0` in `SERVER_PROTOCOL`; every other field of
  `nxt_unit_request_t` is filled the same way. Header names arrive
  lower-case, which they may already (h1 clients do send lower-case), and
  `prepare_msg` upper-cases them for CGI names anyway
  (`src/nxt_router.c:7954-7965`).
- The access log `$response_header_connection` and
  `$response_header_transfer_encoding` are empty for h2, which is correct
  (the fields do not exist on the wire).
- Two new parsers (HPACK inside nghttp2, our pseudo-header/field mapping)
  run before routing: a fuzz target feeds `nghttp2_session_mem_recv()`
  through the real callbacks (phase 1).
- One new build and package dependency, `libnghttp2`, behind `--nghttp2`.

## Security requirements (acceptance criteria for phase 1)

Every row is a test in `test/test_http2.py` or the fuzz target, and a
value in `src/nxt_h2proto.c` (listener options only where marked).

| Threat | Requirement | Where it is enforced |
|---|---|---|
| Rapid Reset (CVE-2023-44487): open+RST streams faster than the server can cancel work | `nghttp2_option_set_stream_reset_rate_limit(opt, 1000, 33)` (burst, per second; nghttp2 answers exhaustion with GOAWAY). Configure floor: the symbol must link. Plus `requests_total` per connection ≤ `H2_MAX_REQUESTS` (1000) → GOAWAY; the counter includes streams that were reset. | nghttp2 (≥ 1.57 semantics); `on_begin_headers` |
| Concurrency: many open streams each holding a router request and a body buffer | `SETTINGS_MAX_CONCURRENT_STREAMS` = 128 (listener option `http2.max_concurrent_streams`, 1..1024); nghttp2 refuses excess streams before the SETTINGS ACK too (`pending_local_max_concurrent_stream`). In-flight body memory per connection ≤ 128 × `body_buffer_size`; temp files only after that per stream. | nghttp2; body allocator |
| CONTINUATION flood (CVE-2024-28182): endless header block without END_HEADERS | `nghttp2_option_set_max_continuations(opt, 8)`; symbol must link. | nghttp2 |
| HPACK bomb / oversized header lists | Advertise `SETTINGS_MAX_HEADER_LIST_SIZE` = `large_header_buffer_size × large_header_buffers` (32 KiB default); nghttp2's 64 KiB inbound header block cap stays; `SETTINGS_HEADER_TABLE_SIZE` left at 4096 and `nghttp2_option_set_max_deflate_dynamic_table_size(opt, 4096)` for our encoder. A name longer than `NXT_HTTP_MAX_FIELD_NAME` (255) or a total over the list size is a 431 on the stream. `prepare_msg` still refuses the prefixed-name and method overflows (431/501). | `on_header`; nghttp2; router |
| SETTINGS / PING floods (unbounded ACK queue) | `nghttp2_option_set_max_settings(opt, 32)` and the default `nghttp2_option_set_max_outbound_ack` (1000): nghttp2 closes the session beyond them; we never disable them. | nghttp2 |
| Empty DATA / WINDOW_UPDATE / PRIORITY floods | nghttp2 allocates nothing per such frame and our callbacks do no work for them (`on_frame_recv` ignores PRIORITY and empty DATA); the progress timer (`header_read_timeout`) closes a connection whose requests do not advance. No stronger claim is made; the h2load/fuzz runs in 1.6 watch CPU per connection. | nghttp2; timers |
| Slow read / stalled body | Automatic window updates; stream window 256 KiB, connection window 1 MiB; bytes are copied out of nghttp2 at once so memory per connection ≤ streams × `body_buffer_size`; `header_read_timeout` as a progress timer while a request is incomplete; `send_timeout` on the connection write; `idle_timeout` with no streams. | `nxt_h2proto.c` timers, body allocator |
| Pseudo-header and framing abuse | nghttp2 HTTP messaging validation on; ours: `:path`, `:authority`/`host` agreement, CONNECT → 405, name cap. | nghttp2; `on_header`/`on_frame_recv` |
| TLS downgrade of h2 (RFC 9113 §9.2) | TLS ≥ 1.2, no renegotiation and no compression already set (`src/nxt_openssl.c:239-254`); the §9.2.2 cipher black list is not enforced (nginx does not either); ALPN only, no `Upgrade: h2c`. | existing TLS init |
| Resource leak on connection error | Every stream's request fails through `nxt_http_request_error_handler`; the connection is released at `stream_count == 0`; ASan run of the error matrix in CI. | `nxt_h2proto.c` |

## 9. Plan

### Phase 0: generalise the protocol layer (no behaviour change)

Four changes, all in files h1 owns; each is small enough to review as one
commit.

| # | Change | Where |
|---|---|---|
| 0.1 | Build a second hash over the neutral tail of `nxt_h1p_fields[]` (`&nxt_h1p_fields[5]`: seven entries, nine with OTel) in `nxt_h1p_init()`, exported as `nxt_http_request_fields_hash`; the array and the h1 hash are untouched. Drop the `static` from `nxt_http_validate_host()` and declare it, so `:authority` can call it. | `src/nxt_h1proto.c:172-220`, `src/nxt_http_request.c:112`, `src/nxt_http.h:430-458` |
| 0.2 | Factor `nxt_http_request_body_alloc(task, r, body_length)` (memory buffer vs temp file, `max_body_size` check) out of `nxt_h1p_request_body_read()`; h1 calls it. | `src/nxt_h1proto.c:967-1030`, new function in `src/nxt_http_request.c` |
| 0.3 | Guard `r->chunked_field` in `nxt_http_request_chunked_transform()`. | `src/nxt_http_request.c:557-586` |
| 0.4 | Add `nxt_h2p_stream_t *h2` to the `r->proto` union (forward-declared type). | `src/nxt_http.h:90-93` |

Tests: the existing pytest suite stays green (`test/test_chunked.py`,
`test/test_tls*.py`, `test/test_access_log.py`, `test/test_otel*.py`);
one `src/test/` unit test that looks up each neutral name through the new
hash and asserts the h1 hash still resolves `Connection`. The fuzz harness
`fuzzing/nxt_http_h1p_fuzz.c` builds unchanged.

Effort: 1–2 engineer-days.

Not in phase 0 (dropped after review): the H3 enum and table resizing, the
status-text tables, the ALPN dispatcher (that is phase 1 code).

### Phase 1: HTTP/2 via nghttp2 over TLS

| # | Task | Where | Test |
|---|---|---|---|
| 1.1 | `--nghttp2` option, `auto/nghttp2` probe (link tests for `nghttp2_session_server_new2`, `nghttp2_option_set_stream_reset_rate_limit`, `nghttp2_option_set_max_continuations`), `NXT_HAVE_NGHTTP2`, `src/nxt_h2proto.c` in sources, summary/help lines. | `auto/options`, `auto/nghttp2`, `auto/sources`, `auto/summary`, `auto/help` | configure with and without the library; the probe result on Ubuntu 22.04's 1.43 (its security backports may or may not carry the two symbols) is recorded by a CI leg, and either outcome is acceptable as long as it is clean |
| 1.2 | Listener option `"tls": {"http2": true}` (+ optional `max_concurrent_streams`), validation, OpenAPI. | `src/nxt_conf_validation.c:640`, `src/nxt_router.c:3128,3274`, `docs/unit-openapi.yaml` | `test/test_http2.py::test_config_*` (rejects without `tls`) |
| 1.3 | `nxt_tls_conf_t::alpn_h2` bit; `SSL_CTX_set_alpn_select_cb()` on every bundle context; selection after the handshake in `nxt_h1p_conn_proto_init()` under `#if (NXT_HAVE_NGHTTP2)`. | `src/nxt_tls.h:64-82`, `src/nxt_openssl.c:217-370`, `src/nxt_h1proto.c:465-484` | `curl --http1.1` and `--http2` on the same listener; SNI bundles both negotiate h2 (`test_tls_sni.py` parametrised) |
| 1.4 | `src/nxt_h2proto.c` / `.h`: connection init with the §Security options and settings, read state, flush with back-pressure, the nghttp2 callbacks, the seven request hooks, stream/request lifetimes, timers, GOAWAY, error matrix. | new files; table entry `src/nxt_h1proto.c:136-167` | below |
| 1.5 | `$request_line` for h2; `body_bytes_sent`; `$response_header_*` untouched. | `src/nxt_h2proto.c`, `src/nxt_http_variables.c:431-454` | `test_access_log.py` parametrised over h2 |
| 1.6 | CI: `apt-get install libnghttp2-dev nghttp2-client`, an `h2spec` step (release binary, `h2spec -t -k -h 127.0.0.1 -p PORT --strict`, allow-list file in `test/h2spec-allow.txt` for the known generic cases), `h2load` smoke, ASan leg of the error matrix. deb/rpm build deps `libnghttp2-dev` / `libnghttp2-devel`. | `.github/workflows/build-test.yml:88`, `pkg/deb/debian/control.in:7`, `pkg/rpm/unit.spec.in:8-10` | the workflow |
| 1.7 | `fuzzing/nxt_http_h2p_fuzz.c`: like `nxt_http_h1p_fuzz.c`, includes `nxt_h2proto.c`, builds a session with the real callbacks and a fake `nxt_conn_t`, feeds the input to `nghttp2_session_mem_recv()` after the client preface, drains `mem_send()` into a sink; seed corpus (`fuzzing/fuzz_http_h2p_seed_corpus/`) generated once with the Python `h2` package: preface + SETTINGS, a GET, a POST with DATA, a CONTINUATION split, an RST_STREAM. | `fuzzing/`, `fuzzing/build-fuzz.sh` | runs in `fuzzing/run-ci.sh` |

Tests for 1.4, `test/test_http2.py` on the existing TLS helpers
(`test/unit/applications/tls.py` certificates, raw sockets via
`ssl.wrap_socket`) with the pure-Python `h2` package added to
`test/requirements.txt` (no httpx):

- routing, `share` with `fallback`, `return`, `proxy` to an h1 upstream,
  compression, access log, OTel `traceparent` propagation, each once over
  h2 (parametrise the existing tests where the client is a helper);
- pseudo-headers: missing `:path`, `:path` `""`, `:authority` vs `host`
  mismatch, CONNECT, `te: gzip`, upper-case name, a 300-byte name (431), a
  method of 256 bytes (501), header list over 32 KiB (431);
- body: `content-length` present/absent, over `max_body_size` (413), DATA
  before `body_read` on a `share` route, body larger than
  `body_buffer_size` (temp file), a response completed before the request
  body (client observes `RST_STREAM(NO_ERROR)`), a client that stops
  sending mid-body (progress timeout closes it, other connections
  unaffected), a slow client on one stream while another stream on the
  same connection is served;
- error matrix: client RST mid-body, client RST after headers sent, client
  GOAWAY with streams in flight, app crash with N streams, TLS close with
  N streams, reconfigure with streams in flight (GOAWAY, all complete,
  old joint released), `requests_total` cap;
- limits: `max_concurrent_streams` + 1 (REFUSED_STREAM), 1100 RST_STREAM
  in a burst (GOAWAY), 9 CONTINUATION frames (GOAWAY), 40 SETTINGS
  frames without ACK (GOAWAY);
- h2spec strict run with the allow-list.

Effort: 12–18 engineer-days, of which the error matrix and h2spec are half.

### Deferred: HTTP/3

Kept as a checklist so the h3 decision can be taken later without repeating
the analysis. None of it is scheduled.

- `--h3` build: OpenSSL ≥ 3.5, ngtcp2 + `ngtcp2_crypto_ossl` + nghttp3 as
  `pkg/contrib` tarballs (Debian 13/Fedora packages where they exist); a
  CI leg on `/opt/openssl-3.6`.
- UDP listener (`SOCK_DGRAM` branch in `nxt_listen_socket_create()`,
  `IP_PKTINFO`, no `listen()`), the main-process socket RPC carrying a
  second fd for the same listener entry, `Alt-Svc` on the paired TCP
  listener.
- Layout 1 of §7 first (engine 0 owns the socket); `SO_REUSEPORT` + eBPF
  steering and GSO/GRO later.
- Security requirements: Retry with a keyed, expiring token under load
  (amplification stays ≤ 3× until validation, enforced by ngtcp2 but only
  meaningful with Retry on); stateless reset tokens from a per-router
  secret; `disable_active_migration` until steering exists; 0-RTT off, and
  when enabled only for idempotent methods with `Early-Data: 1` passed to
  the app; per-source connection and stream caps; a fuzz target on
  `ngtcp2_conn_read_pkt()`.
- Tests: aioquic client (as in the spike), `curl --http3` where available,
  the ngtcp2 example client in CI, the QUIC interop runner when a second
  stack is on hand.

Effort when taken up: 25–40 engineer-days.

## Pros and cons of the options

| | Loop model | Threads | Deps (Ubuntu 24.04 / Debian 13 / RHEL) | TLS reuse | Effort |
|---|---|---|---|---|---|
| 1. nghttp2 (h2) | bytes in/out on existing `nxt_conn_t` | none | pkg (1.59, classic API) / pkg / pkg | full | 12–18 d |
| 2. OpenSSL QUIC + nghttp3 | lib owns UDP BIO; rpoll/wpoll fds + `SSL_get_event_timeout` | none if thread-assisted mode is off | needs OpenSSL ≥ 3.5: none / pkg / RHEL 10 only; nghttp3 1.x: none / pkg / none | full (second `SSL_CTX` per bundle) | 20–30 d |
| 3. ngtcp2 + nghttp3 (ossl) | app owns UDP socket and timers | none | 0.12 (unusable) / pkg 1.x / none; OpenSSL ≥ 3.5 | full (same `SSL_CTX`) | 25–40 d |
| 4. quiche | app owns UDP socket and timers | none | none / none / none; cargo + cmake + C++ | second TLS stack; SNI/tickets reimplemented | 20–30 d |
| 5. msquic / lsquic / picoquic | own threads / process-wide engine / research | msquic yes | none | partial | n/a |

## Top risks

1. **Multiplexed error semantics** (one connection, many requests) are new
   to the router: the two-lifetime design in Option 1 is the mitigation,
   and the phase-1 error matrix under ASan is the evidence. Review time
   goes there first.
2. **Distro nghttp2 skew**: versions from 1.43 to 1.66 with backports that
   keep the version number. Mitigation: probe by symbol; classic API;
   the CI matrix builds on Ubuntu 22.04 and 24.04 packages.
3. **`nxt_router.c` churn** from the IPC/status/schedules streams: the
   listener, TLS-insert and `prepare_msg` line numbers above will move.
4. **h2spec allow-list creep**: generic failures (mostly around
   `MAX_CONCURRENT_STREAMS` timing and idle-state RST) must be justified
   one by one in `test/h2spec-allow.txt`.
5. **OpenSSL ≥ 3.5 as the h3 floor** leaves RHEL 8/9, Ubuntu 22.04/24.04 and
   Debian 12 without h3 unless we ship OpenSSL ourselves. That is why h3
   is deferred rather than phased.

## Open questions

- Should h2 be on by default whenever a listener has `tls` (as Caddy does,
  nginx does not)? Proposal: off by default in phase 1, default on once
  h2spec is in CI and one release has shipped with it optional.
- Do we want plaintext h2c for internal proxies (`nxt_http_proxy.c` peers)?
  Not in this plan; the peer protocol field makes it possible later.
- `http2.stream_window` (256 KiB) and the connection window (1 MiB) are
  proposed defaults from the spike; the h2load upload numbers in 1.6
  decide whether they need to be listener options at all in the first
  release.
- h3 listener JSON shape (`"listeners": {"*:443": {"http3": true}}`
  creating a TCP and a UDP socket) and the two-fd socket RPC: decide when
  h3 is taken up.
