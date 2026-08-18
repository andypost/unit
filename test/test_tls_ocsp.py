import shutil
import subprocess
from pathlib import Path

import pytest

from unit.applications.tls import ApplicationTLS
from unit.option import option

prerequisites = {'modules': {'openssl': 'any'}}

client = ApplicationTLS()


def _have_openssl_ocsp():
    if shutil.which('openssl') is None:
        return False
    out = subprocess.run(
        ['openssl', 'ocsp', '-help'],
        capture_output=True,
        text=True,
        check=False,
    )
    return out.returncode in (0, 1) and 'ocsp' in (out.stderr + out.stdout)


pytestmark = pytest.mark.skipif(
    not _have_openssl_ocsp(),
    reason='openssl ocsp tooling not available',
)


def _run(args, **kwargs):
    return subprocess.check_output(
        args, stderr=subprocess.STDOUT, **kwargs
    )


def _make_ca(name='ocsp_ca'):
    """Self-signed CA usable as both issuer and OCSP signer."""
    cnf = f'''[req]
default_bits = 2048
prompt = no
encrypt_key = no
distinguished_name = dn
x509_extensions = v3_ca
[dn]
CN = {name}
[v3_ca]
basicConstraints = critical,CA:TRUE
keyUsage = keyCertSign, cRLSign, digitalSignature
extendedKeyUsage = OCSPSigning
'''
    cnf_path = Path(option.temp_dir) / f'{name}.cnf'
    cnf_path.write_text(cnf, encoding='utf-8')

    _run([
        'openssl', 'req', '-x509', '-new', '-nodes',
        '-config', str(cnf_path),
        '-keyout', f'{option.temp_dir}/{name}.key',
        '-out', f'{option.temp_dir}/{name}.crt',
        '-days', '1',
    ])


def _make_leaf(ca='ocsp_ca', name='ocsp_leaf', cn='localhost', san=None):
    san = san or cn
    leaf_cnf = f'''[req]
default_bits = 2048
prompt = no
encrypt_key = no
distinguished_name = dn
req_extensions = v3_req
[dn]
CN = {cn}
[v3_ req]
[v3_req]
subjectAltName = @alt
[alt]
DNS.1 = {san}
'''
    cnf_path = Path(option.temp_dir) / f'{name}.cnf'
    cnf_path.write_text(leaf_cnf, encoding='utf-8')

    ext = f'''subjectAltName = DNS:{san}
'''
    ext_path = Path(option.temp_dir) / f'{name}.ext'
    ext_path.write_text(ext, encoding='utf-8')

    _run([
        'openssl', 'req', '-new', '-nodes',
        '-config', str(cnf_path),
        '-keyout', f'{option.temp_dir}/{name}.key',
        '-out', f'{option.temp_dir}/{name}.csr',
    ])

    # Minimal CA database for openssl ca / openssl x509 -req signing.
    _run([
        'openssl', 'x509', '-req',
        '-in', f'{option.temp_dir}/{name}.csr',
        '-CA', f'{option.temp_dir}/{ca}.crt',
        '-CAkey', f'{option.temp_dir}/{ca}.key',
        '-CAcreateserial',
        '-out', f'{option.temp_dir}/{name}.crt',
        '-days', '1',
        '-sha256',
        '-extfile', str(ext_path),
    ])


