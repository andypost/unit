"""Tests for the top-level "schedules" object (docs/adr/0004-schedules.md).

The validation cases only PUT configurations; no schedule has to fire for
them.  An application is needed because "pass" must name one that exists.

The run cases use test/python/schedule, which appends a JSON record to the
file named by SCHEDULE_LOG when a request starts and when it ends, and can be
told to sleep or to answer with another status through the query string.
"""

import json
import re
import time

import pytest

from unit.applications.lang.python import ApplicationPython
from unit.log import Log
from unit.option import option

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()


def base_conf(schedules):
    empty = f'{option.test_dir}/python/empty'
    targets = f'{option.test_dir}/python/targets'

    return {
        "listeners": {"*:8080": {"pass": "routes/main"}},
        "routes": {"main": [{"action": {"pass": "applications/empty"}}]},
        "upstreams": {"up": {"servers": {"127.0.0.1:8081": {}}}},
        "applications": {
            "empty": {
                "type": client.get_application_type(),
                "processes": {"spare": 0},
                "path": empty,
                "working_directory": empty,
                "module": "wsgi",
            },
            "targets": {
                "type": client.get_application_type(),
                "processes": {"spare": 0},
                "path": targets,
                "working_directory": targets,
                "targets": {
                    "one": {"module": "wsgi", "callable": "application_200"},
                },
            },
        },
        "schedules": schedules,
    }


def put(schedules):
    return client.conf(base_conf(schedules))


def schedule(**kwargs):
    s = {"pass": "applications/empty", "uri": "/cron", "interval": 60}
    s.update(kwargs)

    for key in [k for k, v in s.items() if v is None]:
        del s[key]

    return {"cron": s}


def assert_error(resp, detail=None, path=None):
    assert 'error' in resp, resp

    if detail is not None:
        assert detail in resp['detail'], resp

    if path is not None:
        assert resp['location']['path'] == path, resp


def test_schedules_validation_minimal():
    assert 'success' in put(schedule())
    assert client.conf_get('schedules/cron/interval') == 60


def test_schedules_validation_empty():
    assert 'success' in put({})


def test_schedules_validation_all_fields():
    assert 'success' in put(
        schedule(
            uri="/cron/SECRET_KEY?x=1&y=%2F",
            interval=300,
            jitter=15,
            timeout=240,
            overlap="skip",
            headers={"Host": "example.org", "X-Cron": "1"},
            run_on_start=False,
        )
    )

    assert 'success' in put(schedule(overlap="queue", run_on_start=True))


def test_schedules_validation_target():
    assert 'success' in put(schedule(**{"pass": "applications/targets/one"}))


def test_schedules_validation_limits():
    assert 'success' in put(schedule(interval=1, jitter=0, timeout=1))
    assert 'success' in put(schedule(interval=2147483, timeout=2147483))
    assert 'success' in put(schedule(interval=1073741, jitter=1073741))


def test_schedules_validation_via_path():
    assert 'success' in put({})
    assert 'success' in client.conf(
        {"pass": "applications/empty", "uri": "/", "interval": 5},
        'schedules/x',
    )
    assert_error(client.conf({"uri": "/", "interval": 5}, 'schedules/y'))


@pytest.mark.parametrize('missing', ['pass', 'uri', 'interval'])
def test_schedules_validation_required(missing):
    assert_error(
        put(schedule(**{missing: None})),
        f'Required parameter "{missing}" is missing',
        f'/schedules/cron/{missing}',
    )


@pytest.mark.parametrize(
    'value',
    [
        'routes',
        'routes/main',
        'upstreams/up',
        'applications',
        'applications/missing',
        'applications/targets/missing',
        'applications/empty/extra/segment',
        'applications/$host',
        '$uri',
        'empty',
    ],
)
def test_schedules_validation_pass_invalid(value):
    assert_error(put(schedule(**{"pass": value})), path='/schedules/cron/pass')


def test_schedules_validation_pass_messages():
    assert_error(
        put(schedule(**{"pass": "routes/main"})),
        'must be "applications/<name>"',
    )
    assert_error(
        put(schedule(**{"pass": "applications/$host"})),
        'must not contain variables',
    )


def test_schedules_validation_pass_type():
    assert_error(put(schedule(**{"pass": 1})), path='/schedules/cron/pass')


