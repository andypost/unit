#!/usr/bin/env python3
"""FreeUnit vs nginx + php-fpm on one Drupal site, one variable at a time.

Run from the Drupal project root by .github/workflows/drupal-freeunit-module.yml
(step "Measure").  Environment: SITE, RUN, PORT, PHP_INI, AUTH_COOKIE, HEY,
GITHUB_SHA, GITHUB_STEP_SUMMARY; BENCH_VARIANTS picks the variants (comma
separated names, or "all").

Each variant is a pair of stack configurations.  nginx stays up; php-fpm is
restarted and FreeUnit reconfigured through its control socket between
variants.  Per scenario: one warm-up run, then 3 x `hey -z 5s -c 10`, median.
Server CPU per request is read from /proc for the processes of each stack.
"""

import copy, json, os, re, resource, signal, statistics, subprocess, sys
import threading, time, urllib.request

env = os.environ
SITE, RUN, PORT = env['SITE'], env['RUN'], env['PORT']
PHP_INI = env['PHP_INI']
UNIT = f"http://127.0.0.1:{PORT}"
NGINX = 'http://127.0.0.1:8081'
FPM_VER = subprocess.run(['php', '-r', 'echo PHP_MAJOR_VERSION.".".PHP_MINOR_VERSION;'],
                         capture_output=True, text=True).stdout
DURATION = env.get('BENCH_DURATION', '5s')
RUNS = int(env.get('BENCH_RUNS', '3'))

# The ini the module's example config sets through options.admin.
EXAMPLE_ADMIN = {
    'memory_limit': '1G',
    'opcache.enable': '1',
    'opcache.validate_timestamps': '1',
    'opcache.revalidate_freq': '0',
    'realpath_cache_size': '4096K',
    'realpath_cache_ttl': '600',
    'upload_max_filesize': '64M',
    'post_max_size': '64M',
}

# One ini, read by both stacks at startup (php-fpm -c, FreeUnit options.file),
# so PHP_INI_SYSTEM settings (opcache SHM, JIT buffer) are identical too.
BASE_INI = {
    'memory_limit': '1G',
    'upload_max_filesize': '64M',
    'post_max_size': '64M',
    'opcache.enable': '1',
    'opcache.memory_consumption': '256',
    'opcache.interned_strings_buffer': '16',
    'opcache.max_accelerated_files': '20000',
    'opcache.validate_timestamps': '1',
    'opcache.revalidate_freq': '0',
    'opcache.jit': 'disable',
    'opcache.jit_buffer_size': '0',
    'realpath_cache_size': '4096K',
    'realpath_cache_ttl': '600',
}

DYNAMIC = {'max': 8, 'spare': 2, 'idle_timeout': 60}

FAIR_INI = {**BASE_INI, 'opcache.revalidate_freq': '2'}

# setup-php's conf.d, read after the -c / options.file ini, turns the tracing
# JIT on with a 256M buffer, so both stacks run with it unless a variant turns
# it off at runtime: "admin" is FreeUnit's options.admin and php-fpm's
# php_admin_value, both applied after startup.
VARIANTS = {
    # Run 6 as it was: FreeUnit with the example's options.admin on the CLI
    # php.ini, dynamic processes; php-fpm on the CLI php.ini, defaults.
    'v0-run6': dict(ini=None, admin=None, processes=DYNAMIC, threads=None),
    # Same ini file on both, the example's values (revalidate_freq=0).
    'v1-same-ini': dict(ini=BASE_INI, admin=None, processes=DYNAMIC, threads=None),
    # ... with revalidate_freq=2 (PHP's default) on both.
    'v2-revalidate2': dict(ini=FAIR_INI, admin=None, processes=DYNAMIC, threads=None),
    # ... and FreeUnit static 8 processes, like pm=static 8.
    'v3-static8': dict(ini=FAIR_INI, admin=None, processes=8, threads=None),
    # ... and 2 router threads, like nginx's 2 workers.
    'v4-threads2': dict(ini=FAIR_INI, admin=None, processes=8, threads=2),
    # ... 1 router thread.
    'v5-threads1': dict(ini=FAIR_INI, admin=None, processes=8, threads=1),
    # v4 with the JIT off on both.
    'v6-jit-off': dict(ini=FAIR_INI, admin={'opcache.jit': 'disable'}, processes=8, threads=2),
}

