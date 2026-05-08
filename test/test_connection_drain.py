"""
Phase 5 of the FreeUnit graceful-shutdown plan: connection drain with
graceful_timeout escalation.

Background
----------
P1 plumbed NXT_PORT_QUIT_GRACEFUL through the SIGQUIT signal handler so
libunit on the worker side could drain in-flight requests.  P4.5 made
engine->active_connections enumerable.  P5 builds on both: when SIGQUIT
fires on the unitd master, nxt_runtime_quit() now coordinates a drain
state machine on the router/main process side as well, so the TCP
listener and IPC ports stay alive until either every active conn has
finished its current request or graceful_timeout (30 s, hard-coded for
now) elapses.

State machine implemented in src/nxt_runtime.c:

    stop arming -> drain -> grace_timeout -> SIGTERM -> grace_period -> SIGKILL

For P5 the load-bearing transitions are:

  * On entering graceful drain (active_conns_cnt > 0):
        nxt_log NXT_LOG_NOTICE "graceful drain: N active connection(s)"
  * On natural drain completion (active_conns_cnt hits 0 first):
        nxt_log NXT_LOG_NOTICE "graceful drain complete"
  * On graceful_timeout escalation (timer fires before drain done):
        nxt_log NXT_LOG_WARN  "graceful drain timeout: forcing close on N
                               active connection(s)"

These markers are how the tests below distinguish "drain ran and
finished cleanly", "drain ran and was force-escalated", and "drain did
not run at all" (NORMAL fast-exit path).

See roadmap/plan-graceful-shutdown.md (P5) for the full plan.
"""

import os
import re
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
    Mirrors the P1 module's autouse skip: every test here signals the
    unitd master, so the autouse `run` fixture in conftest.py needs
    --restart to rmtree the temp dir on teardown rather than PUT
    /config to a dead daemon.
    """
    if not request.config.getoption('--restart'):
        pytest.skip(
            'test_connection_drain.py signals the unitd master process; '
            'rerun with --restart so conftest.py rmtrees the temp dir '
            'instead of trying /config PUT on a dead daemon'
        )


# Default graceful_timeout in src/nxt_runtime.c — hard-coded for now per
# the P5 follow-up TODO.  Tests that target the timer escalation path
# pick a delay above this; tests that target natural drain pick one
# safely below.
GRACEFUL_TIMEOUT_S = 30

# Recv loop timeout — must exceed GRACEFUL_TIMEOUT_S plus a margin for
# the timeout-escalation case where the server takes the full timer to
# kick the conn loose.
RESPONSE_TIMEOUT = 60


def _open_inflight_request(delay: int) -> socket.socket:
    """
    Open a TCP socket to Unit and send a single ASGI request that takes
    *delay* seconds inside the handler.  ASGI is required (not WSGI)
    because the synchronous WSGI worker would block in time.sleep() and
    never pump libunit's message loop, so a SIGQUIT mid-request would
    queue behind the response and the test would pass for the wrong
    reason.  See test/python/delayed/asgi.py.
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


def _recv_all(sock: socket.socket, deadline: float) -> bytes:
    """
    Drain the socket until EOF, ECONNRESET, or *deadline* (monotonic
    seconds).  Returns whatever bytes arrived.  A b'' return on a
    drain-failure path is the regression signal: the router force-
    closed the conn before the worker's response was relayed.
    """
    chunks = []
    while time.monotonic() < deadline:
        try:
            sock.settimeout(max(0.1, deadline - time.monotonic()))
            data = sock.recv(4096)
        except (ConnectionResetError, socket.timeout):
            break
        if not data:
            break
        chunks.append(data)
    return b''.join(chunks)


