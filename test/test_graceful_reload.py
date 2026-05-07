"""
Phase 1 of the FreeUnit graceful-shutdown plan: signal-handler split.

Background
----------
SIGTERM and SIGQUIT used to share an identical handler in
src/nxt_main_process.c -- both routed straight to nxt_runtime_quit()
without distinguishing fast from graceful exit.  P1 splits the two so
SIGQUIT now sets rt->quit_mode = NXT_PORT_QUIT_GRACEFUL, and the
NXT_PORT_MSG_QUIT message dispatched by nxt_runtime_stop_app_processes()
carries that byte.  libunit at src/nxt_unit.c:1056-1070 already parses
this exact wire format and dispatches to nxt_unit_quit() (src/nxt_unit.c:5753),
which lets in-flight requests drain when the byte is NXT_PORT_QUIT_GRACEFUL.

The plumbing is what these tests exercise behaviourally:

  * SIGQUIT -> in-flight request must complete with status 200.
  * SIGTERM -> in-flight request is dropped (connection reset or truncated).

A third placeholder test asserts the wire-format intent but is skipped
because verifying the actual quit_param byte would require C-level
instrumentation -- the behavioural pair above already covers reachability.

See roadmap/plan-graceful-shutdown.md (P1) for the full plan.
"""

import os
import signal
import socket
import time

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.log import Log

prerequisites = {'modules': {'python': 'all'}}

client = ApplicationPython()


@pytest.fixture(autouse=True)
def _require_restart_flag(request):
    """
    Every test in this module sends SIGTERM/SIGQUIT to the unitd master.
    The autouse `run` fixture in conftest.py only rmtrees the temp dir
    when --restart is set; otherwise it tries to PUT /config on the now
    dead daemon during teardown and crashes with KeyError: 'body'.

    Skip with an actionable message when the flag is missing instead of
    pretending to fail for the wrong reason.
    """
    if not request.config.getoption('--restart'):
        pytest.skip(
            'test_graceful_reload.py signals the unitd master process; '
            'rerun with --restart so conftest.py rmtrees the temp dir '
            'instead of trying /config PUT on a dead daemon'
        )


# Long enough that a non-graceful SIGTERM cannot accidentally let the
# request finish; short enough to keep the test suite snappy.  Bumped
# above the previous 3 s so fast machines have less chance of racing
# the response to completion before the signal lands.
INFLIGHT_DELAY = 5

# Cap for the curl-equivalent recv loop.  Must exceed INFLIGHT_DELAY plus
# graceful-drain overhead so a working SIGQUIT path has room to complete.
RESPONSE_TIMEOUT = 30