# (scenario, path, header the response must carry, authenticated, stacks)
SCENARIOS = [
    ('page_cache hit', '/node/1', 'x-drupal-cache: hit', False, ('freeunit', 'nginx+fpm')),
    ('page_cache hit /', '/', 'x-drupal-cache: hit', False, ('freeunit', 'nginx+fpm')),
    # The same app on a listener that passes straight to it: no routes, no
    # "share" lookups in front of PHP.
    ('page_cache hit, no share', '/node/1', 'x-drupal-cache: hit', False, ('freeunit-direct',)),
    ('authenticated', '/node/1', '200 ok', True, ('freeunit', 'nginx+fpm')),
    ('trivial php', '/bench.php', '200 ok', False, ('freeunit', 'nginx+fpm')),
]


def sh(*a, **kw):
    return subprocess.run(a, check=True, capture_output=True, text=True, **kw).stdout


def drush(*a):
    sh('vendor/bin/drush', '-y', *a)


def curl_unit(method, path, data=None):
    a = ['curl', '-sf', '-X', method, '--unix-socket', f'{RUN}/control.sock',
         f'http://localhost{path}']
    if data is not None:
        a += ['--data-binary', '@-']
    return subprocess.run(a, input=data, check=True, capture_output=True, text=True).stdout


def head(url, cookie=None):
    h = ['-H', 'Cookie: ' + cookie] if cookie else []
    return subprocess.run(['curl', '-s', '-o', '/dev/null', '-D', '-', *h, url],
                          capture_output=True, text=True).stdout.lower()


##
## CPU accounting from /proc
##

CLK = os.sysconf('SC_CLK_TCK')


def procs():
    out = {}
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            cmd = open(f'/proc/{pid}/cmdline', 'rb').read().replace(b'\0', b' ').decode(errors='replace')
            st = open(f'/proc/{pid}/stat').read()
        except OSError:
            continue
        f = st[st.rindex(')') + 2:].split()
        out[int(pid)] = (cmd, (int(f[11]) + int(f[12])) / CLK)
    return out


def group(cmd):
    if cmd.startswith('unit: router'):
        return 'unit_router'
    if cmd.startswith('unit: "drupal" application'):
        return 'unit_php'
    if cmd.startswith('unit: '):
        return 'unit_other'
    if cmd.startswith('nginx: worker'):
        return 'nginx'
    if cmd.startswith('php-fpm: pool'):
        return 'fpm'
    return None


def cpu_snapshot():
    snap = {}
    for pid, (cmd, t) in procs().items():
        g = group(cmd)
        if g:
            snap[pid] = (g, t)
    total = [int(x) for x in open('/proc/stat').readline().split()[1:]]
    return snap, total, resource.getrusage(resource.RUSAGE_CHILDREN)


def cpu_delta(a, b):
    sa, ta, ra = a
    sb, tb, rb = b
    per = {}
    for pid, (g, t) in sb.items():
        per[g] = per.get(g, 0.0) + t - (sa[pid][1] if pid in sa else 0.0)
    busy = lambda t: sum(t) - t[3] - t[4]
    per['busy_pct'] = 100.0 * (busy(tb) - busy(ta)) / max(1, sum(tb) - sum(ta))
    per['hey'] = (rb.ru_utime + rb.ru_stime) - (ra.ru_utime + ra.ru_stime)
    per['_pids'] = {g: sorted(p for p, (gg, _) in sb.items() if gg == g) for g in ('unit_php', 'fpm')}
    return per


##
## hey
##

def hey(url, cookie=None):
    h = ['-H', 'Cookie: ' + cookie] if cookie else []
    a = cpu_snapshot()
    out = subprocess.run([env['HEY'], '-z', DURATION, '-c', '10', '-disable-compression', *h, url],
                         capture_output=True, text=True, check=True).stdout
    b = cpu_snapshot()
    lat = dict(re.findall(r'(\d+)%+ in ([\d.]+) secs', out))  # hey prints '50%% in'
    codes = dict(re.findall(r'\[(\d+)\]\s+(\d+) responses', out))
    n = sum(int(v) for v in codes.values())
    errors = sum(int(v) for c, v in codes.items() if c != '200')
    errors += sum(int(v) for v in re.findall(r'\[(\d+)\]\s+Get', out))
    cpu = cpu_delta(a, b)
    return {'rps': float(re.search(r'Requests/sec:\s+([\d.]+)', out)[1]),
            'p50_ms': float(lat['50']) * 1000, 'p99_ms': float(lat['99']) * 1000,
            'errors': errors, 'n': n, 'cpu': cpu}


