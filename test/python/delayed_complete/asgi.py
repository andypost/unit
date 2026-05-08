"""
ASGI handler that sleeps for X-Delay seconds inside the request handler
and then returns a real, complete response with a body containing a
canonical "200 OK" status line indicator.

Distinct from delayed/asgi.py which never sends a final
http.response.body event when the request body is empty — that app is
useful only for log-marker assertions.  This app is for tests that need
to verify the byte stream that reaches the client.

Used by test/test_connection_drain.py to prove that the P5 router
drain keeps the TCP listener alive long enough for the response to be
written back to the client.
"""

import asyncio


async def application(scope, receive, send):
    if scope['type'] != 'http':
        # Lifespan / other scopes are ignored — this app handles HTTP only.
        return

    body = b''
    while True:
        m = await receive()
        body += m.get('body', b'')
        if not m.get('more_body', False):
            break

    headers = scope.get('headers', [])

    def get_header(n, v=None):
        for h in headers:
            if h[0] == n:
                return h[1]
        return v

    delay = int(get_header(b'x-delay', 0))

    loop = asyncio.get_event_loop()
    future = loop.create_future()
    loop.call_later(delay, future.set_result, None)
    await future

    payload = b'drained 200 ok'

    await send(
        {
            'type': 'http.response.start',
            'status': 200,
            'headers': [
                (b'content-length', str(len(payload)).encode()),
                (b'content-type', b'text/plain'),
            ],
        }
    )
    await send(
        {
            'type': 'http.response.body',
            'body': payload,
            'more_body': False,
        }
    )