@pytest.mark.parametrize(
    'uri',
    [
        '',
        'cron',
        '*',
        'http://example.org/cron',
        '/cron job',
        '/cron\tjob',
        '/cron#frag',
        '/cron\x01',
        '/cron\x7f',
        '/crön',
        pytest.param('/' + 'a' * 4096, id='too-long'),
    ],
)
def test_schedules_validation_uri_invalid(uri):
    assert_error(put(schedule(uri=uri)), path='/schedules/cron/uri')


def test_schedules_validation_uri_max():
    assert 'success' in put(schedule(uri='/' + 'a' * 4095))


@pytest.mark.parametrize('field', ['interval', 'timeout'])
@pytest.mark.parametrize('value', [0, -1, 2147484, 1.5, "60", None, True])
def test_schedules_validation_seconds_invalid(field, value):
    s = schedule()
    s['cron'][field] = value

    assert_error(put(s), path=f'/schedules/cron/{field}')


@pytest.mark.parametrize('value', [-1, 1.5, "1"])
def test_schedules_validation_jitter_invalid(value):
    assert_error(put(schedule(jitter=value)), path='/schedules/cron/jitter')


def test_schedules_validation_jitter_over_interval():
    assert_error(
        put(schedule(interval=10, jitter=11)),
        'must not exceed "interval"',
        '/schedules/cron/jitter',
    )


def test_schedules_validation_jitter_sum():
    assert_error(
        put(schedule(interval=2000000, jitter=1000000)),
        'The sum of "interval" and "jitter"',
        '/schedules/cron/jitter',
    )


@pytest.mark.parametrize('value', ['SKIP', 'wait', '', 1, True])
def test_schedules_validation_overlap_invalid(value):
    assert_error(put(schedule(overlap=value)), path='/schedules/cron/overlap')


@pytest.mark.parametrize('value', [1, "true", None])
def test_schedules_validation_run_on_start_invalid(value):
    s = schedule()
    s['cron']['run_on_start'] = value

    assert_error(put(s), path='/schedules/cron/run_on_start')


@pytest.mark.parametrize(
    'name',
    [
        'Content-Length',
        'content-length',
        'Transfer-Encoding',
        'Connection',
        'Upgrade',
        'Keep-Alive',
        'TE',
        'te',
        'Expect',
        'Sec-WebSocket-Key',
        'sec-websocket-version',
    ],
)
def test_schedules_validation_header_reserved(name):
    assert_error(
        put(schedule(headers={name: "x"})),
        'cannot be set by a schedule',
        f'/schedules/cron/headers/{name}',
    )


@pytest.mark.parametrize(
    'name', ['', 'Bad Name', 'Bad:Name', 'X-é', 'X\r\nY', '(x)']
)
def test_schedules_validation_header_name_invalid(name):
    assert_error(put(schedule(headers={name: "x"})))


@pytest.mark.parametrize(
    'value', ['a\rb', 'a\nb', 'a\x00b', 'a\x7fb', 1, None, ["x"]]
)
def test_schedules_validation_header_value_invalid(value):
    assert_error(
        put(schedule(headers={"X-Test": value})),
        path='/schedules/cron/headers/X-Test',
    )


def test_schedules_validation_header_value_tab():
    assert 'success' in put(schedule(headers={"X-Test": "a\tb c"}))


def test_schedules_validation_headers_size():
    # Each field is "name: value\r\n": 4 bytes of framing.
    ok = {f'X-{i:02d}': 'v' * 1990 for i in range(4)}
    assert 'success' in put(schedule(headers=ok))

    big = {f'X-{i:02d}': 'v' * 2100 for i in range(4)}
    assert_error(
        put(schedule(headers=big)),
        'must not exceed 8192 bytes',
        '/schedules/cron/headers',
    )


def test_schedules_validation_headers_type():
    assert_error(
        put(schedule(headers=["Host: x"])), path='/schedules/cron/headers'
    )


def test_schedules_validation_unknown_member():
    assert_error(
        put(schedule(method="POST")),
        'Unknown parameter "method"',
        '/schedules/cron',
    )


def test_schedules_validation_schedule_type():
    assert_error(put({"cron": "applications/empty"}), path='/schedules/cron')
    assert_error(client.conf([], 'schedules'))


