"""
Tests for PHP TrueAsync mode with graceful shutdown.

Background
----------
Commit 58ffa236 (cherry-picked from EdmondDantes/052afd8) adds
nxt_php_quit_handler() to src/nxt_php_sapi.c.  When Unit sends a quit
signal to a PHP worker started in TrueAsync mode, the handler calls
ZEND_ASYNC_SHUTDOWN() so the coroutine scheduler terminates gracefully
instead of leaving the process hanging indefinitely.

Status: TDD — tests are written for code that is not yet complete.
See TODO.md items #1 and #2.  All tests in this file are automatically
skipped (via the require_trueasync autouse fixture) when:
  - The "async"/"entrypoint" config fields are not parsed by Unit, OR
  - nxt_php_extension.c is not built (no \\Unit\\Server PHP class).

Prerequisites
-------------
- PHP 8.5+ (TrueAsync is a PHP 8.5+ feature).
- Unit PHP extension built from nxt_php_extension.c.
- The application config must set "async": true + "entrypoint" to
  activate nxt_php_quit_handler registration in nxt_php_start().

Fixture layout
--------------
  test/php/async_shutdown/entrypoint.php  – minimal OK responder
  test/php/async_mirror/entrypoint.php    – echo request body
  test/php/async_slow/entrypoint.php      – sleeps N seconds; writes
                                            sentinel file when handler starts

Running
-------
  sudo pytest-3 --print-log test/test_php_trueasync.py
"""

import os
import re
import signal
import subprocess
import tempfile
import time

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.option import option

# ──────────────────────────────────────────────────────────────────────────────
# Module-level prerequisites
# ──────────────────────────────────────────────────────────────────────────────

# pytest_generate_tests() reads this and parametrises against each PHP
# version satisfying the filter — only 8.5+ gets TrueAsync support.
prerequisites = {
    'modules': {
        'php': lambda version: version.split('.')[:2] >= ['8', '5'],
    }
}

client = ApplicationPHP()

# ──────────────────────────────────────────────────────────────────────────────
# Constants
# ──────────────────────────────────────────────────────────────────────────────

# Seconds a worker is given to exit cleanly after the quit signal.
# Keep it generous on slow CI runners, but short enough to surface hangs.
SHUTDOWN_TIMEOUT = 15

# Seconds to poll before declaring a worker "failed to start".
WORKER_START_TIMEOUT = 10

# ──────────────────────────────────────────────────────────────────────────────
# Module-level autouse fixture — replaces per-test runtime probes
# ──────────────────────────────────────────────────────────────────────────────

@pytest.fixture(autouse=True)
def require_trueasync():
    """
    Skip every test in this module if the TrueAsync infrastructure is absent.

    Runs before each test body (after the conftest 'run' fixture starts Unit).
    Configures a minimal async application, makes one probe request, and calls
    pytest.skip() if the response is not 200.  Two distinct failure modes are
    distinguished so the skip message is actionable:

    • Config rejected with "error" → "async"/"entrypoint" fields missing from
      nxt_php_app_conf_t (TODO.md #1).  The fix is in nxt_application.h.

    • Config accepted but status != 200 → nxt_php_extension.c is not built,
      so the PHP worker starts but \\Unit\\Server is undefined and the entrypoint
      fails to register a request callback (TODO.md #2).

    The probe config is intentionally minimal (async_shutdown fixture, one
    process) to minimise side-effects.  The test body is responsible for
    loading whatever config it actually needs — client.conf() in the test body
    will replace the probe config before any real assertions run.
    """
    probe_conf = {
        'listeners': {'*:8080': {'pass': 'applications/_trueasync_probe'}},
        'applications': {
            '_trueasync_probe': {
                'type': client.get_application_type(),
                'processes': {'spare': 0},
                'root': f'{option.test_dir}/php/async_shutdown',
                'async': True,
                'entrypoint': 'entrypoint.php',
            }
        },
    }

    r = client.conf(probe_conf)
    if 'error' in r:
        pytest.skip(
            'PHP async config rejected — "async"/"entrypoint" fields are '
            'missing from nxt_php_app_conf_t (see TODO.md #1)'
        )

    if client.get()['status'] != 200:
        pytest.skip(
            'PHP TrueAsync extension not available — '
            'build nxt_php_extension.c and relink the PHP module '
            '(see TODO.md #2)'
        )

    yield  # test body runs here; probe config is replaced by the test itself


# ──────────────────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────────────────

def _async_app_conf(script: str) -> dict:
    """
    Return a Unit application config dict for TrueAsync mode.

    "async": true + "entrypoint" is the combination that activates
    nxt_php_quit_handler registration inside nxt_php_start().
    """
    return {
        'type': client.get_application_type(),
        'processes': {'spare': 0},
        'root': f'{option.test_dir}/php/{script}',
        'async': True,
        'entrypoint': 'entrypoint.php',
    }


