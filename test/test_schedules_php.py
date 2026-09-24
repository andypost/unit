"""Schedules against PHP, through test/php/schedule/index.php."""

from unit.applications.lang.php import ApplicationPHP
from unit.log import Log
from unit.option import option
from unit.utils import jsonl_records, waitforrecords

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


def run_log():
    return f'{option.temp_dir}/schedule_php.log'


def put_run(**kwargs):
    path = f'{option.test_dir}/php/schedule'
    s = {"pass": "applications/schedule", "uri": "/cron", "interval": 1}

    assert 'success' in client.conf({
        "listeners": {"*:8080": {"pass": "applications/schedule"}},
        "applications": {"schedule": {
            "type": client.get_application_type(),
            # A warm process keeps start-up out of the measured run time.
            "processes": {"max": 4, "spare": 1},
            "root": path,
            "working_directory": path,
            "script": "index.php",
            "environment": {"SCHEDULE_LOG": run_log()},
        }},
        "schedules": {"cron": {**s, **kwargs}},
    })


def test_schedules_php_fires():
    uri = '/cron/SECRET_KEY?x=1'
    put_run(uri=uri, headers={"Host": "example.org", "X-Cron": "yes"})

    starts = waitforrecords(run_log(), 3, 10)

    for rec in starts:
        assert rec['method'] == 'GET'
        assert rec['uri'] == uri
        assert rec['query'] == 'x=1'
        assert rec['host'] == rec['server_name'] == 'example.org'
        assert rec['x_cron'] == 'yes'
        assert rec['user_agent'] == 'FreeUnit-Schedule/cron'
        assert rec['server_port'] == '80'
        assert rec['remote_addr'] == '127.0.0.1'

    gaps = [b['time'] - a['time'] for a, b in zip(starts, starts[1:])]
    assert all(0.5 < gap < 2.5 for gap in gaps), gaps


def test_schedules_php_fastcgi_finish_request_overlap_skip():
    # ADR 0004: after fastcgi_finish_request() the run is complete while the
    # worker goes on, so "overlap": "skip" does not cover the trailing work.
    put_run(uri="/?finish=1&sleep=2", overlap="skip", timeout=10)

    waitforrecords(run_log(), 3, 8, 'end')

    assert not Log.findall(r'run skipped')

    ends = [r['time'] for r in jsonl_records(run_log(), 'end')]
    detached = [r['time'] for r in jsonl_records(run_log(), 'detached_end')]

    assert len(detached) < len(ends) or any(
        0 < d - e < 2.5 for e in ends for d in detached
    ), (ends, detached)
