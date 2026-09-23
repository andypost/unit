"""Regression test for #431.

The object form of "compressors" (only meaningful for a single compressor)
used to be handed to nxt_conf_vldt_object_iterator(), which treats its
"data" argument as a single validator *function*. The descriptor table
nxt_conf_vldt_compressor_members[] it was actually given is not a
function, so the router made a wild call into whatever sat at the head of
that array and crashed (SIGSEGV) while validating an entirely plausible
configuration. The array form was unaffected, which is why this went
unnoticed.

These tests exercise the object form directly against the control socket
and confirm the router is still alive and answering afterwards.
"""

from unit.control import Control

client = Control()


def test_compressors_object_form_validates():
    r = client.conf(
        {
            "settings": {
                "http": {
                    "compression": {
                        "types": ["text/css"],
                        "compressors": {
                            "encoding": "identity",
                            "min_length": 10,
                        },
                    }
                }
            },
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
        }
    )

    assert 'success' in r, 'compressors object form accepted'

    # The router must still be alive and answering; before the fix, the
    # PUT above crashed it and this second request would time out or fail.
    r = client.conf_get('/status')
    assert isinstance(r, dict), 'router still responds after object form'


def test_compressors_object_form_rejects_bad_encoding():
    r = client.conf(
        {
            "settings": {
                "http": {
                    "compression": {
                        "types": ["text/css"],
                        "compressors": {
                            "encoding": "not-a-real-encoding",
                        },
                    }
                }
            },
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
        }
    )

    assert 'error' in r, 'invalid encoding in object form rejected'

    # Still no crash: the router must keep validating normally afterwards.
    r = client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"return": 200}}],
        }
    )
    assert 'success' in r, 'router still validates after rejected config'
