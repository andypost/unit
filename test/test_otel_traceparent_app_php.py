"""PHP variant of test_otel_traceparent_app.py: same three scenarios, but
checking $_SERVER['HTTP_TRACEPARENT'] via the PHP SAPI (test/php/traceparent/
index.php) instead of a WSGI environ. See that file's module docstring for
the full rationale; this one only covers what differs for PHP.
"""

import re
import socket
import time

import pytest

from unit.applications.lang.php import ApplicationPHP

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()

TRACE_ID = '0af7651916cd43dd8448eb211c80319c'
PARENT_ID = 'b7ad6b7169203331'
INBOUND_TRACEPARENT = f'00-{TRACE_ID}-{PARENT_ID}-01'

TRACEPARENT_RE = re.compile(
    r'^[0-9a-f]{2}-[0-9a-f]{32}-[0-9a-f]{16}-[0-9a-f]{2}$'
)


def _get_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def _configure_telemetry_or_skip():
    current = client.conf_get()

    current.setdefault('settings', {})['telemetry'] = {
        'endpoint': f'http://127.0.0.1:{_get_free_port()}/v1/traces',
        'protocol': 'http',
        'sampling_ratio': 1.0,
        'batch_size': 1,
    }

    conf = client.conf(current)
    if 'success' in conf:
        return
    if 'telemetry' in str(conf).lower():
        pytest.skip('unit built without --otel')
    pytest.fail(f'valid telemetry config rejected: {conf}')


def _seen_traceparent(headers, retries=150, delay=0.1):
    resp = client.get(headers=headers)
    for _ in range(retries):
        seen = resp['headers'].get('X-Seen-Traceparent', '')
        if resp['status'] == 200 and seen:
            return seen
        time.sleep(delay)
        resp = client.get(headers=headers)

    return resp['headers'].get('X-Seen-Traceparent', '')


def test_traceparent_forwarded_without_otel():
    client.load('traceparent')

    resp = client.get(
        headers={
            'Host': 'localhost',
            'traceparent': INBOUND_TRACEPARENT,
            'Connection': 'close',
        }
    )

    assert resp['status'] == 200
    assert (
        resp['headers'].get('X-Seen-Traceparent') == INBOUND_TRACEPARENT
    ), '$_SERVER[\'HTTP_TRACEPARENT\'] must carry the exact inbound value'


def test_traceparent_generated_when_missing_with_otel():
    client.load('traceparent')
    _configure_telemetry_or_skip()

    seen = _seen_traceparent(
        headers={'Host': 'localhost', 'Connection': 'close'}
    )

    assert seen, 'PHP must see a generated traceparent when telemetry is on'
    assert TRACEPARENT_RE.match(seen), f'not a valid traceparent: {seen!r}'
    assert TRACE_ID not in seen, 'this must be a new trace, not the fixture id'


def test_traceparent_inherited_with_otel():
    client.load('traceparent')
    _configure_telemetry_or_skip()

    seen = _seen_traceparent(
        headers={
            'Host': 'localhost',
            'traceparent': INBOUND_TRACEPARENT,
            'Connection': 'close',
        }
    )

    assert seen, 'PHP must see a traceparent header at all'
    assert TRACEPARENT_RE.match(seen), f'not a valid traceparent: {seen!r}'
    assert TRACE_ID in seen, 'trace id must be inherited, not replaced'