def test_schedules_validation_name():
    s = schedule()['cron']

    assert 'success' in put({"drupal cron.1": s})
    assert_error(put({"": s}), 'must be 1 to 128 bytes')
    assert_error(put({"a" * 129: s}), 'must be 1 to 128 bytes')
    assert 'success' in put({"a" * 128: s})
    assert_error(put({"cr\non": s}), 'printable ASCII')
    assert_error(put({"crön": s}), 'printable ASCII')


def test_schedules_validation_several():
    s = schedule()['cron']
    t = dict(s, **{"pass": "applications/targets/one", "interval": 5})

    assert 'success' in put({"a": s, "b": t})
    assert_error(put({"a": s, "b": dict(t, interval=0)}), path='/schedules/b/interval')


def test_schedules_validation_app_removed():
    conf = base_conf(schedule())
    conf['routes']['main'][0]['action']['pass'] = 'applications/targets/one'

    assert 'success' in client.conf(conf)

    # Only the schedule refers to "empty", and it cannot go away under it.
    assert_error(
        client.conf_delete('applications/empty'), path='/schedules/cron/pass'
    )

    assert 'success' in client.conf_delete('schedules/cron')
    assert 'success' in client.conf_delete('applications/empty')


# Runs.  Timings are loose on purpose: the timer bias is 50 ms and a CI box
# can stall; what is asserted is ordering and bounds, not exact spacing.


def run_log():
    return f'{option.temp_dir}/schedule.log'


def run_conf(schedules, listeners=True, limits=None, extra=None):
    path = f'{option.test_dir}/python/schedule'

    app = {
        "type": client.get_application_type(),
        "processes": {"max": 4, "spare": 0},
        "path": path,
        "working_directory": path,
        "module": "wsgi",
        "environment": {"SCHEDULE_LOG": run_log()},
    }

    if limits is not None:
        app["limits"] = limits

    conf = {
        "listeners": (
            {"*:8080": {"pass": "applications/schedule"}} if listeners else {}
        ),
        "applications": {"schedule": app},
        "schedules": schedules,
    }

    if extra is not None:
        conf.update(extra)

    return conf


def run_schedule(**kwargs):
    s = {"pass": "applications/schedule", "uri": "/cron", "interval": 1}
    s.update(kwargs)

    return {"cron": s}


def records(event=None):
    try:
        with open(run_log(), encoding='utf-8') as f:
            recs = [json.loads(line) for line in f if line.strip()]

    except FileNotFoundError:
        return []

    return [r for r in recs if event is None or r['event'] == event]


def wait_for_starts(n, timeout):
    end = time.monotonic() + timeout

    while time.monotonic() < end:
        starts = records('start')

        if len(starts) >= n:
            return starts

        time.sleep(0.1)

    pytest.fail(f'{len(records("start"))} run(s) started, expected {n}')


def assert_no_concurrency():
    """Every run ended before the next one started."""

    running = 0

    for rec in records():
        running += 1 if rec['event'] == 'start' else -1

        assert running in (0, 1), 'two runs at once'


def put_run(schedules, **kwargs):
    assert 'success' in client.conf(run_conf(schedules, **kwargs))


def test_schedules_run_fires():
    put_run(
        run_schedule(
            uri="/cron/SECRET_KEY?x=1",
            headers={"Host": "example.org", "X-Cron": "yes"},
        )
    )

    starts = wait_for_starts(3, 10)

    for rec in starts:
        assert rec['method'] == 'GET'
        assert rec['uri'] == '/cron/SECRET_KEY?x=1'
        assert rec['path'] == '/cron/SECRET_KEY'
        assert rec['query'] == 'x=1'
        assert rec['host'] == 'example.org'
        assert rec['server_name'] == 'example.org'
        assert rec['x_cron'] == 'yes'
        assert rec['user_agent'] == 'FreeUnit-Schedule/cron'
        assert rec['server_port'] == '80'
        assert rec['remote_addr'] == '127.0.0.1'

    gaps = [b['time'] - a['time'] for a, b in zip(starts, starts[1:])]
    assert all(0.7 < gap < 2.5 for gap in gaps), gaps

    # The info line shows the URI up to its last "/" only.
    lines = Log.findall(r'.*\[info\].*schedule "cron" run \d+: GET .*')
    assert lines, 'no run was logged'
    assert all('/cron/... -> 200 in ' in line for line in lines), lines
    assert not any('SECRET_KEY' in line for line in lines), lines


