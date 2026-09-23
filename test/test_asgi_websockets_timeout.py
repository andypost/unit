"""The request deadline must not end an established WebSocket (#422).

"limits": {"timeout"} bounds how long a worker may take to answer a request.
A WebSocket upgrade answers it -- the 101 goes out -- and what follows is a
session, not a request, so a quiet session must outlive the deadline.
"""

from packaging import version

from unit.applications.lang.python import ApplicationPython
from unit.applications.websockets import ApplicationWebsocket

prerequisites = {
    'modules': {'python': lambda v: version.parse(v) >= version.parse('3.5')}
}

client = ApplicationPython(load_module='asgi')
ws = ApplicationWebsocket()


def test_asgi_websockets_timeout_quiet_session(skip_alert):
    skip_alert(r'socket close\(\d+\) failed')

    assert 'success' in client.conf(
        {'http': {'websocket': {'keepalive_interval': 0}}}, 'settings'
    ), 'clear keepalive_interval'

    client.load('websockets/mirror', limits={'timeout': 1})

    resp, sock, _ = ws.upgrade()
    assert resp['status'] == 101, 'upgrade'

    # Read raw rather than through frame_read(): on a closed connection that
    # would spin on EOF instead of failing.
    sock.settimeout(3)

    try:
        early = sock.recv(4096)
    except TimeoutError:
        early = None

    assert early is None, f'nothing arrives on a quiet session: {early!r}'

    ws.frame_write(sock, ws.OP_TEXT, 'still here')
    frame = ws.frame_read(sock, read_timeout=2)

    assert frame['opcode'] == ws.OP_TEXT, 'session survived the deadline'
    assert frame['data'].decode('utf-8') == 'still here', 'mirror'

    ws.frame_write(sock, ws.OP_CLOSE, ws.serialize_close())
    frame = ws.frame_read(sock)
    assert frame['opcode'] == ws.OP_CLOSE, 'close'

    sock.close()
