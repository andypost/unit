#!/usr/bin/env python3
"""Validate unit-drupal-freeunit.json against a running unitd, and exercise it.

UNVERIFIED-PROTOTYPE companion: this does not need Drupal.  It rewrites the
paths in the example configuration to a scratch directory, PUTs it to the
control socket, and then checks, with a stand-in PHP script, that

  - an anonymous GET is rendered by PHP once and then served by the router
    from the static cache (identity and precompressed gzip variants);
  - a session cookie (either the exact SESS/SSESS name or any *SESS* cookie
    on a single Cookie line), an Authorization header, a query string, a
    POST and /index.php always reach PHP;
  - a dot-segment traversal is refused by the router, never served;
  - /freeunit/cron is 404 for clients but is reached by the schedule.

Usage:
    unitd --control unix:$DIR/control.sock ...   # a scratch unitd
    check-static-cache.py $DIR/control.sock $DIR/work 18080

The scratch unitd needs the PHP module; the example's "user"/"group" are
replaced with the current user.
"""

import grp
import gzip
import http.client
import json
import os
import pwd
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = 'example.org'
SESS = 'SESSbfabc37432958b063360d3ad6461c9c4'

STUB = r'''<?php
// Stand-in for Drupal + the freeunit module's StaticCacheWriter.
$uri = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);
if ($uri === '/freeunit/cron') {
    $ok = ($_SERVER['HTTP_X_FREEUNIT_CRON_KEY'] ?? '') === 'k3y';
    file_put_contents(__DIR__ . '/../cron.log', sprintf("%s %s %s\n",
        $ok ? 'ok' : 'denied', $_SERVER['REMOTE_ADDR'] ?? '-',
        $_SERVER['HTTP_USER_AGENT'] ?? '-'), FILE_APPEND);
    http_response_code($ok ? 204 : 403);
    return;
}
header('Content-Type: text/html; charset=UTF-8');
header('X-Drupal-Cache: MISS');
echo "<html><body>php $uri</body></html>\n";
fastcgi_finish_request();
$anon = !preg_match('/S?SESS[0-9a-f]+=/', $_SERVER['HTTP_COOKIE'] ?? '');
if ($_SERVER['REQUEST_METHOD'] === 'GET' && ($_SERVER['QUERY_STRING'] ?? '') === '' && $anon) {
    $file = getenv('CACHE_DIR') . '/http/' . $_SERVER['SERVER_NAME'] . $uri . '_.html';
    @mkdir(dirname($file), 0755, true);
    $html = "<html><body>static $uri</body></html>\n";
    foreach ([$file => $html, "$file.gz" => gzencode($html, 9)] as $f => $data) {
        $tmp = tempnam(dirname($f), '.fu');
        file_put_contents($tmp, $data);
        chmod($tmp, 0644);
        rename($tmp, $f);
    }
}
'''


class UnixConn(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost')
        self.sock_path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.sock_path)


def control(sock, method, path, body=None):
    c = UnixConn(sock)
    c.request(method, path, body=body)
    r = c.getresponse()
    return r.status, r.read().decode()


def raw_get(port, lines, method='GET', path='/node/1'):
    s = socket.create_connection(('127.0.0.1', port))
    req = [f'{method} {path} HTTP/1.1', f'Host: {HOST}'] + lines
    s.sendall(('\r\n'.join(req + ['Connection: close', '', ''])).encode())
    data = b''
    while True:
        d = s.recv(65536)
        if not d:
            break
        data += d
    head, _, body = data.partition(b'\r\n\r\n')
    status = int(head.split(b' ', 2)[1])
    headers = {}
    for line in head.split(b'\r\n')[1:]:
        k, _, v = line.decode().partition(':')
        headers[k.strip().lower()] = v.strip()
    return status, headers, body