def test_schedules_run_defaults():
    # No Host: server_name falls back to "localhost", as for a client.
    put_run(run_schedule(uri="/"))

    rec = wait_for_starts(1, 5)[0]

    assert rec['host'] is None
    assert rec['server_name'] == 'localhost'
    assert rec['user_agent'] == 'FreeUnit-Schedule/cron'


def test_schedules_run_user_agent():
    put_run(run_schedule(headers={"User-Agent": "cron/1"}))

    assert wait_for_starts(1, 5)[0]['user_agent'] == 'cron/1'


def test_schedules_run_on_start():
    begin = time.time()

    put_run(run_schedule(interval=60, run_on_start=True))

    rec = wait_for_starts(1, 5)[0]

    assert rec['time'] - begin < 2.5
    assert Log.findall(r'schedule "cron": first run in \d+ ms')

    time.sleep(1.5)
    assert len(records('start')) == 1, 'ran again before the interval'


def test_schedules_run_not_on_start():
    put_run(run_schedule(interval=3))

    time.sleep(2)
    assert records('start') == [], 'ran before the interval'

    wait_for_starts(1, 5)


def test_schedules_run_jitter_bounds():
    put_run(run_schedule(interval=1, jitter=1))

    starts = wait_for_starts(4, 15)

    gaps = [b['time'] - a['time'] for a, b in zip(starts, starts[1:])]
    assert all(0.7 < gap < 2.7 for gap in gaps), gaps


def test_schedules_run_overlap_skip():
    put_run(run_schedule(uri="/?sleep=2.5", overlap="skip", timeout=10))

    wait_for_starts(2, 10)
    time.sleep(1)

    assert_no_concurrency()

    skips = Log.findall(
        r'schedule "cron": run skipped, the previous one is still running'
    )
    assert len(skips) >= 2, skips


def test_schedules_run_overlap_queue():
    put_run(run_schedule(uri="/?sleep=2.5", overlap="queue", timeout=10))

    wait_for_starts(3, 12)

    assert_no_concurrency()

    # A queued run starts as soon as the previous one ends, not on the next
    # tick; and the due runs coalesced instead of piling up.
    recs = records()
    ends = [r['time'] for r in recs if r['event'] == 'end']
    starts = [r['time'] for r in recs if r['event'] == 'start']

    for end, start in zip(ends, starts[1:]):
        assert start - end < 0.8, (end, start)

    assert not Log.findall(r'run skipped')


def test_schedules_run_timeout():
    put_run(run_schedule(uri="/?sleep=3", interval=2, timeout=1))

    assert Log.wait_for_record(
        r'schedule "cron" run 1: GET /\.\.\. timed out after \d+ ms'
    )

    # The timeout freed the schedule: the next run is dispatched on time,
    # not skipped, although the first request still executes in its
    # abandoned worker.  (That worker still counts towards "processes", so
    # run 2 waits for capacity and may time out in turn: ADR 0004, R5.)
    assert Log.wait_for_record(r'schedule "cron" run 2: GET /\.\.\. ')
    assert not Log.findall(r'run skipped')

    # The abandoned worker still finishes its request.
    time.sleep(1.5)
    assert len(records('end')) >= 1


def test_schedules_run_status():
    put_run(run_schedule(uri="/?status=500"))

    assert Log.wait_for_record(
        r'\[warn\].*schedule "cron" run 1: GET /\.\.\. -> 500 in \d+ ms: '
        r'"ran /\?status=500\."'
    )


def test_schedules_run_app_timeout():
    # The application's own "limits" answer 503 first.
    put_run(
        run_schedule(uri="/?sleep=3", interval=10, run_on_start=True),
        limits={"timeout": 1},
    )

    assert Log.wait_for_record(
        r'\[warn\].*schedule "cron" run 1: GET /\.\.\. -> 503 in \d+ ms'
    )


def test_schedules_run_no_listeners():
    # With no listener nothing but the schedule holds the configuration.
    put_run(run_schedule(), listeners=False)

    wait_for_starts(3, 10)


