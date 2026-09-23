# FreeUnit Drupal core dev kit

One image for Drupal core code sprints: FreeUnit and PHP 8.5, with the
extensions Drupal needs, plus Composer, drupal/coder, SQLite and Xdebug. It
serves a Drupal checkout that you mount from the host, and it runs the
installer, the tests and the linters against that checkout.

- Image: `Dockerfile.drupal-core-dev-php8.5`, layered on the FreeUnit
  `php-8.5` image.
- FreeUnit configuration: [`unit-drupal.json`](unit-drupal.json).
- Entrypoint: [`drupal-dev.sh`](drupal-dev.sh).

## Build

The image is not published yet. Build it from `pkg/docker`:

```sh
cd pkg/docker
make build-drupal-core-dev-php8.5          # tag: unit:1.36.1-drupal-core-dev-php8.5
# or, on top of a locally built base (make build-php-8.5):
make build-drupal-core-dev-php8.5 \
    DEVKIT_BASE_drupal-core-dev-php8.5=unit:1.36.1-php-8.5
```

The examples below use `IMAGE=unit:1.36.1-drupal-core-dev-php8.5`.

## Sprint quickstart

From a Drupal core clone
(`git clone https://git.drupalcode.org/project/drupal.git`), one command:

```sh
docker run -d --name drupal-dev -p 8080:80 \
    -e DRUPAL_AUTO_INSTALL=1 \
    -v "$PWD:/var/www/drupal/web" \
    "$IMAGE"
```

Then open <http://localhost:8080>. On the first start this runs
`composer install`, which needs the network once. It then installs the
`standard` profile on SQLite and starts FreeUnit. A core clone has no Drush,
so the installer is core's own `core/scripts/drupal install`, which prints
the admin password; see `docker logs drupal-dev`. With a
`drupal/recommended-project` mounted at `/var/www/drupal` (docroot in `web/`),
Drush is used instead, with `admin`/`admin`.

The container's `www-data` user takes the uid and gid of the mounted
checkout, so everything Drupal, Composer and PHPUnit write stays editable
on the host.

## Commands

Run the commands in the running container with `docker exec drupal-dev
drupal-dev.sh <command>`, or pass them as the container command, in which
case FreeUnit is started in the background first.

| command | what it does |
|---|---|
| `serve` | the default: apply the FreeUnit configuration and run `unitd` in the foreground. With `DRUPAL_AUTO_INSTALL=1`, run `si` first if the site is not installed. |
| `si [args]` | `composer install` if `vendor/` is missing, then `drush site:install` on SQLite (`sites/default/files/.ht.sqlite`), or core's installer for a core clone. Extra arguments go to the installer. |
| `test <path> [args]` | PHPUnit with `core/phpunit.xml(.dist)`, `SIMPLETEST_BASE_URL=http://localhost` and `SIMPLETEST_DB=sqlite://localhost//tmp/test.sqlite`. Functional tests talk to this FreeUnit. Paths are relative to the docroot. |
| `lint [paths]` | `phpcs` with `core/phpcs.xml.dist` (core clone) or `Drupal,DrupalPractice`, then `phpstan` if the project has it. With no paths, lints the files changed against `HEAD`. It uses the project's `vendor/bin/phpcs` if there is one, otherwise the image's own drupal/coder, which works offline. |
| `shell` | a login shell in the project, as `www-data` (`SHELL_USER=root` for root) |

Examples:

```sh
docker exec drupal-dev drupal-dev.sh test core/modules/node/tests/src/Functional/NodeCreationTest.php
docker exec drupal-dev drupal-dev.sh test --group node core/modules/node
docker exec drupal-dev drupal-dev.sh lint core/modules/node/src/NodeForm.php
docker exec -it drupal-dev drupal-dev.sh shell
```

Environment:

