# ddev-freeunit

Run a [DDEV](https://ddev.com) project's `web` container on
[FreeUnit](https://freeunit.org) -- a community application server forked
from NGINX Unit, with PHP embedded directly as a SAPI -- instead of the
default `nginx-fpm` or `apache-fpm`.

This directory is laid out as a standalone add-on repository
(`install.yaml` at the top, `tests/test.bats`, its own
`.github/workflows/tests.yml`) so it can be split out to its own
`freeunitorg/ddev-freeunit` (or similar) repository with no restructuring.

## Quickstart

```bash
ddev add-on get /path/to/pkg/ddev/ddev-freeunit   # or the published repo, once split out
ddev restart
```

The first `ddev restart` builds the web image, which compiles `unitd` and
FreeUnit's PHP module from source against this project's
`php${DDEV_PHP_VERSION}-embed` package (see
[How it's built](#how-its-built)); expect that one build to take a minute
or two. After that, `ddev start`/`ddev restart` are as fast as normal.

Then:

```bash
ddev freeunit status   # what FreeUnit thinks its configuration and app are doing
ddev freeunit reload    # re-render and re-apply the configuration, restart the app
ddev freeunit log        # tail FreeUnit's log
```

To go back to the normal web server: `ddev add-on remove freeunit && ddev restart`.

## How it works

This add-on uses DDEV's [`generic` web server
type](https://docs.ddev.com/en/stable/users/configuration/config/#webserver_type):
`config.freeunit.yaml` sets `webserver_type: generic`, which tells DDEV not
to start nginx or php-fpm at all, and starts FreeUnit itself as a
[`web_extra_daemons`](https://docs.ddev.com/en/stable/users/extend/customization-extendibility/#running-extra-daemons-in-the-web-container)
entry (`freeunit/start-freeunit.sh`, supervised the same way DDEV supervises
its own daemons). `web_extra_exposed_ports` then tells `ddev-router` to
forward the project's normal HTTP(S) traffic to FreeUnit's listener on
container port 80:

```yaml
web_extra_exposed_ports:
  - name: "freeunit"
    container_port: 80
    http_port: 80
    https_port: 443
```

**`ddev-router` still terminates TLS.** FreeUnit only ever sees plain HTTP
on port 80, exactly as nginx-fpm does today; nothing about certificates or
`https://<project>.ddev.site` changes.

On every (re)start, `freeunit/start-freeunit.sh`:

1. runs `freeunit/render-config.sh`, which builds a FreeUnit JSON
   configuration from the running environment (`$DDEV_APPROOT`,
   `$DDEV_DOCROOT`, `$DDEV_PROJECT_TYPE`, and a few `FREEUNIT_*` overrides,
   see below);
2. starts `unitd-freeunit` in the foreground with its control API on a
   Unix socket (`/var/run/freeunit-control.sock`);
3. `PUT`s the rendered configuration to `/config` over that socket.

Both the render step and the `PUT` are idempotent -- re-running them (which
is exactly what happens on every container restart, and what `ddev freeunit
reload` does on demand) always converges to the same configuration, never
accumulates state.

### Routing

`render-config.sh` is adapted from this repository's own
`pkg/docker/drupal/unit-drupal.json`:

- for Drupal-family `$DDEV_PROJECT_TYPE` (`drupal`, `drupal6`..`drupal12`,
  `backdrop`), it emits the same deny rules (`.ht*`, `sites/*/files/*.php`,
  `sites/*/private/*` → 403; `vendor/`, dotfiles, `*.module`/`*.inc`/`*.yml`/
  source files → 404), the same direct-script allowances for
  `update.php`/`install.php`/`rebuild.php`, and the same
  `share` + `fallback: pass applications/app/index` for everything else;
- for anything else, it emits the plain `share` + `fallback` rule only, which
  is enough for any front-controller PHP app (`index.php` handles what
  static files don't).

### Xdebug

FreeUnit loads PHP once per worker process, as an embedded SAPI -- there is
no per-request `php-fpm` fork that would pick up an ini change automatically.
So this add-on overrides DDEV's own `ddev xdebug` (`commands/web/xdebug`) to:

1. `phpenmod -s embed xdebug` / `phpdismod -s embed xdebug` -- scoped to the
   `embed` SAPI only, so `php-cli` (used by `ddev exec phpunit`, Composer,
   etc.) is untouched;
2. re-render and re-`PUT` the configuration, then hit FreeUnit's control API
   at `GET /control/applications/app/restart`, which gracefully recycles the
   app's worker processes so the new `php.ini` takes effect immediately.

`render-config.sh` also raises `limits.timeout` from 60s to 3600s whenever
Xdebug is loaded (`FREEUNIT_APP_TIMEOUT` / `FREEUNIT_APP_TIMEOUT_XDEBUG` to
override), for the same reason `pkg/docker/drupal/drupal-dev.sh` does: to
FreeUnit's router, a worker paused on a breakpoint looks exactly like an
application that stopped answering, and the request would otherwise be
abandoned with a 503 while you are still stepping through it.

```bash
ddev xdebug on
ddev xdebug status
ddev xdebug off
```

### Cron, via FreeUnit's `schedules`

Set `FREEUNIT_CRON_KEY` in `web_environment` (`.ddev/config.yaml`) and
`render-config.sh` adds a `schedules` entry that has FreeUnit itself issue
the periodic request -- no cron daemon, no extra container, no
`automated_cron` relying on visitor traffic:

```yaml
# .ddev/config.yaml
web_environment:
  - FREEUNIT_CRON_KEY=your-drupal-cron-key
  - FREEUNIT_CRON_INTERVAL=300   # optional, seconds; default 300
```

For a Drupal project this hits `/cron/<key>` (Drupal's own cron-key URL);
for anything else, `/<key>`. This is optional and off by default.

### Configuration knobs

All read by `render-config.sh`, set via `web_environment` in
`.ddev/config.yaml`:

| variable | default | meaning |
|---|---|---|
| `FREEUNIT_APP_TIMEOUT` | `60` | `limits.timeout`, seconds, when Xdebug is off |
| `FREEUNIT_APP_TIMEOUT_XDEBUG` | `3600` | `limits.timeout`, seconds, when Xdebug is on |
| `FREEUNIT_PROCESSES_MAX` | `4` | `processes.max` |
| `FREEUNIT_PROCESSES_SPARE` | `1` | `processes.spare` |
| `FREEUNIT_MEMORY_LIMIT` | `256M` | `php.ini` `memory_limit` |
| `FREEUNIT_CRON_KEY` | unset | enables the `schedules` cron entry |
| `FREEUNIT_CRON_INTERVAL` | `300` | cron schedule interval, seconds |

## How it's built

`web-build/Dockerfile.freeunit` builds `unitd` and its PHP module from a
pinned FreeUnit git tag (`FREEUNIT_VERSION`, default matches this
repository's current release), during the web image build, the same way
`pkg/docker/Dockerfile.php-8.5` in this repository does for the published
FreeUnit Docker images -- except it links against the **existing**
`php${DDEV_PHP_VERSION}-embed`/`-dev` packages DDEV already fetches from
`deb.sury.org` (added via `webimage_extra_packages` in
`config.freeunit.yaml`) instead of building PHP itself. `./configure php
--config=php-config${DDEV_PHP_VERSION}` finds that package's embed SAPI
directly; no patches to FreeUnit's `auto/php` were needed. The compiler
toolchain and `git` are removed again at the end of the same `RUN`, so they
don't linger in the final image.

## Comparison with `nginx-fpm` / `apache-fpm`

| | `nginx-fpm` / `apache-fpm` (default) | `ddev-freeunit` (this add-on) |
|---|---|---|
| Process model | web server + separate `php-fpm` pool, over FastCGI | one process (`unitd`) with PHP embedded in each worker |
| Config reload | `nginx -s reload` / `ddev restart` | live `PUT` to FreeUnit's control API, or `ddev freeunit reload` |
| Xdebug toggle | `ddev xdebug on/off` bounces `php-fpm` | overridden `ddev xdebug on/off`, restarts the FreeUnit app over its control API |
| Cron | `automated_cron` (needs visitor traffic) or a host/sidecar cron | FreeUnit's own `schedules`, driven by `FREEUNIT_CRON_KEY` -- no extra process |
| Static files | nginx `try_files` / Apache rewrite | Unit `share` + `fallback` route, same semantics |
| TLS | terminated by `ddev-router` | unchanged: still terminated by `ddev-router` |
| Maturity in DDEV | first-party, default | this add-on; FreeUnit itself is a young NGINX Unit fork |
| Build cost | none (prebuilt image) | one `unitd` + PHP module compile per PHP-version image build |

## Limitations

- **FreeUnit is compiled from source at web image build time.** There is no
  prebuilt FreeUnit web-image layer to pull yet, so the first `ddev
  restart` (or any `ddev restart --no-cache`) pays a real compile. Pin
  `FREEUNIT_VERSION` in a project `.ddev/web-build/Dockerfile.*` if you need
  a specific release.
- **One PHP version per FreeUnit process**, fixed at image-build time by
  `$DDEV_PHP_VERSION`. Changing `ddev config --php-version` requires a
  rebuild (`ddev restart --no-cache` or a plain `ddev restart`, since
  `$DDEV_PHP_VERSION` changing already forces a web image rebuild).
- **No `nginx`/`apache` config files** (`.ddev/nginx_full`, custom vhosts)
  apply. All routing lives in `render-config.sh`'s generated FreeUnit JSON;
  project-specific routing needs edits there (or your own
  `.ddev/web-build/Dockerfile.freeunit.local`-style override plus a
  post-start hook that re-`PUT`s your own JSON).
- **Multiple docroots / vhosts in one project** are not set up by this
  add-on. FreeUnit's `routes`/`listeners` can express that; it isn't
  generated here.
- **Not tested against every Drupal version or every PHP extension.** The
  Drupal routing rules are carried over verbatim from
  `pkg/docker/drupal/unit-drupal.json`, which this repository's own
  `Dockerfile.drupal-core-dev-php8.5` dev kit image exercises; this add-on
  wraps the same rules for DDEV but has its own, narrower test suite (see
  `tests/test.bats`).
- **FreeUnit itself is new** (a fork of NGINX Unit). Treat this add-on as
  early and read `ddev freeunit log` / `docker logs ddev-<project>-web`
  first if something looks wrong.

## Alternative approach considered: a separate FreeUnit service container

Rather than running FreeUnit inside DDEV's `web` container, a second
approach would add a completely separate service, similar to
`ddev-solr` or `ddev-redis`:

```yaml
# docker-compose.freeunit.yaml (sketch, not implemented)
services:
  freeunit:
    container_name: ddev-${DDEV_SITENAME}-freeunit
    image: ghcr.io/freeunitorg/freeunit:1.36.1-php-8.3
    labels:
      com.ddev.site-name: ${DDEV_SITENAME}
      com.ddev.approot: ${DDEV_APPROOT}
    environment:
      HTTP_EXPOSE: "80:80"
      HTTPS_EXPOSE: "443:80"
      VIRTUAL_HOST: $DDEV_HOSTNAME
    volumes:
      - "../:/var/www/drupal/web:cached"
```

This has real advantages: the FreeUnit image is prebuilt (no compile at
`ddev start`), and its PHP build is guaranteed consistent with the upstream
`pkg/docker/Dockerfile.php-8.X` images rather than reassembled against
`deb.sury.org`. The trade-off is that it stops being DDEV's normal `web`
container:

- **PHP no longer comes from DDEV.** Every extension DDEV normally manages
  through `webimage_extra_packages` (and, more importantly, Composer's
  platform requirements, `ddev composer`, `ddev exec`'s `php` binary) is on
  a different container's PHP, with its own package set, unless you also
  install a matching PHP in `web` just for tooling.
  `ddev xdebug`/`ddev-webserver`'s built-in tooling stops applying to the
  PHP that actually serves requests.
- **Mounts and file sync** (Mutagen, NFS, the plain bind mount) are wired up
  for the `web` container specifically; a second service needs its own
  volume wiring, kept in sync with `web`'s.
- **`VIRTUAL_HOST`/`HTTP_EXPOSE`** on a plain service container is
  `ddev-router`'s older, more manual mechanism (predating
  `web_extra_exposed_ports`); it works, but duplicates what DDEV already
  does automatically for `web`.

Approach (a) -- this add-on -- was chosen because it keeps FreeUnit as *the*
web container: `ddev composer`, `ddev exec`, Mutagen/NFS mounts,
`webimage_extra_packages`, and `ddev xdebug` (once overridden, see above)
all keep working against the one PHP that's actually serving requests. The
service-container sketch above is left here as a documented alternative,
not implemented, for a future add-on variant that instead wants to track
the prebuilt `ghcr.io/freeunitorg/freeunit` images directly.

## Files

```
install.yaml                     # ddev add-on get manifest
config.freeunit.yaml              # webserver_type: generic, daemon, ports, packages
web-build/Dockerfile.freeunit      # builds unitd + php module from source
freeunit/render-config.sh          # generates the FreeUnit JSON config
freeunit/start-freeunit.sh         # web_extra_daemons entrypoint
commands/web/freeunit              # ddev freeunit status|reload|log
commands/web/xdebug                # overrides ddev xdebug for the embedded SAPI
tests/test.bats                    # bats tests (ddev/github-action-add-on-test)
.github/workflows/tests.yml        # CI, following ddev/ddev-addon-template
```
