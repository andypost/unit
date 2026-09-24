# Review of ADR 0005 (HTTP/2 and HTTP/3) and its spikes

Adversarial review of `docs/adr/0005-http2-http3.md` and
`docs/adr/0005-spikes/`, done as a senior protocol/security engineer
(nghttp2/ngtcp2/QUIC) plus a FreeUnit core maintainer. Line numbers in the
findings refer to the tree at the tip of `proto/h2h3-review` (based on
`ddf19bc3`); the ADR's own line references were verified against that
tree, see R1-01.

Severity: **H** (the design or a claim is wrong and would ship a bug or a
security hole), **M** (a gap that must be closed before phase 1 merges),
**L** (accuracy, scope or code hygiene).

Each finding: severity / finding / location / fix. Findings are numbered
per round; "Status" says what the improve step of that round did.

---

## Round 1

### Stale references and wrong claims

**R1-01 · L · Stale `file:line` references.** The ADR pins every line to
`b3bec257`; `src/nxt_router.c` moved by +70..+110 lines and several other
files by a few lines. Verified against the current tree:

| ADR says | Now |
|---|---|
| `nxt_router.c:7627` `nxt_router_prepare_msg` | `:7737` |
| `nxt_router.c:3815` `nxt_router_listen_socket_rpc_create` | `:3887` |
| `nxt_router.c:4773-4803` `nxt_router_listen_socket_create` | `:4845` |
| `nxt_router.c:2227` `listen_threads`; `:2629` `nxt_ncpu` | `:2299`; `:2702` |
| `nxt_router.c:4293-4363` `nxt_router_engines_create` | `:4365` |
| `nxt_router.c:3065` `listen->handler = nxt_http_conn_init` | `:3137` |
| `nxt_router.c:3005-3060` `nxt_router_conf_tls_insert` caller | `:3128` (function `:3274`) |
| `nxt_router.c:7520` `r->proto.any`; `:7428-7470` rpc data | `:7604`; `:7549-7604` |
| `nxt_router.c:7790-7806` CGI upper-casing | `:7954-7965` (`http_prefix` `:426`) |
| `nxt_http_request.c:819/1088/1060/1071` | `:820/:1089/:1061/:1072` |
| `nxt_http_route.c:1665`; `:482-490` scheme | `:1666`; `:2045-2053` |
| `nxt_http_variables.c:530-534` | `:531` |
| `nxt_openssl.c:1158/1207/1235/1324/1370/1531` | `:1159/:1205/:1236/:1330/:1381/:1542` |
| `nxt_listen_socket.c:50-180`, `:158` listen | `:30-160`, `:151` |
| `nxt_listen_socket.h:11-31` | `:11-35` |
| `nxt_h1proto.c:530` `joint->count++` | `:546` |
| `nxt_conn_write.c:15-130`, `:193-260` | `:17-130`, `:202-260` |
| `nxt_event_engine.c:506`, `:235` | `:507`, `:236` |
| `nxt_http_static.c:1795-1875` | `:1800-1875` |
| `nxt_http_parse.c` (not cited) | `NXT_HTTP_MAX_FIELD_NAME` is `:28` (0xFF) |

Unchanged and confirmed: `nxt_http.h:68-72,90-93,285-300,374-393,430-458`,
`nxt_h1proto.c:136-167,172-196,229,280,326,399-411,465-484,906,1055,1189,
1383,1575,1658,1765,1779,1804,1905,1917,2012`, `nxt_php_sapi.c:1553`,
`nxt_python_wsgi.c:661`, `nxt_http_request.c:327,600,557,646-668,673,682,
689-826,836,1004,1142`, `nxt_http_variables.c:445,536,606`, `nxt_openssl.c:
217,1028,1096`, `nxt_conn.h`, `nxt_epoll_engine.c:121,361-367,571-584`,
`nxt_timer.*`, `nxt_sendbuf.c:397`, `nxt_conn_accept.c:37-75,94`,
`nxt_conf_validation.c:587`, `auto/*`, `pkg/*`, CI lines.
*Fix:* rewrite the references; drop the `b3bec257` pin and cite the
review branch. **Status: fixed in R1 improve.**

**R1-02 · L · "The spike code is not kept in the tree" is false.** §8 and
the header say the spikes live only in commit `48cc253d`; they are in
`docs/adr/0005-spikes/` on this branch (`ddf19bc3`). *Fix:* point at the
directory. **Status: fixed.**