def test_sigquit_active_request_drains(unit_pid, skip_alert):
    """
    Positive regression guard: with P5 wired, a SIGQUIT to the unitd
    master while a request is in flight must let the response reach
    the client (status 200).  Without P5 (the P1-only state) the
    router process tears down its TCP listener and IPC ports
    immediately on NXT_PORT_MSG_QUIT and the client gets b'' or a
    truncated response.

    The in-flight delay (~3 s) is comfortably below GRACEFUL_TIMEOUT_S
    so this exercises the natural-drain path, not the timer escalation.

    Uses delayed_complete/asgi.py rather than delayed/asgi.py because
    the latter never sends a final http.response.body event for an
    empty-body GET — useless for byte-stream assertions.
    """
    client.load('delayed_complete', module='asgi')

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')
    skip_alert(r'ASGI Lifespan processing exception')

    inflight_delay = 3
    sock = _open_inflight_request(inflight_delay)

    # Let the ASGI handler enter `await sleep(delay)` before we signal
    # so the QUIT genuinely races the in-flight request.
    time.sleep(0.5)

    os.kill(unit_pid, signal.SIGQUIT)

    deadline = time.monotonic() + RESPONSE_TIMEOUT
    body = _recv_all(sock, deadline)
    sock.close()

    # The exact status line is "HTTP/1.1 200 OK".  Asserting on the
    # ASCII "200" anywhere in the response is robust to header
    # casing and chunked transfer framing.
    assert b'200' in body, (
        f'SIGQUIT did not drain the in-flight request to completion: '
        f'router process exited before the worker response could be '
        f'relayed.  Got body: {body!r}.  Without P5 wired, the router '
        f"closes its TCP listener immediately on QUIT and the client's "
        f'recv() returns b"" or a truncated response.'
    )

    # Belt-and-braces: the drain coordinator should have logged its
    # entry marker.  Absence here means rt->quit_mode was NORMAL when
    # nxt_runtime_quit ran — i.e. P1 plumbing regressed.
    assert Log.wait_for_record(
        r'graceful drain: \d+ active connection'
    ) is not None, (
        'P5 graceful-drain entry marker missing from unit log; '
        'rt->quit_mode == NXT_PORT_QUIT_GRACEFUL was not observed by '
        'nxt_runtime_quit().'
    )

    assert Log.wait_for_record(
        r'graceful drain complete'
    ) is not None, (
        'P5 natural-drain completion marker missing; either '
        'nxt_runtime_drain_conn_completed was never called, or '
        'engine->active_conns_cnt did not reach zero before the '
        'process exited.'
    )


def test_sigquit_drain_timeout_escalates(unit_pid, skip_alert):
    """
    A request whose server-side delay exceeds graceful_timeout must
    cause the runtime to escalate: the timer handler walks
    engine->active_connections and calls nxt_conn_close() on each,
    logging NXT_LOG_WARN with the count.

    By design this test takes ~GRACEFUL_TIMEOUT_S to complete; it is
    the only way to assert the timer actually fires.  Do not shorten
    the delay below the timeout — it would hide regressions where the
    timer never arms.
    """
    client.load('delayed_complete', module='asgi')

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')
    skip_alert(r'ASGI Lifespan processing exception')
    skip_alert(r'graceful drain timeout')

    # 5 s above the timeout: the response would only arrive at t+35s,
    # but the timer fires at t+30s.  The conn is force-closed first.
    inflight_delay = GRACEFUL_TIMEOUT_S + 5
    sock = _open_inflight_request(inflight_delay)

    time.sleep(0.5)

    os.kill(unit_pid, signal.SIGQUIT)

    # Poll the unit log up to graceful_timeout + a few seconds for the
    # WARN marker.  wait_for_record sleeps in 0.1 s increments, so the
    # wait count must cover 35 s comfortably (350+ iterations).
    found = Log.wait_for_record(
        r'graceful drain timeout: forcing close on \d+ active connection',
        wait=400,
    )

    sock.close()

    assert found is not None, (
        f'graceful_timeout escalation marker not seen within '
        f'{GRACEFUL_TIMEOUT_S + 10}s of SIGQUIT.  Either the timer was '
        f'never armed (drain coordinator skipped graceful path), or '
        f'the handler did not run.  Check src/nxt_runtime.c '
        f'nxt_runtime_graceful_timeout_handler() and the timer init in '
        f'nxt_runtime_quit() GRACEFUL branch.'
    )


