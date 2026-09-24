# ddev-freeunit review log

Adversarial review of the add-on prototype (DDEV maintainer + security
reviewer hat), followed by fixes, in up to three rounds. Line numbers refer
to the files as they were at the start of each round.

Reference material (fetched from raw.githubusercontent.com; docs.ddev.com
and packages.sury.org are blocked from the review sandbox, so anything
marked *unverified* was not checked against a live source):

- `ddev/ddev` `main`: `pkg/ddevapp/config.go` (generated Dockerfile,
  supervisord program template for `web_extra_daemons`), `pkg/ddevapp/ddevapp.go`
  (container environment), `containers/ddev-webserver/Dockerfile` and
  `ddev-webserver-base-scripts/start.sh`, `docs/content/users/extend/*.md`,
  `docs/content/users/configuration/config.md`.
- `ddev/ddev-addon-template` `main`: `install.yaml`, `tests/test.bats`,
  `.github/workflows/tests.yml`.
- `ddev/ddev-frankenphp` `main`: the only other `webserver_type: generic`
  add-on that replaces PHP-FPM with an embedded PHP.
- FreeUnit sources in this repository: `src/nxt_conf_validation.c`,
  `src/nxt_http_request.c` (`forwarded`), `src/nxt_isolation.c`,
  `src/nxt_runtime.c`, `src/nxt_listen_socket.c` (control socket mode),
  `src/nxt_php_sapi.c` (`HTTPS`), `docs/schedules.md`.

Severity: **blocking** = the add-on cannot work as shipped; **major** =
wrong behaviour, security or maintainability problem a maintainer would
reject; **minor** = cleanup.

## Round 1

### Blocking

1. **`${DDEV_APPROOT}` does not exist inside the web container.**
   `config.freeunit.yaml:16`, `commands/web/freeunit:19`,
   `commands/web/xdebug:29`, `freeunit/render-config.sh:15`.
   DDEV exports `DDEV_DOCROOT`, `DDEV_PROJECT_TYPE`, `DDEV_PHP_VERSION`, ...
   into the container (`ddevapp.go`, `DDEV_*` list) but not `DDEV_APPROOT`,
   which is a host-side install-action variable. The supervisord program
   therefore runs `bash /.ddev/freeunit/start-freeunit.sh` (or a host path)
   and the daemon never starts; `ddev freeunit reload` cannot find
   `render-config.sh` either.
   *Fix:* use `/mnt/ddev_config/...` (the `.ddev` bind mount DDEV checks on
   every start) for the scripts and `/var/www/html` for the project.

2. **Everything under `/var/run` and `/var/lib/freeunit` is root-owned, but
   the daemon runs as the project user.** `freeunit/start-freeunit.sh:15-32`,
   `web-build/Dockerfile.freeunit:28-35,56`. `start.sh` in ddev-webserver
   `exec`s supervisord as the (non-root) container user, and
   `web_extra_daemons` programs inherit that. `rm -f /var/run/...sock`,
   the pid file, the state dir and `/var/run/freeunit.json` all fail with
   EACCES; supervisord retries 15 times and gives up.
   *Fix:* a `/run/freeunit` directory created in the Dockerfile and owned by
   `$uid:$gid` (exactly what DDEV's own generated Dockerfile does for
   `/run/php`), and configure-time defaults pointing into it.

3. **`"user": "www-data"` / `"group": "www-data"` on the application.**
   `freeunit/render-config.sh:184-185`. An unprivileged `unitd` cannot
   change credentials; `nxt_isolation_main_prefork()`
   (`src/nxt_isolation.c:108-125`) rejects an app whose `user` differs from
   the configured default with "cannot set user ... missing capabilities".
   It only works today because `configure --user=www-data` happens to match.
   The app must run as the launching user anyway (files written by PHP must
   belong to the host user, as with php-fpm in DDEV).
   *Fix:* drop both keys and `--user/--group` from configure.

