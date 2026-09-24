"""End-to-end "schedules" tests against a PHP application.

This is the PHP-specific companion to test_schedules.py, which already
covers configuration validation and the run mechanics against a Python
application. Here the point is PHP itself: an application that sleeps,
answers a chosen status, or calls fastcgi_finish_request() the way Drupal's
"automated_cron" does, using test/php/schedule/index.php. See
test/README (or test/setup-php-embed.sh) for how the PHP embed SAPI this
module links against is staged, and docs/schedules.md for the documented
caveats these tests exercise.
"""

import json
import time

import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.log import Log
from unit.option import option

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


def run_log():
    return f'{option.temp_dir}/schedule_php.log'


def run_conf(schedules, listeners=True, limits=None):
    path = f'{option.test_dir}/php/schedule'

    app = {
        "type": client.get_application_type(),
        # A little "spare" keeps a warm process around: PHP embed SAPI
        # process start-up cost would otherwise be visible as extra
        # "running" time by itself, obscuring the trailing-work behavior
        # these tests are about.
        "processes": {"max": 4, "spare": 1},
        "root": path,
        "working_directory": path,
        "script": "index.php",
        "environment": {"SCHEDULE_LOG": run_log()},
        "options": {"admin": {"auto_globals_jit": "1"}},
    }

    if limits is not None:
        app["limits"] = limits

    return {
        "listeners": (
            {"*:8080": {"pass": "applications/schedule"}} if listeners else {}
        ),
        "applications": {"schedule": app},
        "schedules": schedules,
    }


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


def wait_for(event, n, timeout):
    end = time.monotonic() + timeout

    while time.monotonic() < end:
        recs = records(event)

        if len(recs) >= n:
            return recs

        time.sleep(0.1)

    pytest.fail(f'{len(records(event))} "{event}" record(s), expected {n}')


def wait_for_starts(n, timeout):
    return wait_for('start', n, timeout)


def assert_no_concurrency():
    """Every "start"/"end" pair for the schedule request itself does not
    overlap another one. "detached_end" (work after
    fastcgi_finish_request()) is deliberately excluded: that is exactly the
    work overlap:skip cannot see (docs/schedules.md)."""

    running = 0

    for rec in records():
        if rec['event'] == 'start':
            running += 1
        elif rec['event'] == 'end':
            running -= 1
        else:
            continue

        assert running in (0, 1), 'two runs at once'


def put_run(schedules, **kwargs):
    assert 'success' in client.conf(run_conf(schedules, **kwargs))


def test_schedules_php_fires():
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
        assert rec['query'] == 'x=1'
        assert rec['host'] == 'example.org'
        assert rec['server_name'] == 'example.org'
        assert rec['x_cron'] == 'yes'
        assert rec['user_agent'] == 'FreeUnit-Schedule/cron'
        assert rec['server_port'] == '80'
        assert rec['remote_addr'] == '127.0.0.1'

    gaps = [b['time'] - a['time'] for a, b in zip(starts, starts[1:])]
    assert all(0.7 < gap < 2.5 for gap in gaps), gaps


def test_schedules_php_overlap_skip():
    # The script sleeps 2.5 s with a 1 s interval: several runs come due
    # while one is in flight and must be skipped, and there is never more
    # than one concurrent run.
    put_run(run_schedule(uri="/?sleep=2.5", overlap="skip", timeout=10))

    wait_for_starts(2, 10)
    time.sleep(1)

    assert_no_concurrency()

    skips = Log.findall(
        r'schedule "cron": run skipped, the previous one is still running'
    )
    assert len(skips) >= 2, skips


def test_schedules_php_timeout():
    put_run(run_schedule(uri="/?sleep=3", interval=2, timeout=1))

    assert Log.wait_for_record(
        r'schedule "cron" run 1: GET /\.\.\. timed out after \d+ ms'
    )

    # The timeout freed the schedule for its next run; the abandoned PHP
    # worker is not stopped and still finishes its own request in the
    # background (docs/schedules.md: "a timeout does not stop the
    # application").
    assert Log.wait_for_record(r'schedule "cron" run 2: GET /\.\.\. ')
    assert not Log.findall(r'run skipped')

    time.sleep(2)
    assert len(records('end')) >= 1


def test_schedules_php_fastcgi_finish_request_overlap_skip():
    """Documents ADR 0004 risk R3: fastcgi_finish_request() makes the
    devnull request "complete" as soon as the response is flushed, while
    the PHP worker keeps running afterwards. overlap:skip therefore does
    NOT protect against overlapping that trailing work -- a run that
    starts while the previous request's *worker* is still busy behind the
    scenes is not skipped, because the schedule already considers the
    previous run finished.

    "finish=1" makes the script call fastcgi_finish_request() right away
    and then sleep for "sleep" seconds before it truly exits; interval is
    shorter than that trailing sleep.
    """

    put_run(
        run_schedule(
            uri="/?finish=1&sleep=2",
            interval=1,
            overlap="skip",
            timeout=10,
        )
    )

    # Several runs get through "end" (the fast, visible completion) well
    # within the 2 s the previous worker is still busy detached.
    wait_for('end', 3, 8)

    # No "run skipped" warning is expected here: from the schedule's point
    # of view every run above found the previous one already finished.
    assert not Log.findall(r'run skipped'), (
        'overlap:skip should not see a finished run as still running, '
        'even though its worker kept executing after '
        'fastcgi_finish_request()'
    )

    # The detached tail work does overlap in real time: this is the
    # documented gap, not a bug in the test.
    ends = [r['time'] for r in records('end')]
    detached = [r['time'] for r in records('detached_end')]

    overlap_seen = any(
        d > e and (d - e) < 2.5 for e in ends for d in detached if d > e
    )
    assert overlap_seen or len(detached) < len(ends), (
        'expected to observe detached PHP work still running past a '
        'later run\'s "end", demonstrating that overlap:skip does not '
        'see it'
    )