**R1-03 · M · `prepare_msg` limits are cited as if they did not exist.**
The ADR never mentions that `nxt_router_prepare_msg()` now refuses a
method longer than 255 bytes with 501 and a field name that overflows the
`uint8_t` `name_length` (with the `HTTP_` prefix) with 431
(`src/nxt_router.c:7715-7830`). This matters for h2: HPACK imposes no
per-name limit, `nxt_http_field_t.name_length` is `uint16_t`
(`nxt_http.h:319`) while the h1 parser caps names at
`NXT_HTTP_MAX_FIELD_NAME` = 255 (`nxt_http_parse.c:28,574,594`). An h2
layer that copies a 60 KB name into a `uint16_t` truncates it. *Fix:*
acceptance criterion: `on_header` rejects `namelen > NXT_HTTP_MAX_FIELD_NAME`
with `RST_STREAM(PROTOCOL_ERROR)` (or 431) and relies on `prepare_msg` for
the prefixed 431/501 cases; `SETTINGS_MAX_HEADER_LIST_SIZE` bounds the
total. **Status: fixed (§Security requirements).**

**R1-04 · M · The nghttp2 `*2` API is not in the distro floor.** The ADR
says "distro packages everywhere" but Ubuntu 24.04 ships 1.59, which has
no `nghttp2_session_mem_send2()` / `nghttp2_data_provider2` /
`nghttp2_submit_response2()` (added in 1.62; `grep -c mem_send2
/usr/include/nghttp2/nghttp2.h` = 0 here). Ubuntu 22.04 has 1.43, Amazon
Linux 2 1.41. *Fix:* code to the `ssize_t` API, never define
`NGHTTP2_NO_SSIZE_T`; probe features by symbol, not by version (Ubuntu
backports `nghttp2_option_set_max_continuations` and
`nghttp2_option_set_stream_reset_rate_limit` into 1.59 without bumping the
version, confirmed in the installed header). Floor: 1.57 semantics (reset
rate limit) via a link test. **Status: fixed (§Dependencies).**

**R1-05 · L · "Per-bundle contexts inherit the ALPN callback because the
servername callback runs first"** is an ordering claim we do not need.
`nxt_openssl_server_init()` creates one `SSL_CTX` per bundle (two
`SSL_CTX_new` sites, `nxt_openssl.c`). *Fix:* set
`SSL_CTX_set_alpn_select_cb()` on every bundle context in that loop; no
ordering assumption. **Status: fixed.**

**R1-06 · L · Phase 0 row "export the response status text tables" is
unnecessary.** `nxt_http_error.c:20-24` renders only the numeric status
(`<title>Error %03d</title>`); h2 sends `:status` as a number. *Fix:* drop
the row. **Status: fixed.**

**R1-07 · L · Phase 0 row "split `nxt_h1p_fields[]` ... h1-only list that
chains to it"** does not match the API: `nxt_http_fields_process()` takes
one `nxt_lvlhsh_t` (`nxt_http_parse.c:1264`), there is no chaining. The
array is already ordered h1-only first (5 entries) then neutral
(`nxt_h1proto.c:172-196`). *Fix:* keep the array, build a second hash
`nxt_http_request_fields_hash` from `&nxt_h1p_fields[5]` in
`nxt_h1p_init()` and export it; h1 is untouched. The hash test is
case-insensitive (`nxt_strcasestr_eq`, `:1161-1170`) so lower-case h2
names match, but the h2 layer must compute `field->hash` with
`nxt_http_field_hash_char()`/`_end()` (`nxt_http_parse.h:120-122`) before
calling `nxt_http_fields_process()`, which the ADR does not say.
**Status: fixed (phase 0 table rewritten).**

### Protocol correctness

**R1-08 · H · Request body: DATA can arrive before `body_read` and nghttp2
does not buffer it.** The ADR allocates `r->body` in the `body_read` hook,
but `nxt_http_request_read_body()` (`nxt_http_request.c:680`) is called
only by the application handler after routing, and never for `share`,
`return` or `proxy`. `on_data_chunk_recv` hands the bytes over once; if
there is no buffer they are lost, and the advertised window then lets the
client push up to `body_buffer_size` per stream into nothing. *Fix:*
allocate the body at `END_HEADERS` (content-length known or not) with the
phase-0 allocator; `body_read` only registers the ready handler (body
already complete → run it at once). For streams whose action never reads
the body, consume DATA without storing; nghttp2 sends
`RST_STREAM(NO_ERROR)` itself when the server ends its side first.
**Status: fixed (§Option 1 body paragraph).**