def _worker_pids(app_name: str, unit_pid: int) -> list:
    """
    Return PIDs of live worker processes for *app_name*.

    Unit names application workers after the application name, so we
    match the Unit master PID as ppid and the app name in the command.
    """
    output = subprocess.check_output(['ps', 'ax', '-O', 'ppid']).decode()
    return re.findall(
        fr'\s*(\d+)\s+{unit_pid}\s+.*{re.escape(app_name)}',
        output,
    )


def _wait_for_worker(app_name: str, unit_pid: int,
                     timeout: float = WORKER_START_TIMEOUT) -> list:
    """
    Poll until at least one worker for *app_name* appears in /proc.
    Raises AssertionError if no worker appears within *timeout* seconds.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pids = _worker_pids(app_name, unit_pid)
        if pids:
            return pids
        time.sleep(0.1)
    raise AssertionError(
        f'Worker for {app_name!r} did not start within {timeout}s'
    )


def _workers_gone(pids: list, timeout: float = SHUTDOWN_TIMEOUT) -> bool:
    """
    Return True once every PID in *pids* has vanished from /proc.
    Return False if any remain alive after *timeout* seconds.

    Checking /proc/{pid} is more reliable than parsing 'ps' output:
    the entry disappears exactly when the kernel reaps the process,
    with no race between the read and the process exiting.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if all(not os.path.exists(f'/proc/{p}') for p in pids):
            return True
        time.sleep(0.1)
    return False


