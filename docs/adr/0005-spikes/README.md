# Spikes for ADR 0005 (HTTP/2 and HTTP/3)

Throwaway code that validated the event-loop integration models in
`docs/adr/0005-http2-http3.md` §8. Nothing here is production code and
nothing links into FreeUnit; each file is a standalone server driven from a
bare epoll loop that stands in for `nxt_event_engine_start()`.

## h2_epoll_server.c: HTTP/2 with nghttp2 over OpenSSL, ALPN, epoll

Validated on Ubuntu 24.04 with the distro `libnghttp2-dev` 1.59 and OpenSSL
3.0.13.

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem \
        -days 2 -subj "/CN=localhost"
cc -O1 -g -Wall -o h2_epoll_server h2_epoll_server.c -lssl -lcrypto -lnghttp2
./h2_epoll_server cert.pem key.pem 8443 &

curl -sk --http2 -w "\nHTTP %{http_version} code %{http_code}\n" https://127.0.0.1:8443/hello
curl -sk --http2 --parallel -o /dev/null -o /dev/null -o /dev/null -o /dev/null \
     -w "%{http_version} %{http_code}\n" \
     https://127.0.0.1:8443/a https://127.0.0.1:8443/b https://127.0.0.1:8443/c https://127.0.0.1:8443/d
head -c 100000 /dev/zero | curl -sk --http2 --data-binary @- -o /dev/null \
     -w "%{http_version} %{http_code} up=%{size_upload}\n" https://127.0.0.1:8443/post
nghttp -nv https://127.0.0.1:8443/x
h2load -n 4000 -c 8 -m 16 https://127.0.0.1:8443/x
```

Observed: `HTTP 2 code 200`; four streams (1, 3, 5, 7) on one connection;
`POST /post body=100000` on the server side; nghttp shows SETTINGS, HEADERS
and three DATA frames (the body is produced by a 100 ms timer through a
deferred `nghttp2_data_provider`, the shape of a future
`nxt_h2p_request_send()`); h2load 4000/4000 succeeded.

## h3_epoll_server.c: HTTP/3 with quiche's C FFI over one UDP socket, epoll

quiche 0.30 built from crates.io with the `ffi` feature (BoringSSL is built
inside by cmake; 2 m 25 s wall on two shared cores, 178 MB `libquiche.a`).

```sh
cargo init --name h3quiche && cargo add quiche@0.30 --features ffi
cargo build --release -j2
QUICHE=$HOME/.cargo/registry/src/*/quiche-0.30.0
cc -O1 -g -Wall -I$QUICHE/include -o h3_epoll_server h3_epoll_server.c \
   target/release/deps/libquiche-*.a -lstdc++ -lm -lpthread -ldl
./h3_epoll_server cert.pem key.pem 4433 &

python3 -m venv venv && venv/bin/pip install aioquic
venv/bin/python h3_client.py 127.0.0.1 4433 /a /b /c
```

Observed:

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