4. **`ddev freeunit log` tails `/dev/stderr`.**
   `web-build/Dockerfile.freeunit:57` symlinks `/var/log/freeunit.log` to
   `/dev/stderr`; `commands/web/freeunit:63` runs `tail -f` on it. This can
   never show anything.
   *Fix:* log to stderr so `ddev logs -s web` shows it (the DDEV way for
   `web_extra_daemons`), and drop the `log` subcommand.

5. **The Dockerfile builds `unitd` twice and needs `git`.**
   `web-build/Dockerfile.freeunit:21-55`: `make unitd`, `make clean`,
   reconfigure, `make php-install` recompiles all of `libnxt`, doubling
   build time on every image rebuild; `git clone --depth 1` needs `git` and
   a writable `.git`.
   *Fix:* one `./configure`, one `./configure php`, one `make -j`, then
   `make install php-install`; fetch the pinned tag tarball with `curl`
   (`https://github.com/freeunitorg/freeunit/archive/refs/tags/<tag>.tar.gz`
   redirects to codeload and returns 200, verified).

### Major

6. **No `X-Forwarded-Proto` handling, so PHP never sees `HTTPS=on`.**
   `freeunit/render-config.sh:159-161`. ddev-router terminates TLS and
   forwards plain HTTP with `X-Forwarded-Proto: https`; DDEV's nginx maps
   that to `fastcgi_param HTTPS on`. Without the listener's `forwarded`
   option (`src/nxt_conf_validation.c:598-616`,
   `src/nxt_http_request.c:333-424` sets `r->tls`, `src/nxt_php_sapi.c:1623`
   sets `HTTPS`), Drupal/Symfony/WordPress generate `http://` URLs and
   redirect loops on `https://<project>.ddev.site`.
   *Fix:* `"forwarded": {"protocol": "X-Forwarded-Proto", "client_ip":
   "X-Forwarded-For", "source": [...]}`; trust the container's own networks
   (RFC1918 + loopback), which is the same trust DDEV's nginx config extends.