def _make_ocsp_response(ca='ocsp_ca', leaf='ocsp_leaf', out=None):
    """Produce a DER-encoded OCSP response signed by the CA."""
    if out is None:
        out = f'{leaf}.ocsp'

    # Build an OCSP request for the leaf against the CA.
    _run([
        'openssl', 'ocsp',
        '-issuer', f'{option.temp_dir}/{ca}.crt',
        '-cert', f'{option.temp_dir}/{leaf}.crt',
        '-reqout', f'{option.temp_dir}/{leaf}.req',
    ])

    # Use -respout to sign a "good" response with the CA key.
    # openssl ocsp -reqin <req> -index <db> -CA <ca> -rsigner <sig> -rkey <key>
    index = Path(option.temp_dir) / f'{leaf}_index.txt'
    index.write_text('', encoding='utf-8')

    # openssl needs a serial number; add a "valid" entry for our leaf.
    serial_hex = subprocess.check_output([
        'openssl', 'x509', '-in', f'{option.temp_dir}/{leaf}.crt',
        '-noout', '-serial',
    ], text=True).strip().split('=')[1]

    subj_line = subprocess.check_output([
        'openssl', 'x509', '-in', f'{option.temp_dir}/{leaf}.crt',
        '-noout', '-subject', '-nameopt', 'compat',
    ], text=True).strip().split('=', 1)[1].strip()

    not_after = subprocess.check_output([
        'openssl', 'x509', '-in', f'{option.temp_dir}/{leaf}.crt',
        '-noout', '-enddate',
    ], text=True).strip().split('=')[1]

    # openssl wants YYMMDDHHMMSSZ; convert from "MMM DD HH:MM:SS YYYY GMT".
    import datetime as _dt
    end = _dt.datetime.strptime(not_after, '%b %d %H:%M:%S %Y %Z')
    expiry = end.strftime('%y%m%d%H%M%SZ')

    index.write_text(
        f'V\t{expiry}\t\t{serial_hex}\tunknown\t{subj_line}\n',
        encoding='utf-8',
    )

    out_path = f'{option.temp_dir}/{out}'

    _run([
        'openssl', 'ocsp',
        '-index', str(index),
        '-CA', f'{option.temp_dir}/{ca}.crt',
        '-rsigner', f'{option.temp_dir}/{ca}.crt',
        '-rkey', f'{option.temp_dir}/{ca}.key',
        '-reqin', f'{option.temp_dir}/{leaf}.req',
        '-respout', out_path,
    ])

    return out_path


def _upload_bundle(name='ocsp_bundle', leaf='ocsp_leaf', ca='ocsp_ca'):
    """Upload leaf+CA chain as a single PEM bundle named ``name``."""
    leaf_pem = Path(f'{option.temp_dir}/{leaf}.crt').read_bytes()
    key_pem = Path(f'{option.temp_dir}/{leaf}.key').read_bytes()
    ca_pem = Path(f'{option.temp_dir}/{ca}.crt').read_bytes()
    return client.conf(key_pem + leaf_pem + ca_pem, f'/certificates/{name}')


def _drop_ocsp_into_store(der_path, name='ocsp_bundle'):
    """Place .ocsp DER alongside the cert in unit's certificate store."""
    store = Path(option.temp_dir) / 'state' / 'certs'
    target = store / f'{name}.ocsp'
    target.write_bytes(Path(der_path).read_bytes())


def _s_client_status(port=8080, servername='localhost'):
    return subprocess.run(
        [
            'openssl', 's_client',
            '-connect', f'127.0.0.1:{port}',
            '-servername', servername,
            '-status',
            '-CAfile', f'{option.temp_dir}/ocsp_ca.crt',
        ],
        input=b'',
        capture_output=True,
        timeout=10,
        check=False,
    )


def _ocsp_serial_from_status(text):
    """Extract the SerialNumber from openssl s_client -status output."""
    import re
    m = re.search(r'Serial Number:\s*([0-9A-Fa-f]+)', text)
    return m.group(1).upper() if m else None


def _cert_serial(leaf):
    serial = subprocess.check_output([
        'openssl', 'x509', '-in', f'{option.temp_dir}/{leaf}.crt',
        '-noout', '-serial',
    ], text=True).strip().split('=')[1]
    return serial.upper()


@pytest.fixture(autouse=True)
def setup_pki():
    # State (incl. cert store) is preserved across tests in the same module;
    # remove any prior .ocsp from the cert store so missing-file tests are
    # actually missing.
    store = Path(option.temp_dir) / 'state' / 'certs'
    for stale in store.glob('*.ocsp'):
        stale.unlink()

    _make_ca()
    _make_leaf()
    der = _make_ocsp_response()
    yield der


def _apply_listener(tls_obj):
    full = {
        "listeners": {
            "*:8080": {"pass": "routes", "tls": tls_obj},
        },
        "routes": [{"action": {"return": 200}}],
    }
    return client.conf(full)