| variable | default | |
|---|---|---|
| `DRUPAL_AUTO_INSTALL` | `0` | `serve` installs the site first if needed |
| `DRUPAL_PROFILE` | `standard` | install profile |
| `DRUPAL_ADMIN_USER` / `DRUPAL_ADMIN_PASS` | `admin` / `admin` | Drush installs only |
| `DRUPAL_DB_URL` | `sqlite://sites/default/files/.ht.sqlite` | Drush installs only |
| `DRUPAL_BASE` / `DRUPAL_DOCROOT` | `/var/www/drupal` / `…/web` | where the checkout is mounted |
| `SIMPLETEST_BASE_URL` / `SIMPLETEST_DB` | see above | for `test` |
| `XDEBUG` | `0` | `1` loads Xdebug and raises `limits.timeout` |
| `UNIT_APP_TIMEOUT` / `UNIT_APP_TIMEOUT_XDEBUG` | `300` / `3600` | FreeUnit `limits.timeout` for the Drupal app, in seconds |
| `UNITD` | `unitd` | `unitd-debug` for the debug build (see below) |
| `UNIT_KEEP_STATE` | `0` | `1` keeps `/var/lib/unit` across restarts instead of re-rendering the configuration |

## What the FreeUnit configuration does

`unit-drupal.json` follows Drupal's `.htaccess` and the usual nginx recipe:

| request | action |
|---|---|
| `*/.ht*`, `/sites/*/files/*.php`, `/sites/*/private/*` | `403` |
| `/vendor/*`, dotfiles other than `/.well-known/*`, Drupal source (`*.module`, `*.inc`, `*.yml`, `*.twig`, …), `composer.json/lock`, backup files | `404` |
| `/core/install.php`, `/core/rebuild.php`, `/update.php`, `/core/modules/statistics/statistics.php` | PHP, the named script (`targets.direct`) |
| any other `*.php` except `/index.php` | `404` |
| everything else | the static file if there is one, else `index.php` (`share` + `fallback`) |

OPcache is on, with `validate_timestamps=1` and `revalidate_freq=0`, so
edits are picked up on the next request. The file is rendered into
`/docker-entrypoint.d/` at every start and applied by the base image's
`docker-entrypoint.sh`. To use your own, mount it over
`/usr/share/unit/drupal/unit-drupal.json`.

## Step debugging with Xdebug

Xdebug is installed but loaded only when `XDEBUG=1`:

```sh
docker run -d --name drupal-dev -p 8080:80 \
    -e XDEBUG=1 \
    --add-host=host.docker.internal:host-gateway \
    -v "$PWD:/var/www/drupal/web" \
    "$IMAGE"
```

The defaults are `xdebug.mode=debug,develop`,
`xdebug.start_with_request=trigger` and `client_host=host.docker.internal`,
port 9003. Override them with `XDEBUG_MODE`, `XDEBUG_START_WITH_REQUEST`,
`XDEBUG_CLIENT_HOST` and `XDEBUG_CLIENT_PORT`. Start a session with the
browser extension, or with `?XDEBUG_TRIGGER=1`. For PHPUnit, use
`docker exec -e XDEBUG_TRIGGER=1 drupal-dev drupal-dev.sh test …`. The
PHPUnit process and the FreeUnit workers serving its functional-test
requests both connect. On Linux, `--add-host` is what makes
`host.docker.internal` resolve.

VS Code (PHP Debug extension), `.vscode/launch.json` in the core clone:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Xdebug: FreeUnit dev kit",
            "type": "php",
            "request": "launch",
            "port": 9003,
            "pathMappings": {
                "/var/www/drupal/web": "${workspaceFolder}"
            }
        }
    ]
}
```

For a recommended-project mounted at `/var/www/drupal`, map
`"/var/www/drupal": "${workspaceFolder}"` instead. In PhpStorm, use a server
named after the `Host` you browse with, mapped the same way.

**Why `limits.timeout` goes up.** FreeUnit's router gives the application
`limits.timeout` seconds to answer (`applications.drupal.limits.timeout`).
A worker stopped on a breakpoint is, to the router, an application that does
not answer. When the time runs out, the browser gets a 503 and the router
abandons the request while you are still stepping through it. So with
`XDEBUG=1` the entrypoint sets the timeout to 3600 s instead of 300 s
(`UNIT_APP_TIMEOUT_XDEBUG`). PHP's own `max_execution_time` in this NTS
build counts CPU time, so time paused at a breakpoint does not use it up.
To switch Xdebug on or off, re-create the container: the configuration is
rendered at start.

A paused worker also holds one of the application's processes (`max: 8`).
Other requests keep being served by the remaining workers.

## Other debugging options

- **FreeUnit debug build.** The base image ships `unitd-debug` and matching
  debug modules. Run with `-e UNITD=unitd-debug` to get FreeUnit's debug log
  (every request, routing decision and port message) on `docker logs`. It
  is very verbose.
- **The FreeUnit configuration as applied:**
  `docker exec drupal-dev curl -s --unix-socket /var/run/control.unit.sock http://localhost/config`.
  Application process state:
  `… http://localhost/status/applications/drupal`.