**R1-09 · H · Stream object in the request pool is a use-after-free.**
The ADR puts `nxt_h2p_stream_t` "in the request pool" and ends the request
from the provider's EOF (`nxt_http_request_done` → `close` → pool
released). nghttp2 still holds `stream_user_data` and will call
`on_stream_close` and possibly the `read_callback` after that. *Fix:*
allocate the stream from the connection's `mem_pool` (or a per-connection
free list); `close` sets `stream->r = NULL` and
`nghttp2_session_set_stream_user_data(…, NULL)`; the stream is freed in
`on_stream_close`. Document the two lifetimes explicitly. **Status: fixed.**

**R1-10 · H · Connection lifetime versus in-flight requests is not
designed.** With N requests per connection, a TLS error or GOAWAY must
fail every request, but `close` hooks arrive asynchronously from the
application path (`nxt_router_http_request_error`, `nxt_router.c:7573`).
Nothing in the ADR keeps `nxt_h2proto_t`/`nxt_conn_t` alive until the last
stream's `close` has run, and nothing says which of the two ends triggers
what. *Fix:* a `stream_count` on the connection; connection error →
`nghttp2_session_terminate_session()`, then for each stream with `r`,
`nxt_http_request_error_handler(task, r, stream)` (the h1 analogue is
`nxt_h1p_conn_request_error`, `:1804`); the connection is released only
when `stream_count == 0` and the session has been deleted. **Status:
fixed (§Option 1 error paragraph + test matrix).**

**R1-11 · M · Flow control: only the stream window is discussed.** With
`nghttp2_option_set_no_auto_window_update(1)` both the stream and the
connection window stop updating; the connection window must be consumed
too (`nghttp2_session_consume_connection()`), or the connection stalls at
64 KB. The ADR also does not bound the per-connection body budget: N
streams × `max_body_size` temp files per client. *Fix:* stream window =
`body_buffer_size`; connection window = 2 × `body_buffer_size` (or a
listener option), consumed at connection level as soon as the bytes are
copied and at stream level when they land in `r->body`. **Status: fixed.**

**R1-12 · M · Pseudo-header validation is under-specified and partly
duplicates nghttp2.** nghttp2's HTTP messaging layer already rejects
duplicate/misordered/unknown pseudo-headers, `connection`, `te` other than
`trailers`, upper-case names, and content-length mismatch with
`RST_STREAM(PROTOCOL_ERROR)` (unless `nghttp2_option_set_no_http_messaging`
is set). What is ours: `:path` must be non-empty and start with `/` or be
`*` for OPTIONS; `:authority` validation with the same function
`nxt_http_request_host()` uses (it is a field callback, `:87`, so factor
its validator); a `host` field present with a different value →
`RST_STREAM(PROTOCOL_ERROR)` (RFC 9113 §8.3.1); plain `CONNECT` → 405 or
`RST_STREAM(REFUSED_STREAM)` since there is no tunnel; `:scheme` is
recorded but not trusted for `scheme` matching (`r->tls` decides,
`nxt_http_route.c:2045-2053`). *Fix:* say exactly this and keep
messaging validation on. **Status: fixed.**

**R1-13 · M · Graceful reload / GOAWAY is only described from the client
side.** The router drains listeners in two phases
(`nxt_router_listen_socket_close`, `nxt_router.c:5012-5100`) and h1 idle
connections notice via `c->listen->draining` in
`nxt_h1p_idle_io_read_handler` (`:420-431`); process shutdown uses
`nxt_runtime_close_idle_connections()` over `engine->idle_connections`.
The ADR does not tie h2 into either. *Fix:* when the connection has no
streams it sits on `nxt_conn_idle()` like h1 (so shutdown closes it); when
`draining`/`joint == NULL` is seen (at idle, or at the next `on_begin_headers`)
send `nghttp2_submit_shutdown_notice()` then, after in-flight streams
finish or `send_timeout` expires, `nghttp2_submit_goaway()` with the real
`last_stream_id` and close. **Status: fixed.**

**R1-14 · L · WebSocket/CONNECT scope statement is incomplete.** Leaving
`ws_frame_start` NULL is right, but the ADR should also say: never
advertise `SETTINGS_ENABLE_CONNECT_PROTOCOL` (so browsers keep using h1 for
WebSocket, RFC 8441 §3), `r->websocket_handshake` stays 0 (only the
h1-only `Upgrade` callback sets it, `nxt_h1proto.c:757`), and plain
CONNECT is refused. **Status: fixed.**

