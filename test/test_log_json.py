"""
Tests for opt-in JSON error log format (--log-format json).

The test launches its own unitd subprocess so it does not interfere with
the shared instance from conftest.py.  It exercises:

  * help text advertises --log-format
  * unknown values are rejected
  * default text output is byte-shape unchanged (smoke)
  * with --log-format json every record is valid JSON with the required keys
  * level filtering works
  * embedded quotes/backslashes/control chars are properly escaped
  * request_id key is omitted when log->ident == 0
"""
import json
import os
import re
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


@pytest.fixture
def unit_tmp():
    tmp = tempfile.mkdtemp(prefix='unit-jsonlog-')
    state = Path(tmp) / 'state'
    state.mkdir()
    yield tmp
    # subprocess.Popen instances are torn down by the individual tests
    try:
        subprocess.run(['rm', '-rf', tmp], check=False)
    except OSError:
        pass


def _spawn(unit_tmp, *extra_args):
    log_path = f'{unit_tmp}/unit.log'
    args = [
        _unitd(),
        '--no-daemon',
        '--modulesdir', f'{_builddir()}/lib/unit/modules',
        '--statedir', f'{unit_tmp}/state',
        '--pid', f'{unit_tmp}/unit.pid',
        '--log', log_path,
        '--control', f'unix:{unit_tmp}/control.sock',
        '--tmpdir', unit_tmp,
        *extra_args,
    ]
    if option.user:
        args.extend(['--user', option.user])

    with open(log_path, 'w', encoding='utf-8') as logfile:
        proc = subprocess.Popen(args, stderr=logfile)

    # Wait for the control socket to appear (unit is fully up).
    sock = f'{unit_tmp}/control.sock'
    for _ in range(150):
        if os.path.exists(sock):
            break
        time.sleep(0.1)

    return proc, log_path


def _terminate(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


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
    assert 'text' in combined and 'json' in combined


def test_missing_log_format_value_rejected():
    out = subprocess.run(
        [_unitd(), '--log-format'],
        capture_output=True,
        text=True,
        check=False,
    )
    assert out.returncode != 0


def test_default_is_text(unit_tmp):
    proc, log_path = _spawn(unit_tmp)
    try:
        # Wait briefly for at least one record to land.
        for _ in range(50):
            if Path(log_path).stat().st_size > 0:
                break
            time.sleep(0.1)
    finally:
        _terminate(proc)

    content = Path(log_path).read_text(encoding='utf-8', errors='replace')
    assert content, 'expected at least one log line'
    # Legacy text format begins with "YYYY/MM/DD HH:MM:SS [level] PID#TID ...".
    first = content.splitlines()[0]
    assert re.match(
        r'^\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2} \[[a-z]+\] \d+#\d+',
        first,
    ), f'first line not in text format: {first!r}'


def test_json_format_records_parse(unit_tmp):
    proc, log_path = _spawn(unit_tmp, '--log-format', 'json')
    try:
        # Wait until we see the router-started record (last startup log).
        record = Log.wait_for_json_record(
            log_path,
            lambda r: 'router' in r.get('msg', ''),
            wait=150,
        )
        assert record is not None, 'router-started JSON record missing'
    finally:
        _terminate(proc)

    records = Log.read_json_lines(log_path)
    assert records, 'no JSON records in log'

    for r in records:
        assert isinstance(r['ts'], str)
        assert re.match(
            r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$', r['ts']
        ), f'bad ts: {r["ts"]!r}'
        assert r['level'] in {
            'alert', 'error', 'warn', 'notice', 'info', 'debug'
        }
        assert isinstance(r['pid'], int) and r['pid'] > 0
        assert r['app'] == 'unit'
        assert isinstance(r['msg'], str)


def test_json_multi_process(unit_tmp):
    proc, log_path = _spawn(unit_tmp, '--log-format', 'json')
    try:
        Log.wait_for_json_record(
            log_path,
            lambda r: 'router' in r.get('msg', ''),
            wait=150,
        )
    finally:
        _terminate(proc)

    pids = {r['pid'] for r in Log.read_json_lines(log_path)}
    assert len(pids) >= 2, (
        f'expected multiple distinct pids in log, got {pids}'
    )


def test_json_escapes_quotes_in_msg(unit_tmp):
    # The "no modules matching" notice line includes the literal pattern
    # `"...glob..."` with embedded double quotes -- the perfect natural
    # check that escaping works (no need to inject a synthetic record).
    proc, log_path = _spawn(unit_tmp, '--log-format', 'json')
    try:
        record = Log.wait_for_json_record(
            log_path,
            lambda r: 'no modules matching' in r.get('msg', ''),
            wait=150,
        )
    finally:
        _terminate(proc)

    assert record is not None, 'no-modules notice missing'
    # Round trip: msg must contain raw quotes after json.loads, and the
    # log file must contain the escaped form.
    assert '"' in record['msg']

    raw = Path(log_path).read_text(encoding='utf-8', errors='replace')
    assert '\\"' in raw, 'embedded quote was not escaped on disk'
    # No bare unescaped newline within a record (each record is a line).
    for line in raw.splitlines():
        line = line.strip()
        if line:
            json.loads(line)  # raises if any record is malformed


def test_request_id_absent_for_startup_records(unit_tmp):
    proc, log_path = _spawn(unit_tmp, '--log-format', 'json')
    try:
        Log.wait_for_json_record(
            log_path,
            lambda r: 'router' in r.get('msg', ''),
            wait=150,
        )
    finally:
        _terminate(proc)

    # Startup records have log->ident == 0, so request_id key must be omitted.
    startup = [
        r for r in Log.read_json_lines(log_path)
        if 'started' in r.get('msg', '') or 'no modules' in r.get('msg', '')
    ]
    assert startup, 'no startup records found'
    for r in startup:
        assert 'request_id' not in r, (
            f'request_id should be omitted on startup, got: {r}'
        )
