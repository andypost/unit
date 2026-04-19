"""Tests for debuggable config validation errors.

Covers the additive response fields introduced by roadmap item D5:
 - ``location.path`` (RFC 6901 JSON Pointer to the offending member).
 - ``suggestion`` (best-match member name when the input was a close typo).

Legacy response fields (``error``, ``detail``, ``location.offset/line/column``)
must remain unchanged; several tests guard that.

All cases are expressed using listeners + routes so they run without any
language SAPI module configured.
"""

from unit.control import Control

client = Control()


def _put(conf, url='/config'):
    return client.conf(conf, url)


def test_unknown_top_level_key_has_root_path():
    r = _put({"foo": 1})
    assert 'error' in r
    assert 'foo' in r['detail']
    assert r['location']['path'] == ''


def test_misspelled_listeners_suggests_listeners():
    r = _put({"listners": {}})
    assert 'error' in r
    assert r.get('suggestion') == 'listeners'


def test_misspelled_applications_suggests_applications():
    r = _put({"aplications": {}})
    assert 'error' in r
    assert r.get('suggestion') == 'applications'


def test_nested_unknown_key_has_container_path():
    r = _put(
        {
            "listeners": {
                "*:8080": {"pass": "routes", "unknownkey": 1}
            },
            "routes": [{"action": {"return": 200}}],
        }
    )
    assert 'error' in r
    assert r['location']['path'] == '/listeners/*:8080'


def test_array_element_path_points_into_routes():
    r = _put(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"return": 200}},
                {"action": {"return": 200}},
                {"action": {"return": 200}, "unknownkey": 1},
            ],
        }
    )
    assert 'error' in r
    assert r['location']['path'].startswith('/routes/2')


def test_type_error_carries_path():
    r = _put(
        {
            "listeners": {"*:8080": {"pass": 123}},
            "routes": [{"action": {"return": 200}}],
        }
    )
    assert 'error' in r
    assert '/listeners' in r['location']['path']


def test_rfc6901_escapes_tilde():
    """Route name ``a~b`` must encode as ``a~0b`` in the JSON Pointer."""
    r = _put(
        {
            "listeners": {"*:8080": {"pass": "routes/a~b"}},
            "routes": {
                "a~b": [{"action": {"return": 200}, "unknownkey": 1}]
            },
        }
    )
    assert 'error' in r
    assert r['location']['path'] == '/routes/a~0b/0'


def test_no_suggestion_when_distance_exceeds_threshold():
    r = _put({"zzzzzzz": 1})
    assert 'error' in r
    assert 'suggestion' not in r


def test_backward_compat_success_has_no_new_fields():
    r = _put(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
            "applications": {},
        }
    )
    assert 'success' in r
    assert 'error' not in r
    assert 'suggestion' not in r


def test_backward_compat_error_shape_preserved():
    r = _put({"foo": 1})
    assert 'error' in r
    assert 'detail' in r
    # 'success' key must never coexist with an error.
    assert 'success' not in r
    # Legacy message wording preserved verbatim.
    assert r['detail'] == 'Unknown parameter "foo".'
