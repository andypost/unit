# ddev-freeunit

Serve a [DDEV](https://ddev.com) project with [FreeUnit](https://freeunit.org)
(an application server forked from NGINX Unit, PHP embedded) instead of
`nginx-fpm`/`apache-fpm`.

This directory is laid out as a standalone add-on repository so it can be
split out to `freeunitorg/ddev-freeunit` without restructuring.

## Install

```bash
ddev add-on get /path/to/pkg/ddev/ddev-freeunit   # or freeunitorg/ddev-freeunit once published
ddev restart
```

The first `ddev restart` rebuilds the web image and compiles `unitd` and its
PHP module from the pinned FreeUnit release (a minute or two). Later starts
are as fast as usual.

```bash
ddev freeunit status    # FreeUnit's /status (connections, requests, app processes)
ddev freeunit config    # the configuration it is running
ddev freeunit restart   # re-render the configuration and restart unitd
ddev logs -s web        # FreeUnit's log
```

Remove with `ddev add-on remove freeunit && ddev restart`.

## How it works

`config.freeunit.yaml` sets [`webserver_type: generic`](https://docs.ddev.com/en/stable/users/configuration/config/#webserver_type),
so DDEV starts neither nginx nor php-fpm, and runs
`freeunit/start-freeunit.sh` as a [`web_extra_daemons`](https://docs.ddev.com/en/stable/users/extend/customization-extendibility/#running-extra-daemons-in-the-web-container)
entry under DDEV's supervisord. `web_extra_exposed_ports` sends the project's
normal HTTP(S) traffic from ddev-router to FreeUnit's listener on container
port 80; **ddev-router still terminates TLS**, FreeUnit reads
`X-Forwarded-Proto` so PHP sees `HTTPS=on` and applications generate
`https://` URLs, exactly as with DDEV's nginx.

On every start, `start-freeunit.sh` renders `freeunit/render-config.sh` into
unitd's state directory and `exec`s `unitd --no-daemon`, so `ddev restart`
and `ddev freeunit restart` always converge to the configuration for the
current `DDEV_DOCROOT` and `DDEV_PROJECT_TYPE`. unitd runs as the project
user, like everything else in the web container; its control socket
(`/run/freeunit/control.sock`, mode 0600) is reachable from `ddev exec`
and `ddev ssh` only.

### Routing

- Drupal-family project types (`drupal*`, `backdrop`) get the deny rules of
  this repository's `pkg/docker/drupal/unit-drupal.json` (`.ht*`,
  `sites/*/files/*.php`, `sites/*/private/*` → 403; `vendor/`, dotfiles,
  `*.module`/`*.inc`/`*.yml`/... → 404; only `index.php`, `update.php`,
  `core/install.php`, `core/rebuild.php` and `statistics.php` run).
- Every other type: dotfiles (except `.well-known`) → 404, any `*.php`
  runs, everything else is served from the docroot with `index.php` as the
  fallback front controller.

To change routing, remove the `#ddev-generated` line from
`freeunit/render-config.sh`, edit it, and run `ddev freeunit restart`.

### PHP configuration and Xdebug

At image build time the embed SAPI's `/etc/php/<version>/embed` is replaced
by a symlink to `fpm`, so the PHP that serves requests uses the same
`php.ini`, `.ddev/php/*.ini` overrides and module switches DDEV manages for
php-fpm. `ddev xdebug on|off` therefore works unchanged; because PHP is
loaded once per worker process rather than per request, restart the daemon
afterwards if DDEV did not do it for you:

```bash
ddev xdebug on
ddev freeunit restart
```

## Limitations

- FreeUnit is compiled at web image build time; `ddev restart --no-cache`
  pays that compile again. `FREEUNIT_VERSION` in
  `web-build/Dockerfile.freeunit` pins the release.
- One PHP version per image, `$DDEV_PHP_VERSION`; DDEV rebuilds the image
  when it changes. `php<version>-embed` must exist on deb.sury.org for that
  version (it does for the versions DDEV preinstalls).
- `.ddev/nginx_full` and `.ddev/apache` configuration does not apply.

## Files

```
install.yaml                  # add-on manifest
config.freeunit.yaml          # webserver_type: generic, daemon, ports
web-build/Dockerfile.freeunit # builds unitd + PHP module
freeunit/render-config.sh     # prints the FreeUnit JSON configuration
freeunit/start-freeunit.sh    # web_extra_daemons entrypoint
commands/web/freeunit         # ddev freeunit status|config|restart
tests/test.bats               # bats tests (ddev/github-action-add-on-test)
.github/workflows/tests.yml   # CI, as in ddev/ddev-addon-template
```
