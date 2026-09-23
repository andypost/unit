"""Minimal HTTP/3 GET client on aioquic, used as the h3 spike's test client
(curl here has no HTTP/3 and ngtcp2's gtlsclient needs a GnuTLS QUIC build)."""
import asyncio
import ssl
import sys

from aioquic.asyncio import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import QuicEvent


class Client(QuicConnectionProtocol):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.h3 = H3Connection(self._quic)
        self.waiters = {}
        self.out = {}

    def quic_event_received(self, event: QuicEvent):
        for ev in self.h3.handle_event(event):
            if isinstance(ev, HeadersReceived):
                self.out.setdefault(ev.stream_id, []).append(
                    "HEADERS " + " ".join(f"{k.decode()}={v.decode()}" for k, v in ev.headers))
            if isinstance(ev, DataReceived):
                self.out.setdefault(ev.stream_id, []).append("DATA " + ev.data.decode().strip())
                if ev.stream_ended:
                    self.waiters[ev.stream_id].set_result(self.out[ev.stream_id])

    async def get(self, host, port, path):
        sid = self._quic.get_next_available_stream_id()
        self.h3.send_headers(sid, [(b":method", b"GET"), (b":scheme", b"https"),
                                   (b":authority", f"{host}:{port}".encode()),
                                   (b":path", path.encode()), (b"user-agent", b"aioquic")],
                             end_stream=True)
        self.waiters[sid] = self._loop.create_future()
        self.transmit()
        return await asyncio.wait_for(self.waiters[sid], 5)


async def main(host, port, paths):
    conf = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN, verify_mode=ssl.CERT_NONE,
                             server_name=host)
    # aioquic.asyncio.connect() opens an AF_INET6 socket; this container has no
    # IPv6, so build the IPv4 datagram endpoint by hand.
    loop = asyncio.get_running_loop()
    quic = QuicConnection(configuration=conf)
    _, c = await loop.create_datagram_endpoint(lambda: Client(quic), local_addr=("0.0.0.0", 0))
    c.connect((host, port))
    await c.wait_connected()
    results = await asyncio.gather(*(c.get(host, port, p) for p in paths))
    for p, r in zip(paths, results):
        print(p, "->", r)
    print("negotiated alpn:", c._quic.tls.alpn_negotiated, "version:", hex(c._quic._version))
    c.close()
    await c.wait_closed()


asyncio.run(main(sys.argv[1], int(sys.argv[2]), sys.argv[3:] or ["/"]))
