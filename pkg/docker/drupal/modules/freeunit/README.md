# freeunit: a Drupal module for FreeUnit (prototype)

> **Unverified prototype.** This module has never been installed on a Drupal
> site. Drupal core could not be installed where it was written (packagist
> and drupal.org were unreachable). Every PHP file passes `php -l` (PHP 8.4).
> The classes that do not need a Drupal runtime (`ControlApiClient`,
> `ScheduleConfigGenerator`, `StaticCachePathMapper`, `StaticCacheFiles`)
> were exercised against a locally built `unitd` with stub interfaces. The
> FreeUnit side, `examples/unit-drupal-freeunit.json`, was PUT to a local
> `unitd` and exercised with a stand-in PHP script
> (`examples/check-static-cache.py`). The event subscriber, the invalidator,
> the hooks, the Drush commands and the install file are untested.
> Review findings and fixes are in `REVIEW.md`.

The design, the measurements to make and the risks are in
[`docs/drupal/freeunit-module.md`](../../../../../docs/drupal/freeunit-module.md).

## What it does

1. **Router-served static page cache.** After page_cache stores an
   anonymous HTML page (`X-Drupal-Cache: MISS`, or `HIT` when the file is
   missing), the module writes it, and a gzip copy, to
   `<directory>/<scheme>/<host><path>_.html`. It does this on
   `kernel.terminate`, after the response has gone out. A FreeUnit route
   serves those files directly with one fixed header set
   (`static_cache.headers`); a page whose response carries any other value
   for those headers, or any header the route neither reproduces nor may
   drop, is not written. No PHP runs for a hit. When cache tags are
   invalidated, the module deletes the files that carry those tags
   (`cache_tags_invalidator` service). A full cache flush, and uninstalling
   the module, delete all of them.
2. **Cron through a FreeUnit schedule.** `drush freeunit:schedule --apply`
   PUTs a `schedules` entry that calls `GET /freeunit/cron` every 300 s with
   the cron key in a header. The listener routes return 404 for that path.
   Schedules pass straight to the application, so the path can only be
   reached by the schedule.
3. **Deployment.** `drush freeunit:restart`, and automatically after
   `drush deploy`: a graceful application restart
   (`GET /control/applications/drupal/restart`) gives fresh workers, an
   empty OPcache and a re-run of `opcache.preload`.
4. **Status.** `drush freeunit:status` shows FreeUnit's `/status` for the
   application. The status report shows whether the site runs on FreeUnit
   and whether `fastcgi_finish_request()` is available.

## Layout

| path | role |
|---|---|
| `freeunit.info.yml` | `core_version_requirement: ^11.2 \|\| ^12`: OOP hooks (11.1) and `hook_runtime_requirements` (11.2) |
| `freeunit.services.yml` | services, the subscriber and the invalidator tags |
| `freeunit.routing.yml` | `/freeunit/cron` |
| `freeunit.install` | table `freeunit_static_cache` (file, tag, expire); `hook_uninstall` purges the files |
| `config/` | `freeunit.settings` and its schema |
| `src/StaticCache/StaticCachePathMapper.php` | request to file mapping; refuses paths whose FreeUnit `$uri` would differ |
| `src/StaticCache/StaticCacheFiles.php` | atomic writes (temporary file + `rename`), deletes, purge (deletes the tree's contents, keeps the root) |
| `src/StaticCache/StaticCacheIndex.php` | tag to file index |
| `src/EventSubscriber/StaticCacheWriter.php` | **feature 1**: the writer (`kernel.terminate`) |
| `src/Cache/StaticCacheTagsInvalidator.php` | **feature 1**: deletes files on tag invalidation, twice (now and after commit) |
| `src/StaticCache/RouteConfigGenerator.php` | builds the `drupal_page` route from `freeunit.settings` (hosts, scheme, headers, session cookie names) |
| `src/Control/ControlApiClient.php` | **feature 2**: control API over the Unix socket |
| `src/Schedule/ScheduleConfigGenerator.php` | **feature 2**: builds, checks and applies the cron schedule |
| `src/Controller/CronController.php` | runs cron for a keyed loopback request, 404 otherwise |
| `src/Hook/FreeUnitHooks.php` | `hook_cache_flush` (purge), `hook_cron` (expired files), `hook_runtime_requirements` (status report) |
| `src/Drush/Commands/FreeUnitCommands.php` | `freeunit:status`, `:schedule`, `:routes`, `:restart`, `:cache-purge`, post-`deploy` hook |
| `examples/unit-drupal-freeunit.json` | the dev kit configuration plus the static cache routes and the schedule |
| `examples/check-static-cache.py` | validates that configuration against a running `unitd` |

## Trying the FreeUnit side without Drupal

```sh
# from the repo root; "configure php" needs the PHP embed SAPI (libphp-embed)
./configure && ./configure php && make -j2 unitd php
mkdir -p /tmp/fu/state /tmp/fu/tmp
build/sbin/unitd --control unix:/tmp/fu/control.sock --pid /tmp/fu/unit.pid \
    --log /tmp/fu/unit.log --statedir /tmp/fu/state --tmpdir /tmp/fu/tmp \
    --modulesdir build/lib/unit/modules
python3 pkg/docker/drupal/modules/freeunit/examples/check-static-cache.py \
    /tmp/fu/control.sock /tmp/fu/work 18080
```

The session cookie names in the example (`SESSbfabc374…`) are what Drupal
derives for host `example.org` with default session settings:
`SESS` + the first 32 hex digits of `sha256("example.org")`.
`drush freeunit:routes` computes them from the site's own session
configuration for every host in `static_cache.hosts`.

## Operational notes

- The cache directory must be writable by the PHP workers' user and
  readable by the FreeUnit router's user. Files are written 0644 and
  directories 0755. Keep it outside the docroot.
- `static_cache.headers` must be exactly what the site sends for an
  anonymous page: `cache-control` follows `cache.page.max_age`
  (`max-age=<n>, public`), `content-language` the default language. On a
  multilingual site only pages in that language are written; the others
  keep going through PHP. The route adds `Accept-Encoding` to `Vary` when
  `gzip` is on.
- `static_cache.scheme` is the scheme the *router* sees (`http` behind a
  TLS-terminating proxy). Both the writer and the route key the file tree
  on it, not on the scheme Drupal derives from `X-Forwarded-Proto`.
- The control socket stays root-only. Only Drush uses it, run as the
  socket's owner. Do not make it group-writable for `www-data`: that would
  let any PHP code rewrite the server configuration.
- `options.file` (a php.ini) is where `opcache.preload` and the OPcache
  memory sizes go. `options.admin` values are applied after PHP module
  startup (`src/nxt_php_sapi.c:442-453`), which is too late for them.