7. **Overriding DDEV's built-in `xdebug` command.** `commands/web/xdebug`
   (whole file). Global commands are copied into every project on start;
   shadowing one with a project command means `ddev xdebug info` disappears,
   `xdebug_enabled: true` in `config.yaml` (handled by `start.sh` →
   `enable_xdebug`) is not covered, and any change to the global command's
   contract silently breaks the add-on. ddev-frankenphp deliberately deleted
   its own override (`install.yaml` "Removing old files") and instead makes
   the daemon restart pick the change up.
   *Fix:* remove the override. Make the `embed` SAPI share the `fpm` SAPI's
   configuration (`/etc/php/X/embed -> fpm` symlink at build time), so
   whatever DDEV does to php-fpm (`enable_xdebug`, `.ddev/php/*.ini`,
   xhprof, blackfire) applies to FreeUnit too, and provide
   `ddev freeunit restart` (`supervisorctl restart webextradaemons:freeunit`)
   for DDEV versions whose `enable_xdebug` does not already restart
   `web_extra_daemons` (*unverified* which versions do; ddev-frankenphp's
   Dockerfile says its supervisorctl patch "is needed only for DDEV versions
   prior to v1.25.0").

8. **Served PHP ignores DDEV's PHP configuration.**
   `freeunit/render-config.sh:194-199`. The embed SAPI reads
   `/etc/php/X/embed/php.ini` and `embed/conf.d`, which DDEV never touches
   (`start.sh` only copies `.ddev/php/*.ini` into `cli` and `fpm`), so
   project ini overrides, DDEV's upload limits etc. do not apply; the
   add-on papers over it with its own `options.admin` block and a
   `FREEUNIT_MEMORY_LIMIT` knob.
   *Fix:* the symlink from finding 7; delete `options.admin`.

9. **Non-Drupal projects get no deny rules at all.**
   `freeunit/render-config.sh:111-118,171-177`. DDEV's
   `nginx-site-php.conf` denies `/\.(?!well-known)` for every project type;
   here a `php`, `laravel`, `wordpress`... project serves `/.env`,
   `/.git/config`, `/.htaccess`, and with an empty docroot the whole
   `.ddev/` directory.
   *Fix:* a dotfile deny (`*/.*` except `*/.well-known/*`) for every
   project type; keep the Drupal-specific rules on top for Drupal types.

10. **Cron via `schedules` is speculative and leaks the cron key.**
    `freeunit/render-config.sh:120-155`, README "Cron". The key lands in
    `.ddev/config.yaml` (committed), the container environment
    (`ddev exec env`), `/var/run/freeunit.json` and `GET /config`; and
    FreeUnit's own `docs/schedules.md` says Drupal needs `headers.Host`,
    which is not set. DDEV users have `ddev drush cron`.
    *Fix:* remove.

11. **Xdebug-dependent `limits.timeout` and the `FREEUNIT_*` knobs.**
    `freeunit/render-config.sh:19-33`. Rendering depends on `php -m` at
    render time, so the served timeout depends on *when* the daemon was
    last restarted; five environment knobs for a dev server nobody asked
    for.
    *Fix:* one fixed generous `limits.timeout` (PHP's own
    `max_execution_time` still bounds scripts, and Xdebug sets it to 0 while
    stepping), fixed `processes`; anyone who needs more edits the script
    (and removes `#ddev-generated`).

12. **`pre_install_actions` webserver_type check is wrong and hostile.**
    `install.yaml:7-14`. DDEV merges `config.*.yaml` after `config.yaml`,
    so `config.freeunit.yaml`'s `webserver_type: generic` wins whatever
    `config.yaml` says (ddev-frankenphp relies on this). The check makes
    installation fail on a perfectly normal project.
    *Fix:* remove.

13. **`ddev_version_constraint: '>= v1.24.0'`.** `install.yaml:41`.
    `webserver_type: generic` as the primary web server is documented as
    v1.24.3+; the template uses `>= v1.24.10`.
    *Fix:* `>= v1.24.10`.

14. **`php${v}-dev` stays in the image.** `config.freeunit.yaml:27-29`
    installs the build-only `-dev` package via `webimage_extra_packages`,
    where it cannot be purged.
    *Fix:* install `-embed` and `-dev` in the Dockerfile's single `RUN`
    and purge `-dev` with the toolchain.

15. **Non-Drupal projects serve PHP sources as static files.**
    `freeunit/render-config.sh:171-177`. With only `share` + `fallback`,
    any `*.php` other than the front controller (WordPress `wp-login.php`,
    `wp-admin/*.php`, a plain project's `info.php`) exists on disk, so
    `share` returns its source instead of running it; only Drupal types get
    the "other `*.php` → 404" rule.
    *Fix:* for non-Drupal types route `*.php` and `*.php/*` to the
    application's `direct` target before the `share` rule.

16. **Tests do not cover what the add-on is for.** `tests/test.bats:317-345`
    checks a PHP page, a static file, the `Server` header and the commands,
    but not HTTPS detection (finding 6), deny rules (finding 9) or Xdebug
    (finding 7).
    *Fix:* `index.php` echoes `HTTPS` and `xdebug` state; assert
    `HTTPS=on` over `https://`, `.env` → 404, and `ddev xdebug on` +
    `ddev freeunit restart` → Xdebug loaded.

### Minor

17. `web-build/Dockerfile.freeunit:20`: `libssl-dev` and `zlib1g-dev` are
    unused (no `--openssl`/`--zlib`). `:24-25`: falling back to an
    unversioned `php-config` hides a wrong-version build. `:62`:
    `unitd-freeunit --version | head -1` is noise. `:40`: the
    `unitd-freeunit` name is unnecessary, nothing else provides `unitd` in
    ddev-webserver.
18. `freeunit/start-freeunit.sh:35-59`: the wait-loop + `PUT` can be
    dropped entirely: `unitd` loads `<statedir>/conf.json` at startup
    (`src/nxt_runtime.c:958`), so the script can render straight into the
    state dir and `exec unitd --no-daemon`. That also removes the `trap`
    and the double `kill`.
19. `commands/web/freeunit:7`: `## ExecRaw: false` is the default;
    `curl_control_put_config` is used before it is defined (works in bash,
    reads badly).
20. `install.yaml:24-34`: two `post_install_actions` to `chmod` and to
    print; keep one `chmod` for the command (DDEV preserving the mode bit
    on `project_files` is *unverified*), drop the prose. `removal_actions`
    only prints; DDEV already tells the user to restart.
21. `README.md`: 270 lines, half of it a design essay ("Alternative
    approach considered", comparison table). Cut to what a user of the
    add-on needs.
22. Security, checked and fine: the control socket is created `0600`
    (`src/nxt_listen_socket.c:135`) and owned by the project user, so only
    `ddev exec`/`ddev ssh` (same user) and root can change the
    configuration; there is no TCP control listener.
23. `.github/workflows/tests.yml` is identical to the template; fine.
    Building `unitd` from source on `ubuntu-24.04-arm` runners is slow but
    acceptable.
24. `tests/test.bats:286`: `GITHUB_REPO=freeunitorg/ddev-freeunit` does not
    exist yet; the `release` test can only pass after the split-out. Note
    it in the file.

### Fixed in round 1

All of 1-14, 16-21, 24. Result: `commands/web/xdebug` deleted, the cron and
knob code deleted, `start-freeunit.sh` down to a render + `exec`, the
Dockerfile to one `configure`/`make`, the README to what is needed.

## Round 2 (review of the round-1 diff)

1. **major** — `forwarded.source` listed RFC1918 ranges only
   (`freeunit/render-config.sh:66`). Docker networks can be IPv6 ULA or a
   non-default pool, in which case the header would be ignored and PHP
   would see `HTTPS=off` again, silently. DDEV's nginx trusts the header
   unconditionally. *Fixed:* `["0.0.0.0/0", "::/0"]` (accepted by
   `nxt_http_route_addr_pattern_parse`, verified at runtime).
2. **major** — the `curl | tar` pipeline in the Dockerfile ran under
   `set -eu` only (`web-build/Dockerfile.freeunit:12`): a failed download
   would leave an empty tree and a confusing `configure` error. *Fixed:*
   `set -eu -o pipefail` (DDEV's generated Dockerfile sets
   `SHELL ["/bin/bash", "-c"]`).
3. **minor** — comment lines inside the `RUN` continuation
   (`web-build/Dockerfile.freeunit:38-43`) rely on the Dockerfile parser
   stripping them. *Fixed:* moved above the instruction.
4. **minor** — `exec unitd` depended on `/usr/sbin` being in supervisord's
   `PATH` (`freeunit/start-freeunit.sh:14`). *Fixed:* absolute path.
5. **minor** — README claimed `php<version>-embed` exists on deb.sury.org
   for every preinstalled version; packages.sury.org is unreachable from
   here, so that is unverified. *Fixed:* stated as a requirement, not a
   fact; also documented that the log goes to the container output by
   reopening stderr.
6. Checked and left as is: a stale `control.sock` is replaced by unitd
   itself (`src/nxt_listen_socket.c:105-120` binds a `.tmp` path and
   renames over it), so the start script needs no `rm -f`; `stopasgroup`
   in DDEV's supervisord program delivers `TERM` straight to the `exec`'d
   unitd; `--auto-remove` after purging the toolchain only removes
   packages that were installed automatically and are no longer required,
   and DDEV's base image already ran `apt-get autoremove`, so nothing the
   image relied on before this step can go.
7. Runtime verification extended: `PATH_INFO` (`/index.php/sub`), a missing
   `*.php` (404 from the PHP module, not the source), a directory without
   trailing slash (301).

## Round 3 (review of the round-2 diff)

Read through the final tree once more; nothing blocking or major left.
Remaining, deliberately not changed:

- `post_install_actions` keeps one `chmod +x` for `commands/web/freeunit`
  because it is *unverified* whether `ddev add-on get` preserves the
  executable bit of `project_files`; the other scripts are run via `bash`
  and need no bit.
- Whether DDEV's own `enable_xdebug` restarts `web_extra_daemons` on
  `webserver_type: generic` (and from which version) is *unverified*;
  `ddev freeunit restart` and the bats test cover both cases.
- `limits.timeout: 3600` is generous on purpose (Xdebug); PHP's
  `max_execution_time` still applies.
- The `release` bats test cannot pass until the add-on has its own
  repository.
