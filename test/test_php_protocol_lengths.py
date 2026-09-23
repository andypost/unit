"""Request lengths that do not fit the libunit protocol's uint8_t fields.

A header field name reaches a PHP application as "HTTP_" + name, and the
name_length on the wire is uint8_t.  The HTTP parser allows names of up to
255 bytes, so names of 251..255 bytes used to wrap to 0..4 and reach the
application truncated.  The request method is uint8_t as well and is not
bounded by the parser at all.  Both are now refused before the request is
sent to the application: 431 and 501.
"""

from unit.applications.lang.php import ApplicationPHP

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


def test_php_protocol_field_name_fits():
    client.load('variables')

    name = 'X' * 250

    resp = client.get(headers={'Host': 'localhost', name: 'v'})
    assert resp['status'] == 200, '250-byte name + "HTTP_" is 255'


def test_php_protocol_field_name_too_long():
    client.load('variables')

    for length in (251, 253, 255):
        resp = client.get(headers={'Host': 'localhost', 'X' * length: 'v'})
        assert resp['status'] == 431, f'{length}-byte name + "HTTP_"'

    assert client.get()['status'] == 200, 'still serving'


def test_php_protocol_method_too_long():
    client.load('variables')

    resp = client.http('A' * 255, headers={'Host': 'localhost'})
    assert resp['status'] == 200, '255-byte method'

    resp = client.http('A' * 256, headers={'Host': 'localhost'})
    assert resp['status'] == 501, '256-byte method'

    assert client.get()['status'] == 200, 'still serving'