def measure(scenario, server, url, expect, cookie=None):
    hey(url, cookie)  # warm up the workers and OPcache
    seen = head(url, cookie)
    assert expect in seen, (scenario, server, expect, seen)
    pids0 = cpu_snapshot()[0]
    runs = [hey(url, cookie) for _ in range(RUNS)]
    med = {k: statistics.median(r[k] for r in runs) for k in ('rps', 'p50_ms', 'p99_ms')}
    med['errors'] = sum(r['errors'] for r in runs)
    # Server CPU (ms) per request, per process group, over all measured runs.
    n = sum(r['n'] for r in runs)
    groups = {}
    for r in runs:
        for g, v in r['cpu'].items():
            if g not in ('busy_pct', '_pids'):
                groups[g] = groups.get(g, 0.0) + v
    med['cpu_ms_per_req'] = {g: round(1000 * v / max(n, 1), 3) for g, v in groups.items()}
    med['busy_pct'] = round(statistics.median(r['cpu']['busy_pct'] for r in runs), 1)
    # Worker churn: PHP worker pids present at the end that were not there
    # before the measured runs.
    grp = 'unit_php' if server.startswith('freeunit') else 'fpm'
    before = {p for p, (g, _) in pids0.items() if g == grp}
    after = set(runs[-1]['cpu']['_pids'][grp])
    med['workers'] = len(after)
    med['respawned'] = len(after - before)
    return {'scenario': scenario, 'server': server, 'url': url, **med}


##
## Stacks
##

def write_ini(name, ini):
    path = f'{RUN}/{name}.ini'
    with open(path, 'w') as f:
        f.write(open(PHP_INI).read())
        f.write('\n; drupal-bench overrides\n')
        for k, v in ini.items():
            f.write(f'{k} = {v}\n')
    return path


def fpm_restart(ini_path, admin):
    try:
        os.kill(int(open(f'{RUN}/fpm.pid').read()), signal.SIGQUIT)
        for _ in range(100):
            if not os.path.exists(f'{RUN}/fpm.pid'):
                break
            time.sleep(0.1)
    except (OSError, ValueError):
        pass
    if os.path.exists(f'{RUN}/fpm.sock'):
        os.unlink(f'{RUN}/fpm.sock')
    with open(f'{RUN}/fpm.conf', 'w') as f:
        f.write(f"""[global]
pid = {RUN}/fpm.pid
error_log = {RUN}/fpm.log
[www]
listen = {RUN}/fpm.sock
pm = static
pm.max_children = 8
pm.max_requests = 0
""")
        for k, v in (admin or {}).items():
            f.write(f'php_admin_value[{k}] = {v}\n')
    subprocess.run([f'php-fpm{FPM_VER}', '-c', ini_path, '-y', f'{RUN}/fpm.conf'], check=True)
    for _ in range(100):
        if os.path.exists(f'{RUN}/fpm.sock'):
            break
        time.sleep(0.1)
    time.sleep(0.5)


def unit_apply(base, ini_path, admin, processes, threads):
    conf = copy.deepcopy(base)
    conf.pop('schedules', None)  # no cron firing in the middle of a run
    extra = {'match': {'uri': ['/bench.php', '/diag.php']},
             'action': {'pass': 'applications/drupal/direct'}}
    conf['routes']['drupal'].insert(0, extra)
    conf['listeners']['127.0.0.1:8082'] = {'pass': 'applications/drupal/index'}
    app = conf['applications']['drupal']
    app['processes'] = processes
    app['options'] = {'file': ini_path}
    if admin:
        app['options']['admin'] = admin
    conf['settings'] = {'listen_threads': threads} if threads else {}
    curl_unit('PUT', '/config', json.dumps(conf))
    time.sleep(1)