def test_ocsp_staple_served():
    assert 'success' in _upload_bundle()
    _drop_ocsp_into_store(f'{option.temp_dir}/ocsp_leaf.ocsp')

    assert 'success' in _apply_listener({
        "certificate": "ocsp_bundle",
        "ocsp_staple": True,
    })

    out = _s_client_status()
    text = out.stdout.decode(errors='replace')
    assert 'OCSP Response Status: successful' in text


def test_ocsp_staple_disabled_omits_extension():
    assert 'success' in _upload_bundle()
    _drop_ocsp_into_store(f'{option.temp_dir}/ocsp_leaf.ocsp')

    assert 'success' in _apply_listener({"certificate": "ocsp_bundle"})

    out = _s_client_status()
    text = out.stdout.decode(errors='replace')
    assert 'OCSP Response Status: successful' not in text


def test_ocsp_staple_invalid_der_rejected(skip_alert):
    """Garbage in <name>.ocsp must fail config apply; a misconfigured
    listener should never go live with a bogus stapling source."""
    skip_alert(r'ocsp_staple:', r'failed to apply new conf')

    assert 'success' in _upload_bundle()

    store = Path(option.temp_dir) / 'state' / 'certs'
    (store / 'ocsp_bundle.ocsp').write_bytes(b'not a valid OCSP response')

    result = _apply_listener({
        "certificate": "ocsp_bundle",
        "ocsp_staple": True,
    })
    assert 'error' in result or 'success' not in result


def test_ocsp_staple_missing_file_no_staple():
    """ocsp_staple=true but no .ocsp sibling: handshake still succeeds,
    no status extension is sent."""
    assert 'success' in _upload_bundle()

    assert 'success' in _apply_listener({
        "certificate": "ocsp_bundle",
        "ocsp_staple": True,
    })

    out = _s_client_status()
    text = out.stdout.decode(errors='replace')
    assert 'OCSP Response Status: successful' not in text


def test_ocsp_staple_multi_cert_sni_mapping():
    """Two certs on one listener; each .ocsp must map to its own bundle.

    Regression test for the async-OCSP bundle context bug: with
    multiple certificates on a single listener, the OCSP response that
    returns first must end up attached to the bundle that initiated the
    request, not to whichever bundle happens to be at the head of
    conf->bundle when the OCSP response arrives.
    """
    # certA covers a.example, certB covers b.example.
    _make_leaf(name='ocsp_leaf_a', cn='a.example', san='a.example')
    _make_leaf(name='ocsp_leaf_b', cn='b.example', san='b.example')

    der_a = _make_ocsp_response(leaf='ocsp_leaf_a', out='bundle_a.ocsp')
    der_b = _make_ocsp_response(leaf='ocsp_leaf_b', out='bundle_b.ocsp')

    assert 'success' in _upload_bundle(name='bundle_a', leaf='ocsp_leaf_a')
    assert 'success' in _upload_bundle(name='bundle_b', leaf='ocsp_leaf_b')

    _drop_ocsp_into_store(der_a, name='bundle_a')
    _drop_ocsp_into_store(der_b, name='bundle_b')

    assert 'success' in _apply_listener({
        "certificate": ["bundle_a", "bundle_b"],
        "ocsp_staple": True,
    })

    serial_a = _cert_serial('ocsp_leaf_a')
    serial_b = _cert_serial('ocsp_leaf_b')

    out_a = _s_client_status(servername='a.example')
    text_a = out_a.stdout.decode(errors='replace')
    assert 'OCSP Response Status: successful' in text_a, \
        f'no staple for a.example:\n{text_a[-2000:]}'
    stapled_a = _ocsp_serial_from_status(text_a)
    assert stapled_a == serial_a, \
        f'a.example stapled wrong serial: got {stapled_a}, want {serial_a}'

    out_b = _s_client_status(servername='b.example')
    text_b = out_b.stdout.decode(errors='replace')
    assert 'OCSP Response Status: successful' in text_b, \
        f'no staple for b.example:\n{text_b[-2000:]}'
    stapled_b = _ocsp_serial_from_status(text_b)
    assert stapled_b == serial_b, \
        f'b.example stapled wrong serial: got {stapled_b}, want {serial_b}'
