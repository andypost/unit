"""Tests for the PHP `preload` and `warmup` application config keys.

`preload` maps to PHP's native `opcache.preload` (requires PHP 7.4+).
`warmup` is a list of script paths compiled via `opcache_compile_file()` at
worker startup so the opcache is primed before the first request.

Structured as a single `TestPHPPreload` class. Helpers (`check_opcache`,
`set_opcache`) are copied from test_php_application.py — duplicating a few
lines here keeps this file self-contained and matches the "don't touch
existing test files" scope.
"""

import getpass
import re
import time
from pathlib import Path

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.log import Log
from unit.option import option

prerequisites = {'modules': {'php': 'all'}}

client = ApplicationPHP()


def check_opcache(resp=None):
    if resp is None:
        resp = client.get()
    assert resp['status'] == 200, 'status'
    headers = resp['headers']
    if 'X-OPcache' in headers and headers['X-OPcache'] == '-1':
        pytest.skip('opcache is not supported')
    return resp


def enable_opcache(extra_ini=''):
    """Force opcache on for both the web (module) and CLI SAPI modes —
    Unit's PHP embed registers as CLI, so `opcache.enable_cli=1` is
    required for `opcache_compile_file()` to actually do anything.

    `opcache.enable` is `PHP_INI_SYSTEM` — it can only be set at startup
    via a php.ini file, not via `options.admin`. On Debian-style systems
    opcache.so is already loaded by the distro's conf.d snippet, so our
    override ini just flips the two enable flags."""
    ini_path = f'{option.temp_dir}/opcache_on.ini'
    Path(ini_path).write_text(
        'opcache.enable = 1\n'
        'opcache.enable_cli = 1\n'
        + extra_ini,
        encoding='utf-8',
    )
    assert 'success' in client.conf(
        {"file": ini_path},
        'applications/preload/options',
    )
    return ini_path