def _wait_for_file(path: str, timeout: float = WORKER_START_TIMEOUT) -> bool:
    """
    Poll until *path* exists on disk (written by the PHP handler as a
    start sentinel) or *timeout* elapses.  Return True if found in time.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return False


# ──────────────────────────────────────────────────────────────────────────────
# Tests
# ──────────────────────────────────────────────────────────────────────────────


def test_php_trueasync_worker_starts(unit_pid):
    """
    Smoke test: a PHP worker in TrueAsync mode starts and serves requests.

    This is the minimal sanity check for every other test.  If it fails
    after the require_trueasync fixture passed, the most likely cause is
    a config field being silently ignored rather than raising an error
    (e.g. "async" accepted but not wired to c->async in nxt_php_start).
    """
    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/async_shutdown'}},
            'applications': {'async_shutdown': _async_app_conf('async_shutdown')},
        }
    ), 'configuration accepted'

    pids = _wait_for_worker('async_shutdown', unit_pid)
    assert pids, 'at least one worker process is running'

    resp = client.get()
    assert resp['status'] == 200, 'worker responds with 200'
    assert 'OK' in resp['body'], 'response body from async handler'


def test_php_trueasync_graceful_shutdown_on_reconfigure(unit_pid, wait_for_record):
    """
    Core regression test for commit 58ffa236.

    Scenario
    --------
    1. Start a PHP worker in TrueAsync mode.
    2. Confirm it is alive and serving.
    3. Remove the application — Unit's router sends the quit signal to
       the worker, exercising nxt_php_quit_handler() → ZEND_ASYNC_SHUTDOWN().
    4. Assert the worker exits within SHUTDOWN_TIMEOUT seconds.

    Before the fix the worker blocked in nxt_unit_run() indefinitely
    because the scheduler never received a shutdown trigger.
    """
    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/async_app'}},
            'applications': {'async_app': _async_app_conf('async_shutdown')},
        }
    ), 'load async_shutdown configuration'

    # Capture PIDs before reconfiguration so we can track these specific
    # processes — not just any worker that might appear later.
    pids = _wait_for_worker('async_app', unit_pid)

    assert client.get()['status'] == 200, 'worker alive before reconfigure'

    # Remove the application; Unit will deliver the quit signal.
    assert 'success' in client.conf(
        {'listeners': {}, 'applications': {}}
    ), 'empty configuration accepted'

    assert _workers_gone(pids), (
        f'TrueAsync worker(s) {pids} did not exit within {SHUTDOWN_TIMEOUT}s '
        f'— nxt_php_quit_handler may not have fired, or '
        f'ZEND_ASYNC_SHUTDOWN() did not stop the scheduler'
    )

    # Informational: verify Unit logged the quit handler invocation.
    # Only present when Unit is built with debug logging; not a hard failure.
    wait_for_record(r'TrueAsync: quit handler called', wait=5)


def test_php_trueasync_graceful_shutdown_on_sigquit(unit_pid, skip_alert):
    """
    Graceful shutdown when SIGQUIT is delivered to the Unit master.

    This is the normal production shutdown path: systemd ExecStop,
    container runtime "stop", or NGINX-style graceful reload all use SIGQUIT.
    The signal path is: SIGQUIT → master → router → quit callback → ZEND_ASYNC_SHUTDOWN.

    skip_alert suppresses the "exited on signal" log alert that would
    otherwise cause the test-teardown checker to flag a failure.
    """
    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/async_app'}},
            'applications': {'async_app': _async_app_conf('async_shutdown')},
        }
    ), 'load configuration'

    pids = _wait_for_worker('async_app', unit_pid)

    assert client.get()['status'] == 200, 'worker alive'

    for pid in pids:
        skip_alert(fr'process {pid} exited on signal')

    os.kill(unit_pid, signal.SIGQUIT)

    assert _workers_gone(pids), (
        f'TrueAsync worker(s) did not exit within {SHUTDOWN_TIMEOUT}s '
        f'after SIGQUIT → Unit master (pid={unit_pid})'
    )


def test_php_trueasync_no_zombie_after_shutdown(unit_pid, skip_alert):
    """
    Exited TrueAsync workers must be reaped — no zombies.

    A zombie means the master is not calling waitpid() after the quit
    handler returns.  This is a process-management regression independent
    of TrueAsync, but worth asserting here alongside the shutdown tests.
    """
    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/async_app'}},
            'applications': {'async_app': _async_app_conf('async_shutdown')},
        }
    ), 'load configuration'

    pids = _wait_for_worker('async_app', unit_pid)
    for pid in pids:
        skip_alert(fr'process {pid} exited on signal')

    assert 'success' in client.conf(
        {'listeners': {}, 'applications': {}}
    ), 'empty configuration to trigger quit'

    assert _workers_gone(pids), 'workers exited before zombie check'

    out = subprocess.check_output(
        ['ps', 'ax', '-o', 'state', '-o', 'ppid']
    ).decode()
    zombie_ppids = re.findall(r'Z\s+(\d+)', out)
    assert str(unit_pid) not in zombie_ppids, (
        'No zombie processes parented to Unit master after async worker shutdown'
    )


def test_php_trueasync_standard_mode_unaffected():
    """
    Regression guard: nxt_php_sapi.c changes must not break standard PHP.

    Standard mode uses nxt_php_request_handler (not the async variant).
    nxt_php_quit_handler is NOT registered (php_init.callbacks.quit = NULL)
    and the pre-existing shutdown path is used unchanged.
    """
    # Load the plain mirror fixture — no async config involved.
    client.load('mirror')

    resp = client.post(body='hello', headers={'Content-Length': '5'})
    assert resp['status'] == 200, 'standard mode: POST succeeds'
    assert resp['body'] == 'hello', 'standard mode: body echoed'

    assert client.get()['status'] == 200, 'standard mode: GET succeeds'


def test_php_trueasync_mirror_request_body(unit_pid):
    """
    Functional test: async handler reads and echoes the request body.

    Exercises nxt_php_request_handler_async(),
    nxt_php_scope_populate_superglobals(), and the response-write path
    including nxt_php_drain_queue under backpressure.
    """
    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/async_mirror'}},
            'applications': {'async_mirror': _async_app_conf('async_mirror')},
        }
    ), 'load async_mirror configuration'

    _wait_for_worker('async_mirror', unit_pid)

    payload = 'TrueAsync echo test payload'
    resp = client.post(
        body=payload,
        headers={'Content-Length': str(len(payload))},
    )
    assert resp['status'] == 200, 'async mirror: status 200'
    assert resp['body'] == payload, 'async mirror: body echoed correctly'


def test_php_trueasync_multiple_workers_all_exit(unit_pid, skip_alert):
    """
    All worker processes must exit gracefully on shutdown, not just one.

    Each worker runs nxt_php_start() independently, so nxt_php_quit_handler
    must be registered per-worker (not once globally).  ZEND_ASYNC_SHUTDOWN()
    must stop each worker's own scheduler instance.
    """
    conf = _async_app_conf('async_shutdown')
    conf['processes'] = 2   # request two workers explicitly

    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/async_multi'}},
            'applications': {'async_multi': conf},
        }
    ), 'load multi-worker configuration'

    # Wait until both workers are visible in the process table.
    deadline = time.monotonic() + WORKER_START_TIMEOUT
    pids = []
    while time.monotonic() < deadline:
        pids = _worker_pids('async_multi', unit_pid)
        if len(pids) >= 2:
            break
        time.sleep(0.1)

    assert len(pids) >= 2, (
        f'Expected ≥2 TrueAsync workers, got {len(pids)}: {pids}'
    )

    for pid in pids:
        skip_alert(fr'process {pid} exited on signal')

    assert 'success' in client.conf(
        {'listeners': {}, 'applications': {}}
    ), 'empty config to stop all workers'

    still_alive = [p for p in pids if os.path.exists(f'/proc/{p}')]
    assert _workers_gone(pids), (
        f'Not all TrueAsync workers exited within {SHUTDOWN_TIMEOUT}s. '
        f'Still alive: {still_alive}'
    )


def test_php_trueasync_inflight_request_completes(unit_pid, skip_alert, tmp_path):
    """
    In-flight requests must complete before the worker exits on shutdown.

    Approach
    --------
    1. Start the slow entrypoint (?sleep=2).  The handler writes a sentinel
       file to *signal_file* the moment it begins executing — before sleeping.
    2. Poll *signal_file* instead of using time.sleep(): once the file exists
       the handler is definitively in-flight inside the PHP coroutine.
    3. Issue the reconfigure (removes the app, triggers the quit signal).
    4. Wait for curl to finish and assert the response is 200 "done".

    The slow request is made via curl in a subprocess rather than a thread:
    subprocess.Popen avoids GIL contention and gives an independent TCP
    connection not shared with the client used for conf() calls.

    NOTE: This test asserts cooperative-shutdown semantics — that
    ZEND_ASYNC_SHUTDOWN() lets active coroutines run to completion.
    If the TrueAsync scheduler cancels coroutines on shutdown instead,
    this test will fail with a connection error or non-200 status; see
    TODO.md #4 for the discussion and resolution path.
    """
    assert 'success' in client.conf(
        {
            'listeners': {'*:8080': {'pass': 'applications/async_slow'}},
            'applications': {'async_slow': _async_app_conf('async_slow')},
        }
    ), 'load async_slow configuration'

    pids = _wait_for_worker('async_slow', unit_pid)
    for pid in pids:
        skip_alert(fr'process {pid} exited on signal')

    # Build the sentinel file path in /tmp rather than in pytest's tmp_path.
    #
    # tmp_path is owned by the user running pytest (typically root in CI),
    # but the Unit PHP worker runs under its own user (e.g. "unit").  The
    # worker process would get EACCES writing to a 0o700 directory it does
    # not own.  /tmp is world-writable (mode 1777) on every POSIX system,
    # so no chmod dance is needed.
    #
    # We include the pytest tmp_path basename (unique per-test) to avoid
    # collisions when multiple test runs overlap, and clean up explicitly
    # at the end of the test.
    signal_file = os.path.join(
        tempfile.gettempdir(),
        f'unit_async_{os.path.basename(str(tmp_path))}',
    )

    # Launch curl in a subprocess: one independent TCP connection,
    # no GIL contention, clean resource lifetime via context manager.
    url = f'http://localhost:8080/?sleep=2&signal={signal_file}'
    with subprocess.Popen(
        ['curl', '-s', '--max-time', '25', url],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    ) as proc:

        # Wait for the PHP handler to create the sentinel file.
        # This is the authoritative signal that the coroutine is sleeping
        # inside the handler — not just enqueued in the kernel or router.
        if not _wait_for_file(signal_file):
            proc.terminate()
            pytest.fail(
                f'Slow request did not reach the PHP handler within '
                f'{WORKER_START_TIMEOUT}s (sentinel file not created). '
                f'Check that async_slow/entrypoint.php writes ?signal= path.'
            )

        # The request is now definitively in-flight.  Trigger shutdown.
        assert 'success' in client.conf(
            {'listeners': {}, 'applications': {}}
        ), 'reconfigure while request is in flight'

        # Wait for curl to finish; generous timeout covers sleep=2 plus
        # scheduler shutdown overhead.
        try:
            stdout, _ = proc.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            pytest.fail('curl did not finish within 30s after shutdown trigger')

    body = stdout.decode()

    assert proc.returncode == 0, (
        f'curl exited with code {proc.returncode} — connection was dropped '
        f'before the response arrived. ZEND_ASYNC_SHUTDOWN() may cancel '
        f'in-flight coroutines rather than letting them complete (TODO.md #4).'
    )
    assert 'done' in body, (
        f'Unexpected response body: {body!r}. '
        f'Expected "done" from async_slow handler.'
    )

    assert _workers_gone(pids), (
        f'Worker(s) did not exit within {SHUTDOWN_TIMEOUT}s after the '
        f'in-flight request completed.'
    )

    # Remove the sentinel file written by the PHP worker.  It lives in /tmp
    # (not in pytest's tmp_path) so pytest cannot clean it up automatically.
    try:
        os.unlink(signal_file)
    except FileNotFoundError:
        pass  # handler may not have written it if the test failed early
