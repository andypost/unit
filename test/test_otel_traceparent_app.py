"""End-to-end traceparent propagation into an application process.

test_otel.py already covers the traceparent header on Unit's own response
(a `return: 200` route, never touching an app). What is not covered
anywhere yet is the thing PHP/Python code actually reads: whether the
app process's request environment (`$_SERVER['HTTP_TRACEPARENT']` in PHP,
`environ['HTTP_TRACEPARENT']` in a WSGI app) carries the same header --
either the one the client sent, or a freshly generated one when telemetry
is configured and the client sent none (src/nxt_otel.c,
nxt_otel_propagate_header(); see docs/observability/usdt-plan.md and the
traceparent parsing/forwarding described for src/nxt_otel.c:170 and :633).

Without --otel (or with --otel but no "settings.telemetry" configured),
r->otel stays NULL for every request (src/nxt_http_request.c) and
nxt_otel_propagate_header() is never reached, so no *new* traceparent is
ever generated -- an inbound one still reaches the app unmodified because
Unit forwards every request header to the app process regardless. Both
halves are exercised here:

  - test_traceparent_forwarded_without_otel: no telemetry configured at
    all (works the same whether or not the binary was built --otel) --
    an inbound traceparent is forwarded to the app byte-for-byte.
  - test_traceparent_generated_when_missing_with_otel: telemetry
    configured against a throwaway OTLP endpoint, no inbound
    traceparent -- the app must see a freshly generated, valid one.
  - test_traceparent_inherited_with_otel: telemetry configured, inbound
    traceparent present -- the app must see the same trace id, but with
    FreeUnit's own span id as parent-id (not the inbound parent-id), so
    that the app's spans become children of FreeUnit's span rather than
    siblings of it.

The otel-dependent tests skip cleanly (never fail) when the binary was
not built with --otel, exactly like test_otel.py's own tests.
"""

import re
import socket

import pytest

from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'all'}}

client = ApplicationPython()

TRACE_ID = '0af7651916cd43dd8448eb211c80319c'
PARENT_ID = 'b7ad6b7169203331'
INBOUND_TRACEPARENT = f'00-{TRACE_ID}-{PARENT_ID}-01'

# W3C Trace Context traceparent: "$version-$trace_id-$parent_id-$flags",
# 2/32/16/2 lowercase hex digits (https://www.w3.org/TR/trace-context/).
TRACEPARENT_RE = re.compile(
    r'^[0-9a-f]{2}-[0-9a-f]{32}-[0-9a-f]{16}-[0-9a-f]{2}$'
)


def _get_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def _configure_telemetry_or_skip():
    """Add telemetry, pointed at a throwaway endpoint, to the app config
    `client.load('traceparent')` already applied.

    This test only cares whether nxt_otel_rs_is_init() ends up true (which
    gates traceparent generation/propagation in nxt_otel_propagate_header(),
    src/nxt_otel.c) -- not whether a span is ever actually exported, so the
    endpoint does not need a listener behind it. The full config is
    re-applied (rather than PUT to the settings/telemetry sub-path) because
    "settings" does not exist yet, and the control API's sub-path PUT
    requires its parent object to already be there.
    """
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
    """GET / with the given headers, retrying while the app reports no
    traceparent at all -- otel (re)init races the very first request the
    same way test_otel.py's _get_until_header documents.
    """
    import time

    resp = client.get(headers=headers)
    for _ in range(retries):
        seen = resp['headers'].get('X-Seen-Traceparent', '')
        if resp['status'] == 200 and seen:
            return seen
        time.sleep(delay)
        resp = client.get(headers=headers)

    return resp['headers'].get('X-Seen-Traceparent', '')


def test_traceparent_forwarded_without_otel():
    """No telemetry configured: an inbound traceparent still reaches the
    app unmodified, since Unit forwards all request headers regardless of
    otel. This holds whether or not the binary was built with --otel."""
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
    ), 'app must see the exact inbound traceparent ($_SERVER-equivalent)'


def test_traceparent_generated_when_missing_with_otel():
    """Telemetry configured, no inbound traceparent: the app must receive
    a freshly generated, valid W3C traceparent it never sent itself."""
    client.load('traceparent')
    _configure_telemetry_or_skip()

    seen = _seen_traceparent(
        headers={'Host': 'localhost', 'Connection': 'close'}
    )

    assert seen, 'app must see a generated traceparent when telemetry is on'
    assert TRACEPARENT_RE.match(seen), f'not a valid traceparent: {seen!r}'
    assert TRACE_ID not in seen, 'this must be a new trace, not the fixture id'


def test_traceparent_inherited_with_otel():
    """Telemetry configured, inbound traceparent present: the app must see
    the same trace id it was sent (span continuation, not a new trace), but
    with FreeUnit's own span id as parent-id rather than the client's
    original parent-id -- otherwise the app's spans (e.g. Drupal/Gander,
    the OTel PHP SDK) become siblings of FreeUnit's span instead of its
    children."""
    client.load('traceparent')
    _configure_telemetry_or_skip()

    seen = _seen_traceparent(
        headers={
            'Host': 'localhost',
            'traceparent': INBOUND_TRACEPARENT,
            'Connection': 'close',
        }
    )

    assert seen, 'app must see a traceparent header at all'
    assert TRACEPARENT_RE.match(seen), f'not a valid traceparent: {seen!r}'
    assert TRACE_ID in seen, 'trace id must be inherited, not replaced'
    assert PARENT_ID not in seen, (
        'the app must see FreeUnit\'s own span id as parent-id, not the '
        'client-supplied parent-id it sent in'
    )