def main():
    sock, work, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    web = os.path.join(work, 'web')
    cache = os.path.join(work, 'page-cache')
    os.makedirs(web, exist_ok=True)
    os.makedirs(os.path.join(cache, 'http', HOST), exist_ok=True)
    with open(os.path.join(web, 'index.php'), 'w') as f:
        f.write(STUB)
    if os.path.exists(os.path.join(work, 'cron.log')):
        os.unlink(os.path.join(work, 'cron.log'))

    raw = open(os.path.join(HERE, 'unit-drupal-freeunit.json')).read()
    raw = raw.replace('/var/www/drupal/web', web)
    raw = raw.replace('/var/lib/drupal-freeunit/page-cache', cache)
    conf = json.loads(raw)
    conf['listeners'] = {f'127.0.0.1:{port}': {'pass': 'routes/drupal'}}
    app = conf['applications']['drupal']
    app['user'] = pwd.getpwuid(os.getuid()).pw_name
    app['group'] = grp.getgrgid(os.getgid()).gr_name
    app['environment'] = {'CACHE_DIR': cache}
    del app['options']['file']
    sched = conf['schedules']['drupal-cron']
    sched['interval'] = 1
    sched['jitter'] = 0
    sched['timeout'] = 1
    sched['run_on_start'] = True
    sched['headers']['X-FreeUnit-Cron-Key'] = 'k3y'

    st, body = control(sock, 'PUT', '/config', json.dumps(conf))
    print('PUT /config:', st, body.strip().replace('\r\n', ' '))
    if st != 200:
        return 1

    failures = 0

    def check(label, cond):
        nonlocal failures
        print(('ok   ' if cond else 'FAIL ') + label)
        failures += not cond

    s, h, b = raw_get(port, [])
    check('first anonymous GET rendered by PHP', h.get('x-drupal-cache') == 'MISS')
    time.sleep(0.3)
    s, h, b = raw_get(port, [])
    check('second anonymous GET served by the router',
          h.get('x-drupal-cache') == 'HIT-FREEUNIT' and b.startswith(b'<html><body>static'))
    s, h, b = raw_get(port, ['Accept-Encoding: gzip'])
    check('gzip variant served precompressed',
          h.get('content-encoding') == 'gzip'
          and gzip.decompress(b).startswith(b'<html><body>static'))
    check('gzip variant has the HTML content type and Vary',
          h.get('content-type') == 'text/html; charset=UTF-8'
          and h.get('vary') == 'Cookie, Accept-Encoding')
    s, h, b = raw_get(port, [], method='HEAD')
    check('HEAD served by the router', h.get('x-drupal-cache') == 'HIT-FREEUNIT')
    for label, lines, method, path in [
        ('exact session cookie', [f'Cookie: a=1; {SESS}=x'], 'GET', '/node/1'),
        ('secure session cookie', [f'Cookie: S{SESS}=x'], 'GET', '/node/1'),
        ('exact session cookie on a 2nd Cookie line', ['Cookie: a=1', f'Cookie: {SESS}=x'], 'GET', '/node/1'),
        ('other host session (*SESS*)', ['Cookie: SESS0123=x'], 'GET', '/node/1'),
        ('Authorization header', ['Authorization: Basic Zm9vOmJhcg=='], 'GET', '/node/1'),
        ('query string', [], 'GET', '/node/1?page=1'),
        ('POST', ['Content-Length: 0'], 'POST', '/node/1'),
        ('/index.php', [], 'GET', '/index.php'),
        ('/index.php/node/1', [], 'GET', '/index.php/node/1'),
    ]:
        s, h, b = raw_get(port, lines, method, path)
        check(f'{label} reaches PHP', h.get('x-drupal-cache') == 'MISS')
    s, h, b = raw_get(port, [], path='/%2e%2e/%2e%2e/etc/passwd')
    check('encoded dot-segment traversal is refused', s == 400)
    s, h, b = raw_get(port, [], path='/node/1/../../../../etc/passwd')
    check('dot-segment traversal never leaves the cache',
          h.get('x-drupal-cache') != 'HIT-FREEUNIT' and b'root:' not in b)
    s, h, b = raw_get(port, [], path='/freeunit/cron')
    check('/freeunit/cron is 404 for clients', s == 404)
    time.sleep(2.5)
    log = open(os.path.join(work, 'cron.log')).read() if os.path.exists(
        os.path.join(work, 'cron.log')) else ''
    check('schedule reached /freeunit/cron with the key', log.startswith('ok'))
    print('cron.log:', log.strip().splitlines()[:1])
    # Leave a quiet configuration behind: no schedule firing every second.
    del conf['schedules']
    control(sock, 'PUT', '/config', json.dumps(conf))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