def test_sigterm_does_not_drain(unit_pid, skip_alert):
    """
    SIGTERM remains the fast-exit path: the GRACEFUL branch in
    nxt_runtime_quit() must not run.  Positive evidence is the same
    libunit "active request on ctx quit" marker that test_graceful_
    reload.py::test_sigterm_drops_inflight_request asserts on for P1
    — its presence proves rt->quit_mode == NXT_PORT_QUIT_NORMAL took
    the original force-close branch, which means P5's drain coordinator
    correctly stayed quiet.
    """
    client.load('delayed', module='asgi')

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')
    skip_alert(r'active request on ctx quit')

    sock = _open_inflight_request(5)

    time.sleep(0.5)

    os.kill(unit_pid, signal.SIGTERM)

    deadline = time.monotonic() + 15
    _recv_all(sock, deadline)
    sock.close()

    assert Log.wait_for_record(r'active request on ctx quit') is not None, (
        'SIGTERM did not take the NORMAL fast-exit path: P1 marker '
        'absent.  P5 drain coordinator may be running on the NORMAL '
        'path (regression in nxt_runtime_quit() guard).'
    )

    # The complement: the drain entry marker must NOT appear, because
    # the GRACEFUL branch should not have run.  This is the new P5-
    # specific assertion that goes beyond P1.
    assert Log.findall(r'graceful drain: \d+ active connection') == [], (
        'P5 drain coordinator ran on the SIGTERM (NORMAL) path; the '
        'rt->quit_mode == NXT_PORT_QUIT_GRACEFUL guard in '
        'nxt_runtime_quit() is broken.'
    )


def _proc_alive(pid: int) -> bool:
    """
    Return True iff /proc/<pid> exists and the process is not a zombie.
    os.kill(pid, 0) returns success on zombies (kill(2) tracks the pid
    until wait(2) reaps), which would mask a real "exited promptly" in
    this test, so we look at /proc/<pid>/stat directly.
    """
    try:
        with open(f'/proc/{pid}/stat', 'r') as f:
            stat = f.read()
    except FileNotFoundError:
        return False
    # Format: "<pid> (<comm>) <state> ..." — state is the third field.
    # comm may contain spaces/parens; split from the right of ')'.
    state = stat.rsplit(')', 1)[1].split()[0]
    return state != 'Z'


def test_sigquit_no_active_exits_fast(unit_pid, skip_alert):
    """
    SIGQUIT with no in-flight request must exit promptly: the drain
    coordinator observes active_conns_cnt == 0, skips the timer arm,
    and posts nxt_runtime_exit immediately.  Cap is ~3 s — anything
    higher means the runtime is waiting on a phantom drain.
    """
    client.load('empty', module='asgi')

    skip_alert(r'process \d+ exited on signal')
    skip_alert(r'sendmsg.+failed')
    skip_alert(r'last message send failed')
    skip_alert(r'ASGI Lifespan processing exception')

    # No client connection — the router has no active conns.
    os.kill(unit_pid, signal.SIGQUIT)

    deadline = time.monotonic() + 3
    exited = False
    while time.monotonic() < deadline:
        if not _proc_alive(unit_pid):
            exited = True
            break
        time.sleep(0.05)

    assert exited, (
        f'unitd master (pid {unit_pid}) did not exit within 3 s of '
        f'SIGQUIT despite no active connections.  Drain coordinator '
        f'is waiting on graceful_timeout for nothing — check the '
        f'active_conns_cnt == 0 fast-path in nxt_runtime_quit().'
    )

    # The drain entry log marker must report zero active conns to
    # confirm we took the fast-exit branch consciously rather than
    # bypassing the GRACEFUL block entirely.
    found = Log.findall(r'graceful drain: (\d+) active connection')
    assert found, (
        'graceful drain entry marker absent on SIGQUIT — GRACEFUL '
        'branch did not run.'
    )
    assert any(int(n) == 0 for n in found), (
        f'graceful drain entry marker reported active conns: {found!r}; '
        f'expected at least one report of 0 (test had no client '
        f'connection open).'
    )