**R1-15 · L · Response copy.** The ADR lists the extra copy into nghttp2
frames as a con; `NGHTTP2_DATA_FLAG_NO_COPY` + `send_data_callback`
removes it but conflicts with the `mem_send` → `c->write` design. Keep the
copy for v1; note the option. **Status: noted in ADR.**

### Security

**R1-16 · H · Mitigations are named, not specified.** The ADR says
"Rapid Reset is mitigated inside nghttp2 ≥ 1.57" and "we still cap
MAX_CONCURRENT_STREAMS and stream creation rate ourselves" but gives no
values, and there is no CONTINUATION-flood (CVE-2024-28182), HPACK-bomb,
SETTINGS/PING-flood or header-list-size requirement at all. The nghttp2
defaults, from the installed header: reset rate limit burst 1000 / 33 per
second (`nghttp2_option_set_stream_reset_rate_limit`, GOAWAY on
exhaustion), `max_continuations` 8, `max_settings` 32, `max_outbound_ack`
1000. *Fix:* an explicit acceptance-criteria table with values and the
nghttp2 call for each, plus what nghttp2 does not do (per-connection total
request cap, per-connection body budget, handshake/idle timers), and a
fuzz target. **Status: fixed (new §"Security requirements").**

**R1-17 · M · Header list size.** No `SETTINGS_MAX_HEADER_LIST_SIZE` is
advertised in the design or the spike. nghttp2 also caps an inbound
header block at 64 KiB internally (the spike rejected a 70 KB header, see
spike results). *Fix:* advertise `large_header_buffer_size ×
large_header_buffers` (32 KiB with the defaults at `nxt_router.c:3020-3021`)
and treat over-size as 431 on the stream. **Status: fixed.**

**R1-18 · M · QUIC security list is a sentence, not requirements.**
Amplification limit (3×, enforced by ngtcp2/quiche but only if Retry is
not the only validation), Retry with a keyed token and a rotating secret,
stateless reset with a static key, CID steering (engine id in the CID,
`SO_ATTACH_REUSEPORT_EBPF`), 0-RTT replay (early data off, or idempotent
methods only and `Early-Data: 1` to the app). *Fix:* keep as a
requirements list in the deferred h3 section. **Status: fixed.**

### Event loop, dependencies, CI

**R1-19 · M · CI has none of the tooling.** `build-test.yml:88-89` installs
only `libbrotli-dev ccache`; h2 needs `libnghttp2-dev` and
`nghttp2-client` (nghttp, h2load). h2spec is not packaged by Ubuntu and
could not be built here (its `@latest` needs Go ≥ 1.26; GitHub releases are
unreachable from this container), so the CI step must download the release
binary. `test/requirements.txt` has only `pytest` and `pyOpenSSL`; a pure
Python `h2` package is the smallest client dependency (curl in the runner
has nghttp2 too). deb/rpm: add `libnghttp2-dev` / `libnghttp2-devel`
(`pkg/deb/debian/control.in:7`, `pkg/rpm/unit.spec.in:8-10`). **Status:
fixed (phase 1 table).**

**R1-20 · L · Timers.** Correct as written for h3 (bias 0 because
`nxt_timer_add()` coalesces within `bias`, `nxt_timer.c:70`); h2 needs no
change, the connection timers keep the 50 ms bias. No action.

### Spikes

**R1-21 · M · h2 spike does not exercise manual flow control.** It calls
`nghttp2_session_consume()` while auto window update is on; nghttp2 returns
`NGHTTP2_ERR_INVALID_STATE`, which the spike ignores, so the "100 KB POST
exercising WINDOW_UPDATE" is nghttp2's automatic behaviour, not the ADR's
model. *Fix:* `nghttp2_option_set_no_auto_window_update(1)`, consume both
levels, re-test with a 5 MB POST. **Status: fixed and re-run.**

**R1-22 · M · h2 spike sets no security options.** No
`SETTINGS_MAX_HEADER_LIST_SIZE`, reset rate limit, `max_continuations`.
*Fix:* add them; it also proves the 1.59 header has the symbols.
**Status: fixed.**

**R1-23 · L · h2 spike bugs.** (a) `conn_flush()` truncates a `mem_send`
result larger than `outbuf` and silently corrupts the stream (unreachable
with 16 KiB frames but wrong); (b) the accept loop ignores
`conn_handshake()`'s return, so an ALPN-`http/1.1` client is closed only
on its next event; (c) `SSL_CTX` is never freed, `conns[]` is
`FD_SETSIZE`-bound, the timer scan is O(FD_SETSIZE) — spike-only. *Fix:*
(a) treat as a fatal error, (b) close on failure. **Status: fixed.**

