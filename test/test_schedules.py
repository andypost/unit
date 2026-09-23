"""Tests for the top-level "schedules" object (docs/adr/0004-schedules.md).

The validation cases only PUT configurations; no schedule has to fire for
them.  An application is needed because "pass" must name one that exists.
"""

import pytest

from unit.applications.lang.python import ApplicationPython
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