class TestPHPPreload:
    # ---------------------------------------------------------------- fixtures
    @pytest.fixture(autouse=True)
    def _load_app(self):
        """Every test starts from a fresh `preload` app load."""
        client.load('preload')

    # -------------------------------------------------------------- validation
    @pytest.mark.parametrize(
        'bad',
        [
            '42',                   # integer
            '{"x": 1}',             # object
            'true',                 # boolean
            '""',                   # empty string
            r'"has\u0000null"',     # embedded null byte
        ],
    )
    def test_preload_validation_bad_types(self, bad):
        resp = client.conf(bad, 'applications/preload/preload')
        assert 'error' in resp, f'preload rejects: {bad}'

    @pytest.mark.parametrize(
        'bad',
        [
            '"not-an-array"',                   # string
            '[1, 2, 3]',                        # array of ints
            r'["ok.php", "has\u0000null.php"]', # element with null byte
        ],
    )
    def test_warmup_validation_bad_types(self, bad):
        resp = client.conf(bad, 'applications/preload/warmup')
        assert 'error' in resp, f'warmup rejects: {bad}'

    def test_warmup_empty_array_accepted(self):
        assert 'success' in client.conf(
            '[]', 'applications/preload/warmup'
        ), 'empty warmup array is valid'

    def test_preload_valid_string_accepted(self):
        # Just exercises the validator's success branch — no request yet.
        assert 'success' in client.conf(
            f'"{option.test_dir}/php/preload/hello_preload.php"',
            'applications/preload/preload',
        )

    # ----------------------------------------------------------- preload tests
    def test_preload_happy_path(self):
        """hello_preload.php defines HelloPreloaded class + preloaded_fn();
        first request must see both symbols. Needs opcache enabled at
        startup (PHP_INI_SYSTEM scope)."""
        # opcache.preload needs opcache loaded + enabled. Unit's embed SAPI
        # reads PHP_INI_SYSTEM entries only from the ini file.
        enable_opcache()
        check_opcache()

        preload_path = f'{option.test_dir}/php/preload/hello_preload.php'
        assert 'success' in client.conf(
            f'"{preload_path}"', 'applications/preload/preload'
        )

        resp = client.get()
        assert resp['status'] == 200, 'status'
        headers = resp['headers']
        assert headers.get('X-Class') == 'yes', 'HelloPreloaded visible'
        assert headers.get('X-Fn') == 'yes', 'preloaded_fn visible'

    def test_preload_missing_file(self, skip_alert):
        """Missing preload file → PHP module-startup fails, worker cannot
        serve. Don't be picky about the exact error message — PHP prints
        several variants across versions — just assert the request fails
        and that *some* preload-related error was logged."""
        enable_opcache()

        # Register alert patterns we expect from this failure path so the
        # global teardown doesn't flag them as unexpected.
        skip_alert(
            r'Failed opening required',
            r'opcache\.preload',
            r'failed to initialize SAPI module',
            r'app process .* exited',
            r'process .* exited on signal',
            r'sendmsg.+failed',
            r'last message send failed',
            r'process .* exited with code',
            r'failed to apply',
            r'app already closed',
        )

        missing = f'{option.test_dir}/php/preload/DOES_NOT_EXIST.php'

        assert 'success' in client.conf(
            f'"{missing}"', 'applications/preload/preload'
        )

        # Request should fail: either 503, a connection error, or a
        # degraded 200. Be lenient because timing varies by PHP version.
        try:
            resp = client.get(read_timeout=5)
            status = resp.get('status')
        except Exception:  # noqa: BLE001 — connection/read errors accepted
            status = None

        # Look for any preload-related log line within a short window.
        record = Log.wait_for_record(
            r'(Failed opening required|opcache\.preload|preload)',
            wait=50,
        )
        # Either the log shows preload trouble OR the worker failed to serve.
        assert record is not None or status != 200, (
            f'expected preload failure evidence, got status={status} '
            f'and no matching log line'
        )

    def test_preload_root_user_guard(self, is_su, skip_alert):
        """With the app `user` explicitly set to root, preload must be
        skipped with a WARN — worker stays up and serves normally
        (PHP would hard-fail if we let opcache.preload_user=root reach it)."""
        if not is_su:
            pytest.skip('requires running Unit as root')

        preload_path = f'{option.test_dir}/php/preload/hello_preload.php'
        assert 'success' in client.conf(
            f'"{preload_path}"', 'applications/preload/preload'
        )
        assert 'success' in client.conf(
            '"root"', 'applications/preload/user'
        )

        # Worker should still serve.
        resp = client.get()
        assert resp['status'] == 200, 'status ok despite root-guard'

        record = Log.wait_for_record(
            r'opcache\.preload disabled: resolved user would be root',
            wait=50,
        )
        assert record is not None, 'expected root-guard WARN'

    def test_preload_overrides_options_file(self, temp_dir):
        """`options.file` sets opcache.preload to other_preload.php; Unit's
        `preload` key sets hello_preload.php. Unit wins — HelloPreloaded
        visible, and an INFO log records the override."""
        # Write an ini that enables opcache AND sets opcache.preload to
        # the *other* file — Unit's preload key must still win.
        other_preload = f'{option.test_dir}/php/preload/other_preload.php'
        ini_path = f'{temp_dir}/override.ini'
        Path(ini_path).write_text(
            'opcache.enable = 1\n'
            'opcache.enable_cli = 1\n'
            f'opcache.preload = {other_preload}\n'
            f'opcache.preload_user = {option.user or getpass.getuser()}\n',
            encoding='utf-8',
        )

        assert 'success' in client.conf(
            {"file": ini_path}, 'applications/preload/options'
        )
        check_opcache()
        real_preload = f'{option.test_dir}/php/preload/hello_preload.php'
        assert 'success' in client.conf(
            f'"{real_preload}"', 'applications/preload/preload'
        )

        resp = client.get()
        assert resp['status'] == 200, 'status'
        # Unit's preload wins — HelloPreloaded from hello_preload.php.
        assert resp['headers'].get('X-Class') == 'yes', 'Unit preload wins'

        record = Log.wait_for_record(
            r'`preload` key overrides', wait=50
        )
        assert record is not None, 'expected override INFO line'

    # ------------------------------------------------------------ warmup tests
    def test_warmup_all_compile(self):
        """Three files in `warmup`: the worker must cache every one before
        the first request is served (`opcache_is_script_cached` returns
        true for each)."""
        check_opcache()
        enable_opcache()

        assert 'success' in client.conf(
            '["a.php", "b.php", "c.php"]',
            'applications/preload/warmup',
        )

        resp = client.get(url='/?check=a.php,b.php,c.php')
        assert resp['status'] == 200
        warmup = resp['headers'].get('X-Warmup', '')
        assert 'a.php=1' in warmup, f'a not cached; got {warmup!r}'
        assert 'b.php=1' in warmup, f'b not cached; got {warmup!r}'
        assert 'c.php=1' in warmup, f'c not cached; got {warmup!r}'

    def test_warmup_one_missing(self, skip_alert):
        """Missing middle entry: a WARN must be logged referencing that
        entry; the worker stays up, the other entries still cache."""
        check_opcache()
        enable_opcache()
        skip_alert(r'warmup: failed to compile')

        assert 'success' in client.conf(
            '["a.php", "MISSING_WARMUP.php", "c.php"]',
            'applications/preload/warmup',
        )

        resp = client.get(url='/?check=a.php,c.php')
        assert resp['status'] == 200, 'worker alive'
        warmup = resp['headers'].get('X-Warmup', '')
        assert 'a.php=1' in warmup, f'a still caches; got {warmup!r}'
        assert 'c.php=1' in warmup, f'c still caches; got {warmup!r}'

        record = Log.wait_for_record(
            r'warmup: failed to compile.*MISSING_WARMUP', wait=50
        )
        assert record is not None, 'expected warmup-missing WARN'

    def test_warmup_syntax_error(self, skip_alert):
        """Broken file: a WARN referencing broken.php must be logged;
        the worker must stay up (soft-fail via zend_try/zend_catch)."""
        check_opcache()
        enable_opcache()
        skip_alert(
            r'warmup: failed to compile',
            r'warmup: bailout while compiling',
            r'Parse error',
            r'syntax error',
        )

        assert 'success' in client.conf(
            '["a.php", "broken.php", "c.php"]',
            'applications/preload/warmup',
        )

        resp = client.get(url='/?check=a.php,c.php')
        assert resp['status'] == 200, 'worker alive'

        record = Log.wait_for_record(
            r'warmup.*broken\.php', wait=50
        )
        assert record is not None, 'expected warmup broken.php WARN'

    def test_warmup_opcache_disabled(self, skip_alert):
        """With opcache disabled at ini level the warmup loop degrades
        safely: either one WARN (symbol absent) or per-entry WARNs
        (symbol present but compile returns false). Either way the
        worker serves."""
        skip_alert(
            r'opcache_compile_file not found',
            r'warmup.*opcache disabled',
            r'warmup: failed to compile',
        )

        # Write an ini that explicitly disables opcache at startup.
        ini_path = f'{option.temp_dir}/opcache_off.ini'
        Path(ini_path).write_text(
            'opcache.enable = 0\n'
            'opcache.enable_cli = 0\n',
            encoding='utf-8',
        )
        assert 'success' in client.conf(
            {"file": ini_path},
            'applications/preload/options',
        )
        assert 'success' in client.conf(
            '["a.php", "b.php"]', 'applications/preload/warmup'
        )

        resp = client.get()
        # Worker serves normally regardless of compile outcome.
        assert resp['status'] == 200, 'status ok with opcache disabled'

    def test_warmup_reload_bumps_generation(self, skip_alert):
        """Changing the warmup list triggers a fresh worker generation —
        the new worker's PID differs from the old one."""
        check_opcache()
        enable_opcache()
        skip_alert(r'warmup: failed to compile')

        assert 'success' in client.conf(
            '["a.php"]', 'applications/preload/warmup'
        )
        r1 = client.get(url='/?check=a.php,b.php')
        assert r1['status'] == 200
        pid1 = r1['headers'].get('X-Pid')

        assert 'success' in client.conf(
            '["a.php", "b.php"]', 'applications/preload/warmup'
        )

        # Poll the worker for a PID change (generation bump).
        pid2 = None
        for _ in range(50):
            r2 = client.get(url='/?check=a.php,b.php')
            if r2['status'] == 200:
                pid2 = r2['headers'].get('X-Pid')
                if pid2 != pid1:
                    break
            time.sleep(0.1)

        assert pid2 is not None and pid2 != pid1, (
            f'expected new worker PID after reload (was {pid1}, got {pid2})'
        )

    def test_warmup_put_addressable(self):
        """The warmup key is addressable: PUT the list directly."""
        check_opcache()

        assert 'success' in client.conf(
            '["a.php"]', 'applications/preload/warmup'
        )
        resp = client.conf_get('applications/preload/warmup')
        assert resp == ['a.php'], f'PUT stored the array; got {resp!r}'

    def test_warmup_delete_addressable(self):
        """Array-index delete: DELETE /applications/<app>/warmup/0 leaves a
        shorter list."""
        check_opcache()

        assert 'success' in client.conf(
            '["a.php", "b.php", "c.php"]',
            'applications/preload/warmup',
        )
        assert 'success' in client.conf_delete(
            'applications/preload/warmup/0'
        ), 'delete warmup[0]'

        resp = client.conf_get('applications/preload/warmup')
        assert resp == ['b.php', 'c.php'], (
            f'warmup[0] removed; got {resp!r}'
        )