**R1-24 · L · h3 spike.** `getrandom()` return unchecked (`-Wextra`
warning), `quiche_h3_send_response/send_body` return values ignored (a
blocked stream would drop the body without notice), no Retry. *Fix:* check
the returns and log; the rest is documented as out of scope. **Status:
fixed and re-run.**

### Simplicity

**R1-25 · H · The plan carries three phases that a first h2 release does
not need.** Phase 2 (h3, 25–40 days) and phase 3 (GSO, eBPF steering,
0-RTT, priority hints, `/status` counters, perf gates) are analysis, not
decisions, and priority hints are deprecated by RFC 9113 anyway. Phase 0
lists seven rows where four are needed. *Fix:* decide h2 now; record the
h3 analysis as a deferred decision with its requirements list; cut phase 0
to the four rows that h2 needs; move `/status` counters and h2c out;
phase 1 gets a task list with tests. **Status: fixed.**

### Round 1 spike re-run (Ubuntu 24.04, nghttp2 1.59.0-1ubuntu0.4, OpenSSL 3.0.13, `unshare -n`)

Before the fixes (as committed in `ddf19bc3`):

- `curl --http2`: `HTTP 2 code 200`; `--parallel` ×4: four `2 200`;
  POST 100 KB: `2 200 up=100000`; POST 5 MB: `2 200 up=5000000`.