def test_schedules_run_reconfigure_running():
    put_run(run_schedule(uri="/?sleep=2.5", overlap="skip", timeout=10))

    wait_for_starts(1, 5)

    # A new configuration while the run is in flight: the state carries over
    # by name, so the schedule still knows a run is going on.
    put_run(
        run_schedule(uri="/?sleep=2.5", overlap="skip", jitter=1, timeout=10)
    )
    put_run(
        run_schedule(
            uri="/?sleep=2.5&v=2", overlap="skip", jitter=1, timeout=10
        )
    )

    time.sleep(4)

    assert_no_concurrency()
    assert Log.findall(r'run skipped'), 'the running state was lost'

    starts = records('start')
    assert starts[0]['uri'] == '/?sleep=2.5'
    assert any(r['uri'] == '/?sleep=2.5&v=2' for r in starts[1:]), starts


def test_schedules_run_reconfigure_keeps_clock():
    put_run(run_schedule(interval=3))

    time.sleep(1.5)

    # Changing "uri" only must not restart the 3 s wait.
    put_run(run_schedule(interval=3, uri="/v2"))

    rec = wait_for_starts(1, 5)[0]
    assert rec['uri'] == '/v2'
    assert Log.findall(r'schedule "cron": first run in') == [
        'schedule "cron": first run in'
    ]


def test_schedules_run_remove():
    put_run(run_schedule(uri="/?sleep=2", timeout=10))

    wait_for_starts(1, 5)

    conf = run_conf({})
    assert 'success' in client.conf(conf)

    # The run in flight completes and is reported; nothing starts after.
    assert Log.wait_for_record(r'schedule "cron" run 1: GET /\.\.\. -> 200')
    assert Log.findall(r'schedule "cron": removed, the run in progress')

    n = len(records('start'))
    time.sleep(2.5)
    assert len(records('start')) == n


def test_schedules_run_rename():
    put_run(run_schedule())
    wait_for_starts(1, 5)

    s = run_schedule()
    put_run({"other": s["cron"]})

    assert Log.wait_for_record(r'schedule "other" run 1: GET')
    assert Log.findall(r'schedule "cron": removed')


def test_schedules_run_access_log():
    put_run(
        run_schedule(uri="/logged"),
        extra={"access_log": f'{option.temp_dir}/access.log'},
    )

    assert Log.wait_for_record(
        r'127\.0\.0\.1 - - \[.+\] "GET /logged HTTP/1\.1" 200 \d+ "-" '
        r'"FreeUnit-Schedule/cron"',
        'access.log',
    )


def test_schedules_run_listen_threads():
    put_run(
        run_schedule(uri="/?sleep=1.5", timeout=10),
        extra={"settings": {"listen_threads": 4}},
    )

    wait_for_starts(1, 5)

    # Fewer threads while a run is in flight: its engine may be the one
    # that goes, and it must stay until the run has ended.
    assert 'success' in client.conf({"listen_threads": 1}, 'settings')

    assert Log.wait_for_record(r'schedule "cron" run 1: GET /\.\.\. -> 200')

    wait_for_starts(3, 8)

    assert 'success' in client.conf({"listen_threads": 2}, 'settings')

    n = len(records('start'))
    wait_for_starts(n + 2, 8)


def test_schedules_run_several():
    put_run(
        {
            "a": {"pass": "applications/schedule", "uri": "/a", "interval": 1},
            "b": {"pass": "applications/schedule", "uri": "/b", "interval": 1},
        }
    )

    wait_for_starts(6, 10)

    uris = [r['uri'] for r in records('start')]
    assert uris.count('/a') >= 2 and uris.count('/b') >= 2, uris


def test_schedules_run_listener_unaffected():
    put_run(run_schedule(uri="/?sleep=0.2"))

    for _ in range(10):
        assert client.get()['status'] == 200

    end = time.monotonic() + 5

    while time.monotonic() < end:
        ua = [r['user_agent'] for r in records('start')]

        if ua.count('FreeUnit-Schedule/cron') >= 2:
            break

        time.sleep(0.1)

    assert ua.count('FreeUnit-Schedule/cron') >= 2, ua
    assert ua.count(None) == 10, ua