def diag(base, n=40):
    """Hit diag.php concurrently; one record per distinct worker pid."""
    seen = {}
    lock = threading.Lock()

    def one():
        try:
            d = json.loads(urllib.request.urlopen(base + '/diag.php', timeout=10).read())
        except Exception as e:  # noqa: BLE001
            d = {'error': repr(e)}
        with lock:
            seen.setdefault(d.get('pid'), []).append(d)

    ts = [threading.Thread(target=one) for _ in range(n)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    recs = [v[-1] for v in seen.values()]
    first = recs[0]
    starts = sorted({r.get('opcache', {}).get('start_time') for r in recs if 'opcache' in r} - {None})
    return {'workers_seen': len(recs), 'opcache_start_times': starts,
            'shm_shared': len(starts) == 1, 'sample': first}


def summarize_diag(d):
    s = d['sample']
    o = s.get('opcache', {})
    j = o.get('jit', {})
    return (f"sapi={s.get('sapi')} ini={s.get('ini_loaded')} opcache_enabled={o.get('opcache_enabled')} "
            f"hits={o.get('hits')} misses={o.get('misses')} scripts={o.get('num_cached_scripts')} "
            f"mem_used={o.get('used_memory')} mem_free={o.get('free_memory')} "
            f"jit={s.get('ini', {}).get('opcache.jit')} jit_on={j.get('on')} jit_buf={j.get('buffer_size')} "
            f"validate_ts={s.get('ini', {}).get('opcache.validate_timestamps')} "
            f"revalidate_freq={s.get('ini', {}).get('opcache.revalidate_freq')} "
            f"realpath={s.get('ini', {}).get('realpath_cache_size')}/{s.get('ini', {}).get('realpath_cache_ttl')} "
            f"realpath_used={s.get('realpath_cache_used')} "
            f"workers_seen={d['workers_seen']} shm_shared={d['shm_shared']}")


def timed_get(url):
    t = time.perf_counter()
    try:
        urllib.request.urlopen(url, timeout=30).read()
    except Exception as e:  # noqa: BLE001
        print(f'timed_get {url}: {e!r}', flush=True)
    return round((time.perf_counter() - t) * 1000, 1)


def unit_workers():
    return sorted(p for p, (c, _) in procs().items() if group(c) == 'unit_php')


def apply_variant(base, name, v):
    if v['ini'] is None:
        # Run 6: the CLI php.ini for both, the example's admin on FreeUnit,
        # only memory_limit on php-fpm.
        fpm_restart(PHP_INI, {'memory_limit': '1G'})
        unit_apply(base, PHP_INI, EXAMPLE_ADMIN, v['processes'], v['threads'])
        return PHP_INI
    path = write_ini(name, v['ini'])
    fpm_restart(path, v['admin'])
    unit_apply(base, path, v['admin'], v['processes'], v['threads'])
    return path


def lazy_start(base, ini_path):
    """First request after a (re)start, and after idle workers were reaped."""
    out = []
    for spare in (0, 1):
        procs_conf = {'max': 8, 'spare': spare, 'idle_timeout': 1}
        unit_apply(base, ini_path, None, procs_conf, 2)
        cold = timed_get(UNIT + '/node/1')  # fresh prototype: OPcache is empty
        subprocess.run([env['HEY'], '-z', '2s', '-c', '10', UNIT + '/node/1'], capture_output=True)
        time.sleep(4)                        # idle_timeout reaps down to "spare"
        idle = len(unit_workers())
        first = timed_get(UNIT + '/node/1')  # OPcache warm, maybe no worker
        second = timed_get(UNIT + '/node/1')
        out.append({'processes': procs_conf, 'cold_after_apply_ms': cold,
                    'idle_workers': idle, 'first_ms': first, 'second_ms': second})
    fpm_restart(ini_path, None)
    cold = timed_get(NGINX + '/node/1')
    out.append({'processes': 'php-fpm static 8', 'cold_after_apply_ms': cold,
                'idle_workers': 8, 'first_ms': timed_get(NGINX + '/node/1'),
                'second_ms': timed_get(NGINX + '/node/1')})
    for r in out:
        print(f'[lazy] {r}', flush=True)
    return out


def main():
    base = json.load(open(f'{RUN}/site.json'))
    page, asset = '/node/1', '/core/misc/drupal.js'
    open(f'{SITE}/web/bench.php', 'w').write('<?php echo "x";\n')
    open(f'{SITE}/web/diag.php', 'w').write(DIAG_PHP)
    bases = {'freeunit': UNIT, 'nginx+fpm': NGINX, 'freeunit-direct': 'http://127.0.0.1:8082'}

    names = env.get('BENCH_VARIANTS', 'all')
    names = list(VARIANTS) if names == 'all' else names.split(',')

    # page_cache only: the module's router cache off and purged.
    drush('config:set', 'freeunit.settings', 'static_cache.enabled', '0')
    drush('freeunit:cache-purge')

    results, diags, ini_path = [], {}, PHP_INI
    for name in names:
        v = VARIANTS[name]
        ini_path = apply_variant(base, name, v)
        for p in ('/node/1', '/'):
            head(UNIT + p); head(NGINX + p)
        n0 = len(results)
        for sc, path, expect, auth, stacks in SCENARIOS:
            for srv in stacks:
                r = measure(sc, srv, bases[srv] + path, expect, env['AUTH_COOKIE'] if auth else None)
                r['variant'] = name
                results.append(r)
        for srv in ('freeunit', 'nginx+fpm'):
            diags[f'{name}/{srv}'] = diag(bases[srv])
            print(f'[{name}] {srv}: ' + summarize_diag(diags[f"{name}/{srv}"]), flush=True)
        a, b = diags[f'{name}/freeunit']['sample'], diags[f'{name}/nginx+fpm']['sample']
        keys = sorted(set(a.get('ini', {})) | {'extensions', 'zend_extensions'})
        va = {k: a['ini'].get(k) if k in a.get('ini', {}) else a.get(k) for k in keys}
        vb = {k: b['ini'].get(k) if k in b.get('ini', {}) else b.get(k) for k in keys}
        # The embed SAPI has no cgi-fcgi, fpm has no unit/pcntl: expected.
        diff = {k: (va[k], vb[k]) for k in keys if va[k] != vb[k] and k != 'extensions'}
        diags[f'{name}/diff'] = diff
        print(f'[{name}] ini differences freeunit vs fpm: {diff}', flush=True)
        for x in results[n0:]:
            print(f"[{name}] {x['server']:15} {x['scenario']:24} {x['rps']:7.0f} req/s "
                  f"p50 {x['p50_ms']:.2f} p99 {x['p99_ms']:.2f} "
                  f"cpu/req {x['cpu_ms_per_req']} busy {x['busy_pct']}% "
                  f"workers {x['workers']} respawned {x['respawned']} errors {x['errors']}", flush=True)

    lazy = lazy_start(base, ini_path) if env.get('BENCH_LAZY', '1') == '1' else []
    # Back to the last variant for the router cache and the asset.
    apply_variant(base, names[-1], VARIANTS[names[-1]])

    # The router-served static cache and a static asset, on the last variant.
    # page_cache still holds the page, so empty it: the next MISS writes the file.
    drush('config:set', 'freeunit.settings', 'static_cache.enabled', '1')
    drush('php:eval', "\\Drupal::cache('page')->deleteAll();")
    head(UNIT + page)
    for _ in range(50):
        if os.path.exists(f'{SITE}/page-cache/http/127.0.0.1/node/1_.html'):
            break
        time.sleep(0.1)
    extra = [measure('static cache (router)', 'freeunit', UNIT + page, 'x-drupal-cache: hit-freeunit')]
    for srv in ('freeunit', 'nginx+fpm'):
        extra.append(measure('static asset', srv, bases[srv] + asset, '200 ok'))
    for x in extra:
        x['variant'] = names[-1]
    results += extra

    meta = {'php': sh('php', '-r', 'echo PHP_VERSION;'),
            'drupal': sh('vendor/bin/drush', 'status', '--field=drupal-version').strip(),
            'tool': f'hey -z {DURATION} -c 10 -disable-compression, warm-up + {RUNS} runs, median; errors summed',
            'cpus': os.cpu_count(), 'commit': env.get('GITHUB_SHA'),
            'variants': {n: VARIANTS[n] for n in names}}
    json.dump({'meta': meta, 'results': results, 'diag': diags, 'lazy_start': lazy},
              open('drupal-freeunit-metrics.json', 'w'), indent=2, default=str)

    with open(env.get('GITHUB_STEP_SUMMARY', '/dev/stdout'), 'a') as f:
        f.write(f"### Drupal {meta['drupal']}, PHP {meta['php']}, {meta['cpus']} vCPU\n\n")
        f.write('req/s, median. `cpu/req` is server CPU ms per request (FreeUnit: router + PHP '
                'workers; nginx+fpm: nginx workers + fpm children).\n\n')
        f.write('| variant | scenario | FreeUnit | nginx+fpm | ratio | FreeUnit cpu/req | fpm cpu/req |\n'
                '|---|---|---:|---:|---:|---|---|\n')
        by = {(r['variant'], r['scenario'], r['server']): r for r in results}
        for name in names:
            for sc, _, _, _, stacks in SCENARIOS:
                u = by.get((name, sc, 'freeunit')) or by.get((name, sc, 'freeunit-direct'))
                n = by.get((name, sc, 'nginx+fpm'))
                uc = u['cpu_ms_per_req']
                row = (f"| {name} | {sc} | {u['rps']:.0f} | "
                       f"{n['rps']:.0f} | {u['rps'] / n['rps']:.2f} | " if n else
                       f"| {name} | {sc} | {u['rps']:.0f} | - | - | ")
                row += f"router {uc.get('unit_router', 0):.2f} + php {uc.get('unit_php', 0):.2f} | "
                if n:
                    nc = n['cpu_ms_per_req']
                    row += f"nginx {nc.get('nginx', 0):.2f} + fpm {nc.get('fpm', 0):.2f} |\n"
                else:
                    row += '- |\n'
                f.write(row)
        f.write('\n| scenario | server | req/s | p50 ms | p99 ms | errors |\n|---|---|---:|---:|---:|---:|\n')
        for r in extra:
            f.write(f"| {r['scenario']} | {r['server']} | {r['rps']:.0f} | {r['p50_ms']:.2f} | "
                    f"{r['p99_ms']:.2f} | {r['errors']} |\n")
        if lazy:
            f.write('\nFirst request (/node/1, page_cache HIT), ms:\n\n'
                    '| processes | after (re)start, cold OPcache | idle workers | first, warm OPcache | second |\n'
                    '|---|---:|---:|---:|---:|\n')
            for r in lazy:
                f.write(f"| `{json.dumps(r['processes'])}` | {r['cold_after_apply_ms']} | "
                        f"{r['idle_workers']} | {r['first_ms']} | {r['second_ms']} |\n")
        f.write('\nOPcache/ini as seen by each stack (diag.php):\n\n')
        for k, d in diags.items():
            if k.endswith('/diff'):
                f.write(f'- `{k}` (freeunit, fpm): `{d}`\n')
            else:
                f.write(f'- `{k}`: {summarize_diag(d)}\n')
        f.write(f"\n{meta['tool']}. Loopback, client and server share the runner.\n")


DIAG_PHP = r'''<?php
$s = function_exists('opcache_get_status') ? opcache_get_status(false) : false;
$ini = [];
foreach (['opcache.enable', 'opcache.enable_cli', 'opcache.validate_timestamps',
          'opcache.revalidate_freq', 'opcache.memory_consumption',
          'opcache.interned_strings_buffer', 'opcache.max_accelerated_files',
          'opcache.jit', 'opcache.jit_buffer_size', 'opcache.file_cache',
          'opcache.preload', 'realpath_cache_size', 'realpath_cache_ttl',
          'memory_limit', 'zend.assertions', 'open_basedir'] as $k) {
  $ini[$k] = ini_get($k);
}
$o = [];
if ($s) {
  $o = [
    'opcache_enabled' => $s['opcache_enabled'],
    'cache_full' => $s['cache_full'],
    'used_memory' => $s['memory_usage']['used_memory'],
    'free_memory' => $s['memory_usage']['free_memory'],
    'num_cached_scripts' => $s['opcache_statistics']['num_cached_scripts'],
    'hits' => $s['opcache_statistics']['hits'],
    'misses' => $s['opcache_statistics']['misses'],
    'start_time' => $s['opcache_statistics']['start_time'],
    'jit' => $s['jit'] ?? null,
  ];
}
$ext = get_loaded_extensions();
sort($ext);
header('Content-Type: application/json');
echo json_encode([
  'pid' => getmypid(),
  'sapi' => PHP_SAPI,
  'ini_loaded' => php_ini_loaded_file(),
  'ini_scanned' => php_ini_scanned_files(),
  'realpath_cache_used' => realpath_cache_size(),
  'ini' => $ini,
  'opcache' => $o,
  'extensions' => implode(',', $ext),
  'zend_extensions' => implode(',', get_loaded_extensions(true)),
]);
'''

if __name__ == '__main__':
    main()