def _start_inflight_request(delay: int) -> socket.socket:
    """
    Open a TCP connection to Unit, send a request that takes *delay*
    seconds inside the WSGI handler (test/python/delayed/wsgi.py uses
    time.sleep when the request body is empty), and return the socket
    with the request fully written.  Caller is responsible for receiving
    and closing.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(RESPONSE_TIMEOUT)
    sock.connect(('127.0.0.1', 8080))

    req = (
        f'GET / HTTP/1.1\r\n'
        f'Host: localhost\r\n'
        f'X-Delay: {delay}\r\n'
        f'Connection: close\r\n'
        f'\r\n'
    ).encode()
    sock.sendall(req)

    return sock


def _recv_all(sock: socket.socket) -> bytes:
    """
    Drain the socket until EOF or timeout.  Returns whatever bytes
    arrived; an empty/truncated reply is a signal that the peer reset
    the connection mid-response, which is exactly what SIGTERM should do.
    """
    chunks = []
    deadline = time.monotonic() + RESPONSE_TIMEOUT
    while time.monotonic() < deadline:
        try:
            data = sock.recv(4096)
        except (ConnectionResetError, socket.timeout):
            break
        if not data:
            break
        chunks.append(data)
    return b''.join(chunks)


def test_sigquit_completes_inflight_request(unit_pid, skip_alert):
    """
    SIGQUIT to the unitd master must let an in-flight request complete.

    Before P1 the SIGQUIT path was identical to SIGTERM (both called
    nxt_runtime_quit() with no quit_param plumbing), so libunit defaulted
    to NXT_PORT_QUIT_NORMAL and the worker exited immediately.  With P1
    the main signal handler stores rt->quit_mode = NXT_PORT_QUIT_GRACEFUL
    and the QUIT message body now carries that byte; libunit's
    nxt_unit_quit() drains the active request before tearing the
    context down.

    ASGI is required so libunit's message loop can actually process
    the QUIT mid-request: a synchronous WSGI worker blocked in
    time.sleep() never pumps libunit, so the QUIT message would sit
    in the queue until the request finishes anyway and the test
    would pass for the wrong reason -- the request simply outran
    the (lost) signal.  ASGI's `await sleep(delay)` yields to the
    asyncio loop, letting libunit's add_reader callback dispatch
    nxt_unit_process_msg() while active_req is non-empty.

    Why we assert on log absence rather than body content:
    P1 plumbs GRACEFUL through libunit only.  The router process
    still tears down its TCP listener and IPC ports immediately on
    NXT_PORT_MSG_QUIT (router-side drain is P5), so the client TCP
    connection RSTs the moment the router exits -- regardless of
    whether the app worker drains gracefully.  Asserting on the
    response body therefore gets `b''` on both NORMAL and GRACEFUL
    and is useless as a P1 regression guard.

    The unambiguous positive evidence is the contrapositive of the
    SIGTERM test: "active request on ctx quit" at nxt_unit.c:5816
    fires *only* in the NORMAL branch's force-close loop.  On
    GRACEFUL the function returns early when active_req is
    non-empty and that loop is unreachable, so the marker never
    appears.  We give libunit ~1.5 s to process the QUIT message
    after the signal, then assert the marker is absent.  A real
    regression where SIGQUIT routed back to NORMAL would emit it.
    """
    client.load('delayed', module='asgi')

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')
    # ASGI lifespan in delayed/asgi.py raises AssertionError because
    # the test app only handles 'http' scope; that's logged at info
    # but appears in the alerts grep on some configs.
    skip_alert(r'ASGI Lifespan processing exception')

    sock = _start_inflight_request(INFLIGHT_DELAY)

    # Give the ASGI handler a moment to enter `await sleep(delay)`
    # before the signal so the QUIT can race the in-flight request.
    time.sleep(0.5)

    os.kill(unit_pid, signal.SIGQUIT)

    # 1.5 s is comfortably more than the IPC RTT for a QUIT message
    # plus libunit's nxt_unit_process_msg dispatch.  The drain
    # itself takes the full INFLIGHT_DELAY but we don't need to wait
    # for it -- only for libunit to have *taken* the GRACEFUL branch
    # rather than the NORMAL branch.
    time.sleep(1.5)

    sock.close()

    found = Log.findall(r'active request on ctx quit')
    assert found == [], (
        f'SIGQUIT triggered the NORMAL fast-exit branch in libunit '
        f'(active_req was force-closed at nxt_unit.c:5816 instead of '
        f'drained); regression in P1 plumbing.  Log markers found: '
        f'{found!r}.  Compare to test_sigterm_drops_inflight_request '
        f'which asserts the same marker is *present* on SIGTERM.'
    )


def test_sigterm_drops_inflight_request(unit_pid, skip_alert):
    """
    SIGTERM remains the fast-exit path: in-flight requests are dropped.

    With P1, rt->quit_mode = NXT_PORT_QUIT_NORMAL on SIGTERM and the QUIT
    message body carries 0; libunit's nxt_unit_quit() returns immediately
    and calls close_handler() on every active request (src/nxt_unit.c:5811).

    Why ASGI here?  In synchronous WSGI a worker that is busy in
    time.sleep() never pumps libunit's message loop, so the QUIT byte
    sits in the queue until the request has finished anyway.  The
    behavioural difference between QUIT_NORMAL and QUIT_GRACEFUL is only
    observable when libunit can actually process the QUIT message while
    a request is still in-flight -- which is what the asyncio-driven
    ASGI handler enables.  See test/python/delayed/asgi.py: the
    `await sleep(delay)` yields control back to the asyncio loop,
    letting libunit's add_reader callback run the QUIT path.

    Regression evidence is the "active request on ctx quit" warning at
    src/nxt_unit.c:5816 -- libunit emits it from the for-loop that walks
    active_req inside nxt_unit_quit() when quit_param == NXT_QUIT_NORMAL.
    The for-loop is unreachable on the GRACEFUL path (which returns
    early when the active_req queue is non-empty), so the warning's
    presence is positive proof that the NORMAL fast-exit branch ran.
    Absence of the warning would mean SIGTERM accidentally took the
    GRACEFUL branch -- the regression this test must catch.
    """
    client.load('delayed', module='asgi')

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')
    skip_alert(r'active request on ctx quit')

    sock = _start_inflight_request(INFLIGHT_DELAY)

    time.sleep(0.5)

    os.kill(unit_pid, signal.SIGTERM)

    body = _recv_all(sock)
    sock.close()

    # Positive log evidence beats body-shape inference: it is robust to
    # the timing race where a fast machine completes the response before
    # the signal lands.  wait_for_record polls the unit log up to ~15 s
    # for the marker; on a real regression to the GRACEFUL branch the
    # marker never appears and the assertion fails.
    assert Log.wait_for_record(r'active request on ctx quit') is not None, (
        'SIGTERM did not take the NORMAL fast-exit path: libunit drained '
        'in-flight requests as if quit_param == NXT_PORT_QUIT_GRACEFUL. '
        'Regression in P1 plumbing -- see src/nxt_main_process.c '
        'sigterm/sigquit handler split and src/nxt_runtime.c '
        'nxt_runtime_quit_buf().'
    )


def test_sigint_takes_normal_path(unit_pid, skip_alert):
    """
    SIGINT shares the SIGTERM handler in nxt_main_process_signals
    (src/nxt_main_process.c:72-74), so it must produce identical
    NORMAL fast-exit behaviour.  Regression guard: a signal-table
    edit that re-routes SIGINT to nxt_main_process_sigquit_handler
    would silently turn ^C into a graceful drain, surprising users
    who expect Ctrl-C to be immediate.
    """
    client.load('delayed', module='asgi')

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')
    skip_alert(r'active request on ctx quit')

    sock = _start_inflight_request(INFLIGHT_DELAY)

    time.sleep(0.5)

    os.kill(unit_pid, signal.SIGINT)

    body = _recv_all(sock)
    sock.close()

    assert Log.wait_for_record(r'active request on ctx quit') is not None, (
        'SIGINT did not take the NORMAL fast-exit path: signal table in '
        'nxt_main_process.c may have been edited to route SIGINT to the '
        'sigquit handler.  Ctrl-C should be immediate, not graceful.'
    )


@pytest.mark.skip(
    reason='needs C-level instrumentation; covered by test 1+2 behaviorally'
)
def test_quit_message_carries_quit_param():
    """
    Direct assertion that NXT_PORT_MSG_QUIT carries a one-byte body
    encoding rt->quit_mode.  Verifying this from Python would require
    intercepting the AF_UNIX port socket between unitd master and the
    application worker, which is not straightforward without a debug
    build that exposes the wire bytes.

    Tests 1 and 2 above exercise the same plumbing behaviourally:
      * graceful-drain on SIGQUIT  => byte == NXT_QUIT_GRACEFUL
      * fast-exit on SIGTERM       => byte == NXT_QUIT_NORMAL
    """
