# Spikes for ADR 0005 (HTTP/2 and HTTP/3)

Throwaway code that validated the event-loop integration models in
`docs/adr/0005-http2-http3.md` §8. Nothing here is production code and
nothing links into FreeUnit; each file is a standalone server driven from a
bare epoll loop that stands in for `nxt_event_engine_start()`.

## h2_epoll_server.c: HTTP/2 with nghttp2 over OpenSSL, ALPN, epoll

Validated on Ubuntu 24.04 with the distro `libnghttp2-dev` 1.59.0-1ubuntu0.4
and OpenSSL 3.0.13 (`-Wall -Wextra` clean). Since the review
(`../0005-REVIEW.md`, round 1) the spike runs nghttp2 with the ADR's
security settings (manual window updates, RST_STREAM rate limit,
CONTINUATION cap, SETTINGS cap, `MAX_HEADER_LIST_SIZE`) and releases the
stream window only from the timer tick, which stands in for the router
copying DATA into `r->body`.

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem \
        -days 2 -subj "/CN=localhost"
cc -O1 -g -Wall -Wextra -o h2_epoll_server h2_epoll_server.c -lssl -lcrypto -lnghttp2
./h2_epoll_server cert.pem key.pem 8443 &

curl -sk --http2 -w "\nHTTP %{http_version} code %{http_code}\n" https://127.0.0.1:8443/hello
curl -sk --http2 --parallel -o /dev/null -o /dev/null -o /dev/null -o /dev/null \
     -w "%{http_version} %{http_code}\n" \
     https://127.0.0.1:8443/a https://127.0.0.1:8443/b https://127.0.0.1:8443/c https://127.0.0.1:8443/d
head -c 100000 /dev/zero | curl -sk --http2 --data-binary @- -o /dev/null \
     -w "%{http_version} %{http_code} up=%{size_upload}\n" https://127.0.0.1:8443/post
head -c 5000000 /dev/zero | curl -sk --http2 --data-binary @- -o /dev/null \
     -w "%{http_version} %{http_code} up=%{size_upload}\n" https://127.0.0.1:8443/post5m
nghttp -nv https://127.0.0.1:8443/x
h2load -n 2000 -c 8 -m 16 https://127.0.0.1:8443/x
h2load -n 1000 -c 2 -m 200 https://127.0.0.1:8443/y     # above MAX_CONCURRENT_STREAMS
curl -sk --http1.1 https://127.0.0.1:8443/h1              # ALPN http/1.1: refused
curl -sk --http2 -H "X-Big: $(head -c 70000 /dev/zero | tr '\0' a)" https://127.0.0.1:8443/big
```

Observed (2026-09-24 re-run, in `unshare -n` on a high port): `HTTP 2 code
200`; four streams on one connection; `up=100000` and `up=5000000` with the
stream window released 256 KiB per tick; nghttp shows SETTINGS, the
stream-0 WINDOW_UPDATE for the connection window, HEADERS and three DATA
frames (the body is produced by a 100 ms timer through a deferred
`nghttp2_data_provider`, the shape of a future `nxt_h2p_request_send()`);
h2load 2000/2000 and 1000/1000; the `http/1.1` client is closed right after
the handshake; the 70 KB header is refused by nghttp2's inbound header
block cap and the connection closed.

## h3_epoll_server.c: HTTP/3 with quiche's C FFI over one UDP socket, epoll

quiche 0.30 built from crates.io with the `ffi` feature (BoringSSL is built
inside by cmake; 2 m 25 s wall on two shared cores, 178 MB `libquiche.a`).

```sh
cargo init --name h3quiche && cargo add quiche@0.30 --features ffi
cargo build --release -j2
QUICHE=$HOME/.cargo/registry/src/*/quiche-0.30.0
cc -O1 -g -Wall -Wextra -I$QUICHE/include -o h3_epoll_server h3_epoll_server.c \
   target/release/deps/libquiche-*.a -lstdc++ -lm -lpthread -ldl
./h3_epoll_server cert.pem key.pem 4433 &

python3 -m venv venv && venv/bin/pip install aioquic
venv/bin/python h3_client.py 127.0.0.1 4433 /a /b /c
```

Observed (unchanged on the 2026-09-24 re-run from the cached Cargo build,
after checking the `getrandom()` and `quiche_h3_send_*` return values):

```
/a -> ['HEADERS :status=200 server=freeunit-h3-spike content-type=text/plain', 'DATA hello over h3, stream 0']
/b -> ['HEADERS :status=200 server=freeunit-h3-spike content-type=text/plain', 'DATA hello over h3, stream 4']
/c -> ['HEADERS :status=200 server=freeunit-h3-spike content-type=text/plain', 'DATA hello over h3, stream 8']
negotiated alpn: h3 version: 0x1
```

and on the server `[quic] connection closed, recv=10 sent=10 lost=0`.

The same "own UDP socket, own timers, no library threads" loop is what
ngtcp2 + nghttp3 (the ADR's chosen h3 stack) needs; the quiche FFI was used
because it was the only QUIC stack buildable in this container (OpenSSL here
is 3.0, without QUIC; the distro ngtcp2 is 0.12 with a GnuTLS-only backend).

`h3_client.py` builds the aioquic datagram endpoint by hand because
`aioquic.asyncio.connect()` opens an AF_INET6 socket and the container has
no IPv6.
