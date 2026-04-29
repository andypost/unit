"""
Tests for opt-in JSON error log format (--log-format json).

Launches a dedicated unitd subprocess so the assertions do not interfere
with conftest.py's shared instance.  Cleanup uses a process group to
make sure router/controller/discovery children die even if SIGTERM
propagation is unreliable under sudo.
"""
import json
import os
import re
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

from unit.log import Log
from unit.option import option


def _builddir():
    return f'{option.current_dir}/build'


def _unitd():
    return f'{_builddir()}/sbin/unitd'


def _wait_pgid_gone(pgid, deadline):
    while time.time() < deadline:
        try:
            os.killpg(pgid, 0)
        except ProcessLookupError:
            return True
        time.sleep(0.05)
    return False


def _hard_kill(proc):
    """Terminate the unitd process group, escalating to SIGKILL on timeout."""
    try:
        pgid = os.getpgid(proc.pid)
    except ProcessLookupError:
        return

    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return

    if _wait_pgid_gone(pgid, time.time() + 5):
        proc.wait(timeout=1)
        return

    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        pass

    _wait_pgid_gone(pgid, time.time() + 5)
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass


@pytest.fixture(scope='module')
def json_unit_log():
    """Spawn a unitd in JSON mode, capture its log, then fully tear it
    down BEFORE yielding so conftest's autouse _check_processes hook
    never observes our subprocess tree alongside its own."""
    tmp = tempfile.mkdtemp(prefix='unit-jsonlog-')
    Path(f'{tmp}/state').mkdir()
    log_path = f'{tmp}/unit.log'

    args = [
        _unitd(),
        '--no-daemon',
        '--modulesdir', f'{_builddir()}/lib/unit/modules',
        '--statedir', f'{tmp}/state',
        '--pid', f'{tmp}/unit.pid',
        '--log', log_path,
        '--control', f'unix:{tmp}/control.sock',
        '--tmpdir', tmp,
        '--log-format', 'json',
    ]
    if option.user:
        args.extend(['--user', option.user])

    proc = None
    try:
        with open(log_path, 'w', encoding='utf-8') as logfile:
            proc = subprocess.Popen(
                args, stderr=logfile, start_new_session=True
            )

        # Wait until the log has accumulated startup records OR the
        # control socket appears -- whichever happens first, up to ~30s.
        sock = f'{tmp}/control.sock'
        deadline = time.time() + 30
        while time.time() < deadline:
            if os.path.exists(sock):
                break
            try:
                content = Path(log_path).read_text(
                    encoding='utf-8', errors='replace'
                )
                if 'router' in content or 'controller' in content:
                    break
            except OSError:
                pass
            time.sleep(0.1)

        # Settle so additional records can land.
        time.sleep(0.5)

        # Tear down BEFORE yielding so the log is static for tests and no
        # orphan processes remain to confuse conftest's _check_processes.
        _hard_kill(proc)
        proc = None

        yield log_path

    finally:
        if proc is not None:
            _hard_kill(proc)
        try:
            subprocess.run(['rm', '-rf', tmp], check=False)
        except OSError:
            pass


# --- offline (no unitd launch) ---


def test_help_advertises_log_format():
    out = subprocess.run(
        [_unitd(), '--help'], capture_output=True, text=True, check=False
    )
    combined = out.stdout + out.stderr
    assert '--log-format' in combined
    assert '"text" or "json"' in combined


def test_bad_log_format_rejected():
    out = subprocess.run(
        [_unitd(), '--log-format', 'yaml'],
        capture_output=True,
        text=True,
        check=False,
    )
    assert out.returncode != 0
    combined = out.stdout + out.stderr
    assert 'log-format' in combined


def test_missing_log_format_value_rejected():
    out = subprocess.run(
        [_unitd(), '--log-format'],
        capture_output=True,
        text=True,
        check=False,
    )
    assert out.returncode != 0


# --- online (single shared unitd via fixture) ---


def test_records_are_valid_json(json_unit_log):
    records = Log.read_json_lines(json_unit_log)
    assert records, 'no JSON records emitted'

    for r in records:
        assert isinstance(r['ts'], str)
        assert re.match(
            r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$', r['ts']
        ), f'bad ts: {r["ts"]!r}'
        assert r['level'] in {
            'alert', 'error', 'warn', 'notice', 'info', 'debug'
        }, f'unexpected level: {r["level"]!r}'
        assert isinstance(r['pid'], int) and r['pid'] > 0
        assert r['app'] == 'unit'
        assert isinstance(r['msg'], str)


def test_multi_process_pids(json_unit_log):
    records = Log.read_json_lines(json_unit_log)
    pids = {r['pid'] for r in records}
    # main + at least one of (router, controller, discovery)
    assert len(pids) >= 2, f'expected >= 2 distinct pids, got {pids}'


def test_embedded_quotes_escaped(json_unit_log):
    """The 'no modules matching' record contains a literal quoted glob,
    which is the natural existence-proof that escape is correct."""
    raw = Path(json_unit_log).read_text(encoding='utf-8', errors='replace')
    # Every non-empty line must round-trip through json.loads.
    for line in raw.splitlines():
        line = line.strip()
        if line:
            json.loads(line)

    records = Log.read_json_lines(json_unit_log)
    quoted = [r for r in records if 'no modules matching' in r.get('msg', '')]
    if quoted:
        # Original had embedded quotes; on disk they must be escaped.
        assert '"' in quoted[0]['msg']
        assert '\\"' in raw, 'embedded quote was not escaped on disk'


def test_request_id_absent_on_startup(json_unit_log):
    records = Log.read_json_lines(json_unit_log)
    startup = [
        r for r in records
        if 'started' in r.get('msg', '') or 'no modules' in r.get('msg', '')
    ]
    assert startup, 'no startup records found'
    for r in startup:
        assert 'request_id' not in r, (
            f'request_id should be omitted on startup, got: {r}'
        )