- **Attaching to a PHP worker (planned `:debug` tag).** A later tag will add
  `gdb`, `rr` and debug symbols. The workers run as `www-data`, and each one
  shows its application name in `ps`
  (`docker exec drupal-dev ps -ef | grep drupal`). Attaching needs
  `--cap-add=SYS_PTRACE --security-opt seccomp=unconfined`. `rr` also needs
  the host's `perf_event_paranoid` at 1 or lower. Pair it with
  `UNITD=unitd-debug`, so that the router and the module have symbols.

## Cron

Set `DRUPAL_CRON_KEY` to have FreeUnit run Drupal's cron itself, on a
schedule, with no client, sidecar process or `automated_cron` involved (see
`docs/adr/0004-schedules.md` and `docs/schedules.md`):

```sh
docker run -d --name drupal-dev -p 8080:80 \
    -e DRUPAL_AUTO_INSTALL=1 \
    -e DRUPAL_CRON_KEY=$(openssl rand -hex 16) \
    -v "$PWD:/var/www/drupal/web" \
    "$IMAGE"
```

`drupal-dev.sh` then renders a `schedules.drupal-cron` object into the
FreeUnit configuration:

```json
"schedules": {
    "drupal-cron": {
        "pass": "applications/drupal/index",
        "uri": "/cron/<DRUPAL_CRON_KEY>",
        "interval": 300,
        "jitter": 15,
        "timeout": 300,
        "overlap": "skip"
    }
}
```

Leave `DRUPAL_CRON_KEY` unset (the default) and no `schedules` member is
added at all — the configuration is exactly what it was before. Drupal's own
cron key is site-specific: read it back with `drush state:get
system.cron_key`, or set `DRUPAL_CRON_KEY` to the value that key already
holds instead of a fresh one, so `/cron/<key>` matches what Drupal expects.

| variable | default | |
|---|---|---|
| `DRUPAL_CRON_KEY` | unset | Drupal's cron key; setting it adds the schedule |
| `DRUPAL_CRON_INTERVAL` | `300` | seconds between runs |
| `DRUPAL_CRON_JITTER` | `15` | up to this many extra seconds, randomized |
| `DRUPAL_CRON_HOST` | unset | `Host` header the run sends; see below |

**Set `DRUPAL_CRON_HOST`.** Drupal's `trusted_host_patterns` rejects a
`Host` it does not expect. With no `DRUPAL_CRON_HOST`, the run has no `Host`
header and falls back to `server_name` `localhost`, which the docroot's
default `settings.php` in this dev kit trusts; a checkout with a narrower
`trusted_host_patterns` needs `DRUPAL_CRON_HOST` set to a host it trusts, or
the cron run gets a `400`.

**The schedule `timeout` does not stop PHP.** It is capped at
`UNIT_APP_TIMEOUT`/`UNIT_APP_TIMEOUT_XDEBUG` (whichever the entrypoint used
for `limits.timeout`), but a cron run that runs past its own `timeout` keeps
executing in an abandoned worker, which still counts against
`applications.drupal.processes.max` (`8` here) until it finishes. See
`docs/schedules.md` for the rest of the caveats: the access log gets the
full `/cron/<key>` URI, and if Drupal's own `automated_cron` is also left on
(it should not be, once a schedule replaces it), a request that finishes
early with `fastcgi_finish_request()` defeats `overlap: "skip"`'s guarantee.

**Fallback**, if the schedule needs to be ruled out or bypassed live: run
cron directly with `docker exec -u www-data drupal-dev drush cron`
(recommended-project; `docker exec` bypasses the entrypoint, so pick the
user yourself), or a curl loop against the control socket or the listener
(see the project root `README.md`, "Schedules plan B").