- `nghttp -nv`: SETTINGS, HEADERS, `:status: 200`.
- `h2load -n 2000 -c 8 -m 16`: 2000/2000 succeeded (416 req/s, bounded by
  the spike's 100 ms body timer). `h2load -n 1000 -c 2 -m 200` (above the
  server's `MAX_CONCURRENT_STREAMS` = 100): 1000/1000, h2load respects the
  setting.
- ALPN `http/1.1` client: refused (curl exit 56), server log
  `alpn=http/1.1 -> h1 (not served by this spike)`.
- 70 KB request header: refused by nghttp2's inbound header block cap
  (curl exit 55), connection closed.
- quiche h3 spike rebuilt from the cached Cargo build (32 MB binary,
  `-Wextra`: one `getrandom` warning); aioquic 1.3.0 client: three GETs
  on streams 0/4/8 returned `:status 200` with bodies, `alpn h3`, QUIC v1,
  server `recv=10 sent=10 lost=0`.

### Round 1 improve (what changed)

ADR (`0005-http2-http3.md`, rewritten, 933 lines):

- header now cites the review branch, not `b3bec257`; every reference in
  R1-01 corrected; spikes pointed at `docs/adr/0005-spikes/` (R1-02).
- §3 documents the field-hash contract (R1-07), the draining/idle paths
  (R1-13) and that error pages need no status text (R1-06); §6 documents
  the `prepare_msg` 431/501 checks and the `name_length` type (R1-03).
- Option 1 rewritten: classic `ssize_t` API and symbol probes (R1-04),
  ALPN callback on every bundle context (R1-05), two objects with two
  lifetimes and the `close`/`on_stream_close` detach rule (R1-09), body
  allocated at `END_HEADERS`, `body_read` registers interest only,
  connection and stream windows with consume points and the per-connection
  budget (R1-08, R1-11), the nghttp2-vs-ours validation split (R1-12),
  errors in both directions with `stream_count` (R1-10), server-initiated
  GOAWAY on draining/`requests_total` (R1-13), the WebSocket/CONNECT scope
  statement (R1-14), the copy note (R1-15).
- new section "Security requirements" with values and enforcement points
  (R1-16, R1-17); QUIC requirements list under "Deferred: HTTP/3" (R1-18).
- plan: phase 0 cut to four rows with tests; phase 1 is a seven-row task
  list with a concrete `test_http2.py` matrix, h2spec, CI packages and a
  fuzz target (R1-19); h3 is a deferred checklist, not a phase; phase 3
  removed (R1-25).

Spikes:

- `h2_epoll_server.c`: `nghttp2_session_server_new2()` with
  `no_auto_window_update`, `stream_reset_rate_limit(1000, 33)`,
  `max_continuations(8)`, `max_settings(32)`,
  `max_deflate_dynamic_table_size(4096)`; SETTINGS
  `MAX_CONCURRENT_STREAMS` 128, `INITIAL_WINDOW_SIZE` 256 KiB,
  `MAX_HEADER_LIST_SIZE` 32 KiB; connection window 512 KiB via
  `nghttp2_session_set_local_window_size()`; connection-level consume in
  `on_data_chunk_recv`, stream-level consume from the timer tick;
  `conn_flush()` fails instead of truncating; the accept loop closes a
  failed handshake (R1-21, R1-22, R1-23).
- `h3_epoll_server.c`: `getrandom()` and `quiche_h3_send_*` return values
  checked (R1-24).
- README updated with the new commands and the re-run results.

Re-run after the fixes (same environment): identical outcomes for all
cases; the 5 MB POST now completes under manual flow control (window
released 256 KiB per 100 ms tick), `nghttp -nv` shows the stream-0
WINDOW_UPDATE, the `http/1.1` client is closed immediately (curl exit 52
instead of 56). h3: three GETs, `alpn h3`, `recv=10 sent=10 lost=0`.

---

## Round 2 (re-review of the round-1 text)

**R2-01 · H (simplicity) · Manual window updates were kept for no gain.**
Round 1 specified `no_auto_window_update` with a connection-level consume
in `on_data_chunk_recv` and a stream-level consume after the copy. But the
same paragraph copies every chunk into `r->body` immediately (memory up to
`body_buffer_size`, then a temp file), so the bytes never wait in our
memory and the consume points release the window at once anyway: two code
paths that bound nothing. *Fix:* automatic window updates (nghttp2's
default); `SETTINGS_INITIAL_WINDOW_SIZE` 256 KiB and a 1 MiB connection
window as throughput knobs; memory bound = streams × `body_buffer_size`,
disk = `max_body_size` per stream, as for h1. The spike keeps the manual
mode as the harder demonstration. Security table row "Slow read" and the
open questions updated. **Status: fixed.**

**R2-02 · M · `:authority` versus `host` was wrong in detail.**
`nxt_http_request_host()` answers 400 to any second host
(`src/nxt_http_request.c:96-98`), so routing a `host` field through the
neutral hash after `:authority` set `r->host` rejects every request whose
client sends both, even when equal. Also `nxt_http_validate_host()` is
already a separate `static` function (`:112`), so phase 0.1 is an export,
not a refactor. *Fix:* `on_header` intercepts `host`: equal → dropped,
different → `RST_STREAM(PROTOCOL_ERROR)`, no `:authority` → normal path.
Phase 0.1 reworded. **Status: fixed.**

**R2-03 · M · "nghttp2 sends `RST_STREAM(NO_ERROR)` itself" stated as
fact.** It is nghttp2's documented server behaviour after a response ends
before the request, but it was not verified against the source here.
*Fix:* stated as expected and made a phase-1 test, with the fallback
(`close` submits it). **Status: fixed.**

**R2-04 · L · Security table overstated nghttp2 for empty-frame floods.**
`NGHTTP2_ERR_FLOODED` is the outbound ACK-queue check (PING/SETTINGS), not
a defence against empty DATA/WINDOW_UPDATE/PRIORITY floods. *Fix:* row
rewritten to what is true: no allocation per frame, no callback work, the
progress timer, and a CPU watch in the h2load run. **Status: fixed.**

**R2-05 · L · Phase 1.1 asserted that Ubuntu 22.04's nghttp2 1.43 must
fail the probe.** Jammy's security backports (USN-6432-1, USN-6754-1) may
carry both symbols. *Fix:* the CI leg records the probe result; either is
acceptable. **Status: fixed.**

**R2-06 · L · Miscounts and missing lines.** Phase 0.1 said "nine or
eleven" neutral entries; it is seven, nine with OTel. The CGI upper-casing
had no line reference; it is `src/nxt_router.c:7954-7965` (R1-01 table
corrected too). Process shutdown closes idle connections with
`nxt_conn_close()` directly (`src/nxt_runtime.c:507-508`), which the idle
state's `close_handler` must survive; added. **Status: fixed.**

**R2-07 · L · Timers were over-designed.** "One timer for the oldest open
request" needs ordering bookkeeping. *Fix:* one connection progress timer
armed while any request is incomplete and reset by any frame that
advances one; the open question about per-stream timers is closed.
**Status: fixed.**

**R2-08 · L · The fuzz seed-corpus sentence was muddled.** *Fix:* generate
the seeds once with the Python `h2` package (preface + SETTINGS, GET, POST
with DATA, CONTINUATION split, RST_STREAM). **Status: fixed.**

No spike change in round 2; the round-1 binaries and results stand.