def test_schedules_run_lifecycle():
    """Every configuration, state and run a schedule creates is released.

    The evidence is the --debug log: the sanitizer build runs with leak
    detection off (the forking daemon makes it useless), so a configuration
    kept alive for ever by a lost reference would pass unnoticed otherwise.
    """

    for i in range(3):
        put_run(run_schedule(uri=f'/?sleep=1.2&v={i}', timeout=10))
        wait_for_starts(i + 1, 5)

    put_run(run_schedule(uri='/?sleep=1.2&v=3', timeout=10), listeners=False)
    time.sleep(0.5)

    # No schedule left; the run in flight still completes.
    assert 'success' in client.conf(run_conf({}))
    assert Log.wait_for_record(r'schedule "cron": removed')
    time.sleep(2)

    log = Log.read()

    if '[debug]' not in log:
        pytest.skip('the lifecycle evidence needs a --debug build')

    posted = re.findall(r'schedule "cron": run (\d+) posted', log)
    reported = re.findall(r'schedule "cron" run (\d+): GET', log)
    assert posted and sorted(posted) == sorted(reported), (posted, reported)

    inserted = re.findall(r'schedules joint ([0-9A-F]+) inserted', log)
    released = re.findall(r'schedules joint ([0-9A-F]+) released', log)
    assert len(inserted) == 4 and sorted(inserted) == sorted(released), (
        inserted,
        released,
    )

    # Each configuration that had schedules reached its final release.
    live = set()

    for line in log.splitlines():
        m = re.search(r'router conf ([0-9A-F]+): \d+ schedules', line)
        if m:
            assert m.group(1) not in live
            live.add(m.group(1))
            continue

        m = re.search(r'conf skcf [0-9A-F]+: 1, rtcf ([0-9A-F]+): 1$', line)
        if m:
            live.discard(m.group(1))

    assert not live, f'configurations never destroyed: {live}'


# /status "schedules" (docs/observability/status-extensions.md).


def status_schedules():
    return client.conf_get('/status/schedules')


def wait_for_runs(name, n, timeout):
    end = time.monotonic() + timeout

    while time.monotonic() < end:
        scheds = status_schedules()

        if name in scheds and scheds[name]['runs'] >= n:
            return scheds[name]

        time.sleep(0.1)

    pytest.fail(
        f'schedule "{name}" did not reach {n} runs in status: '
        f'{status_schedules()}'
    )


def test_schedules_status_absent():
    # No "schedules" configured: the key is left out entirely, not emptied.
    assert 'success' in client.conf(run_conf({}))
    assert 'schedules' not in client.conf_get('/status')


def test_schedules_status_counters():
    put_run(run_schedule(uri="/status-counters"))

    wait_for_starts(2, 10)

    begin = time.time()
    sched = wait_for_runs('cron', 2, 5)

    assert sched['skipped'] == 0
    assert sched['failed'] == 0
    assert sched['timed_out'] == 0
    assert sched['running'] in (0, 1)
    assert sched['last_status'] == 200
    assert sched['last_duration_ms'] >= 0
    # last_start is wall-clock seconds since the Epoch, close to "now".
    assert abs(sched['last_start'] - begin) < 10, sched


def test_schedules_status_skipped():
    put_run(run_schedule(uri="/?sleep=2.5", overlap="skip", timeout=10))

    wait_for_starts(2, 10)
    time.sleep(1)

    sched = status_schedules()['cron']
    assert sched['skipped'] >= 2, sched
    assert sched['runs'] >= 2, sched


def test_schedules_status_timeout():
    put_run(run_schedule(uri="/?sleep=3", interval=2, timeout=1))

    def timed_out_reached():
        return status_schedules().get('cron', {}).get('timed_out', 0) >= 1

    end = time.monotonic() + 10

    while time.monotonic() < end and not timed_out_reached():
        time.sleep(0.1)

    sched = status_schedules()['cron']
    assert sched['timed_out'] >= 1, sched
    assert sched['failed'] == 0, sched


def test_schedules_status_several():
    put_run(
        {
            "a": {"pass": "applications/schedule", "uri": "/a", "interval": 1},
            "b": {"pass": "applications/schedule", "uri": "/b", "interval": 1},
        }
    )

    wait_for_starts(4, 10)

    scheds = status_schedules()
    assert set(scheds.keys()) >= {'a', 'b'}, scheds
    assert scheds['a']['runs'] >= 1
    assert scheds['b']['runs'] >= 1
