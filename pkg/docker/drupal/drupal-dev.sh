#!/bin/bash
#
# FreeUnit Drupal core dev kit entrypoint.
#
#   serve              start FreeUnit in the foreground (the default);
#                      with DRUPAL_AUTO_INSTALL=1, run "si" first if needed
#   si [drush args]    composer install if needed, then install Drupal on SQLite
#   test <path> [...]  run PHPUnit against the running FreeUnit
#   lint [paths...]    phpcs (drupal/coder), then phpstan when the project has it
#   shell              an interactive shell in the project, as the web user
#   unitd|unitd-debug  hand straight to docker-entrypoint.sh (base image behaviour)
#   anything else      exec'd as given
#
# The FreeUnit configuration is applied by the base image's
# /usr/local/bin/docker-entrypoint.sh, from /docker-entrypoint.d/.  This script
# only renders that file (limits.timeout depends on XDEBUG) and toggles Xdebug.

set -euo pipefail

DRUPAL_BASE=${DRUPAL_BASE:-/var/www/drupal}
DRUPAL_DOCROOT=${DRUPAL_DOCROOT:-$DRUPAL_BASE/web}
DRUPAL_PROFILE=${DRUPAL_PROFILE:-standard}
DRUPAL_ADMIN_USER=${DRUPAL_ADMIN_USER:-admin}
DRUPAL_ADMIN_PASS=${DRUPAL_ADMIN_PASS:-admin}
DRUPAL_SITE_NAME=${DRUPAL_SITE_NAME:-FreeUnit Drupal}
DRUPAL_DB_URL=${DRUPAL_DB_URL:-sqlite://sites/default/files/.ht.sqlite}

# Cron via FreeUnit's "schedules" (docs/adr/0004-schedules.md), in place of
# "drush cron" or "automated_cron". Unset (the default): no schedule is
# added, and the "Cron" section of README.md's fallback still applies.
DRUPAL_CRON_KEY=${DRUPAL_CRON_KEY:-}
DRUPAL_CRON_INTERVAL=${DRUPAL_CRON_INTERVAL:-300}
DRUPAL_CRON_JITTER=${DRUPAL_CRON_JITTER:-15}
DRUPAL_CRON_HOST=${DRUPAL_CRON_HOST:-}

WEB_USER=${WEB_USER:-www-data}

UNITD=${UNITD:-unitd}
UNIT_CONTROL=/var/run/control.unit.sock
UNIT_CONF_TEMPLATE=/usr/share/unit/drupal/unit-drupal.json
UNIT_CONF=/docker-entrypoint.d/unit-drupal.json
UNIT_APP_TIMEOUT=${UNIT_APP_TIMEOUT:-300}
UNIT_APP_TIMEOUT_XDEBUG=${UNIT_APP_TIMEOUT_XDEBUG:-3600}

XDEBUG=${XDEBUG:-0}
XDEBUG_INI=/usr/local/etc/php/conf.d/zz-drupal-dev-xdebug.ini

export SIMPLETEST_BASE_URL=${SIMPLETEST_BASE_URL:-http://localhost}
export SIMPLETEST_DB=${SIMPLETEST_DB:-sqlite://localhost//tmp/test.sqlite}
export COMPOSER_HOME=${COMPOSER_HOME:-/tmp/composer-home}
export COMPOSER_CACHE_DIR=${COMPOSER_CACHE_DIR:-/tmp/composer-cache}

TOOLS_BIN=/opt/drupal-tools/vendor/bin

log() { echo "drupal-dev: $*" >&2; }
die() { log "error: $*"; exit 1; }


# Where composer.json lives.  A drupal/drupal core clone mounted at the
# docroot carries it at the docroot; a drupal/recommended-project carries it
# one level up, with the docroot in web/.
project_root()
{
    if [ -f "$DRUPAL_BASE/composer.json" ]; then
        echo "$DRUPAL_BASE"
    elif [ -f "$DRUPAL_DOCROOT/composer.json" ]; then
        echo "$DRUPAL_DOCROOT"
    else
        echo ""
    fi
}

is_core_clone()
{
    [ -f "$DRUPAL_DOCROOT/core/lib/Drupal.php" ] \
        && [ -f "$DRUPAL_DOCROOT/composer.json" ] \
        && [ ! -f "$DRUPAL_BASE/composer.json" ]
}


# Give the web user the uid/gid that owns the bind-mounted checkout, so that
# files written by Drupal, Composer and PHPUnit stay editable on the host.
match_owner()
{
    local uid gid

    [ "$(id -u)" = 0 ] || return 0
    [ -d "$DRUPAL_BASE" ] || return 0

    uid=$(stat -c %u "$DRUPAL_BASE")
    gid=$(stat -c %g "$DRUPAL_BASE")

    [ "$uid" != 0 ] || return 0

    if [ "$(id -u "$WEB_USER")" != "$uid" ]; then
        usermod -o -u "$uid" "$WEB_USER" >/dev/null
    fi

    if [ "$(id -g "$WEB_USER")" != "$gid" ]; then
        groupmod -o -g "$gid" "$WEB_USER" >/dev/null
    fi
}


# Run a command as the web user, keeping the environment.
as_web()
{
    mkdir -p "$COMPOSER_HOME" "$COMPOSER_CACHE_DIR"
    chown "$WEB_USER:" "$COMPOSER_HOME" "$COMPOSER_CACHE_DIR" 2>/dev/null || true

    if [ "$(id -u)" = 0 ]; then
        HOME=/tmp setpriv --reuid="$WEB_USER" --regid="$WEB_USER" \
            --init-groups -- "$@"
    else
        "$@"
    fi
}


xdebug_setup()
{
    if [ "$XDEBUG" = 1 ]; then
        cat > "$XDEBUG_INI" <<EOF
zend_extension=xdebug
xdebug.mode=${XDEBUG_MODE:-debug,develop}
xdebug.start_with_request=${XDEBUG_START_WITH_REQUEST:-trigger}
xdebug.client_host=${XDEBUG_CLIENT_HOST:-host.docker.internal}
xdebug.client_port=${XDEBUG_CLIENT_PORT:-9003}
xdebug.discover_client_host=0
xdebug.log_level=0
EOF
        log "Xdebug on (mode ${XDEBUG_MODE:-debug,develop}," \
            "client ${XDEBUG_CLIENT_HOST:-host.docker.internal}:${XDEBUG_CLIENT_PORT:-9003})"
    else
        rm -f "$XDEBUG_INI"
    fi
}


# Render the FreeUnit configuration.  A worker stopped on a breakpoint is,
# to the router, a request that does not answer, and limits.timeout would end
# it with a 503 -- so the step-debugging timeout is much longer.
unit_conf()
{
    local timeout

    if [ "$XDEBUG" = 1 ]; then
        timeout=$UNIT_APP_TIMEOUT_XDEBUG
    else
        timeout=$UNIT_APP_TIMEOUT
    fi

    mkdir -p /docker-entrypoint.d

    # shellcheck disable=SC2016  # PHP source, expanded by PHP, not the shell.
    TEMPLATE="$UNIT_CONF_TEMPLATE" OUT="$UNIT_CONF" TIMEOUT="$timeout" \
    DOCROOT="${DRUPAL_DOCROOT%/}" WEB_USER="$WEB_USER" \
    CRON_KEY="$DRUPAL_CRON_KEY" CRON_INTERVAL="$DRUPAL_CRON_INTERVAL" \
    CRON_JITTER="$DRUPAL_CRON_JITTER" CRON_HOST="$DRUPAL_CRON_HOST" \
    php -n -r '
        $raw = file_get_contents(getenv("TEMPLATE"));
        $raw = str_replace("/var/www/drupal/web", getenv("DOCROOT"), $raw);
        $conf = json_decode($raw, true, 512, JSON_THROW_ON_ERROR);
        $conf["applications"]["drupal"]["limits"]["timeout"] = (int) getenv("TIMEOUT");
        $conf["applications"]["drupal"]["user"] = getenv("WEB_USER") ?: "www-data";
        $conf["applications"]["drupal"]["group"] = getenv("WEB_USER") ?: "www-data";

        // FreeUnit "schedules": Drupal cron, in place of "drush cron" or
        // "automated_cron" (docs/adr/0004-schedules.md, docs/schedules.md).
        // The cron key is per site, so the schedule is only added when
        // DRUPAL_CRON_KEY is set; leaving it unset keeps the config exactly
        // as before, with no "schedules" member at all.
        $cronKey = getenv("CRON_KEY");

        if ($cronKey !== false && $cronKey !== "") {
            $timeout = (int) getenv("TIMEOUT");
            $scheduleTimeout = min($timeout, (int) getenv("CRON_INTERVAL"));
            $host = getenv("CRON_HOST");

            $schedule = [
                "pass" => "applications/drupal/index",
                "uri" => "/cron/" . $cronKey,
                "interval" => (int) getenv("CRON_INTERVAL"),
                "jitter" => (int) getenv("CRON_JITTER"),
                // At or below limits.timeout (ADR 0004 R5/§7.2): a timeout
                // does not stop the worker, so it must not outlive the
                // application deadline that would eventually reclaim it.
                "timeout" => $scheduleTimeout > 0 ? $scheduleTimeout : $timeout,
                "overlap" => "skip",
            ];

            // trusted_host_patterns rejects a Host it does not expect
            // (ADR 0004 R4); with no DRUPAL_CRON_HOST, the run falls back to
            // "localhost", which the default drupal-dev settings.php trusts.
            if ($host !== false && $host !== "") {
                $schedule["headers"] = ["Host" => $host];
            }

            $conf["schedules"]["drupal-cron"] = $schedule;
        }

        file_put_contents(getenv("OUT"), json_encode($conf,
            JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n");
    '

    if [ -n "$DRUPAL_CRON_KEY" ]; then
        log "FreeUnit config rendered to $UNIT_CONF (limits.timeout=${timeout}s," \
            "schedules.drupal-cron every ${DRUPAL_CRON_INTERVAL}s)"
    else
        log "FreeUnit config rendered to $UNIT_CONF (limits.timeout=${timeout}s," \
            "no cron schedule: set DRUPAL_CRON_KEY to add one)"
    fi
}


prepare()
{
    match_owner
    xdebug_setup
    unit_conf

    # docker-entrypoint.sh only configures an empty state directory.  The
    # configuration here is generated from the environment on every start,
    # so start from an empty one unless asked to keep it.
    if [ "${UNIT_KEEP_STATE:-0}" != 1 ] && [ ! -S "$UNIT_CONTROL" ]; then
        rm -rf /var/lib/unit/*
    fi
}


unit_running()
{
    [ -S "$UNIT_CONTROL" ] \
        && curl -fs --unix-socket "$UNIT_CONTROL" http://localhost/ >/dev/null
}


# For the subcommands: start FreeUnit in the background when the container was
# started with one of them instead of "serve".
ensure_unit()
{
    unit_running && return 0

    prepare
    /usr/local/bin/docker-entrypoint.sh "$UNITD" --control "unix:$UNIT_CONTROL"

    for _ in $(seq 20); do
        unit_running && return 0
        sleep 0.5
    done

    die "FreeUnit did not come up; see /var/log/unit.log"
}


cmd_si()
{
    local project

    project=$(project_root)
    [ -n "$project" ] || die "no composer.json in $DRUPAL_BASE or $DRUPAL_DOCROOT"

    match_owner

    if [ ! -f "$project/vendor/autoload.php" ]; then
        log "composer install in $project"
        (cd "$project" && as_web composer install --no-interaction --no-progress)
    fi

    as_web mkdir -p "$DRUPAL_DOCROOT/sites/default/files"

    if [ -x "$project/vendor/bin/drush" ]; then
        log "drush site:install $DRUPAL_PROFILE on $DRUPAL_DB_URL"
        (cd "$DRUPAL_DOCROOT" && as_web "$project/vendor/bin/drush" -y \
            site:install "$DRUPAL_PROFILE" \
            --db-url="$DRUPAL_DB_URL" \
            --account-name="$DRUPAL_ADMIN_USER" \
            --account-pass="$DRUPAL_ADMIN_PASS" \
            --site-name="$DRUPAL_SITE_NAME" "$@")
    else
        # A core clone has no Drush.  Core's own installer also targets SQLite
        # (sites/default/files/.sqlite) and prints the admin password.
        log "no Drush in $project/vendor/bin; using core/scripts/drupal install"
        (cd "$DRUPAL_DOCROOT" && as_web php core/scripts/drupal install \
            "$DRUPAL_PROFILE" --site-name="$DRUPAL_SITE_NAME" \
            --no-interaction "$@")
    fi

    if [ "${SI_NO_UNIT:-0}" != 1 ]; then
        ensure_unit
    fi

    log "site installed: $SIMPLETEST_BASE_URL (inside the container)"
}


site_installed()
{
    grep -qs "databases\['default'\]" "$DRUPAL_DOCROOT/sites/default/settings.php"
}


cmd_test()
{
    local project config

    [ $# -ge 1 ] || die "usage: test <path> [phpunit args...]"

    project=$(project_root)
    [ -n "$project" ] || die "no composer.json in $DRUPAL_BASE or $DRUPAL_DOCROOT"
    [ -x "$project/vendor/bin/phpunit" ] || die "no vendor/bin/phpunit; run 'si' first"

    if [ -f "$DRUPAL_DOCROOT/core/phpunit.xml" ]; then
        config=$DRUPAL_DOCROOT/core/phpunit.xml
    else
        config=$DRUPAL_DOCROOT/core/phpunit.xml.dist
    fi

    ensure_unit

    export BROWSERTEST_OUTPUT_DIRECTORY=${BROWSERTEST_OUTPUT_DIRECTORY:-$DRUPAL_DOCROOT/sites/simpletest/browser_output}
    as_web mkdir -p "$BROWSERTEST_OUTPUT_DIRECTORY"

    log "phpunit -c $config $* (SIMPLETEST_BASE_URL=$SIMPLETEST_BASE_URL," \
        "SIMPLETEST_DB=$SIMPLETEST_DB)"

    cd "$DRUPAL_DOCROOT"
    as_web "$project/vendor/bin/phpunit" -c "$config" "$@"
}


# Files changed against HEAD, for lint without arguments.
changed_files()
{
    git -C "$1" -c safe.directory='*' diff --name-only --diff-filter=ACMR HEAD \
        2>/dev/null || true
    git -C "$1" -c safe.directory='*' ls-files --others --exclude-standard \
        2>/dev/null || true
}


cmd_lint()
{
    local project phpcs phpstan standard rc=0
    local -a paths

    project=$(project_root)
    [ -n "$project" ] || die "no composer.json in $DRUPAL_BASE or $DRUPAL_DOCROOT"

    if [ $# -gt 0 ]; then
        paths=("$@")
    else
        mapfile -t paths < <(changed_files "$project" | sort -u)
        [ ${#paths[@]} -gt 0 ] || { log "no changed files to lint"; return 0; }
    fi

    if [ -x "$project/vendor/bin/phpcs" ]; then
        phpcs=$project/vendor/bin/phpcs
    else
        phpcs=$TOOLS_BIN/phpcs
    fi

    if is_core_clone && [ -f "$DRUPAL_DOCROOT/core/phpcs.xml.dist" ]; then
        standard=$DRUPAL_DOCROOT/core/phpcs.xml.dist
    else
        standard=Drupal,DrupalPractice
    fi

    log "phpcs --standard=$standard (${#paths[@]} paths)"
    (cd "$project" && as_web "$phpcs" -p --standard="$standard" \
        --extensions=php,module,inc,install,test,profile,theme,info,yml \
        "${paths[@]}") || rc=$?

    phpstan=$project/vendor/bin/phpstan

    if [ -x "$phpstan" ]; then
        local conf=""

        for c in "$DRUPAL_DOCROOT/core/phpstan.neon.dist" \
                 "$project/phpstan.neon" "$project/phpstan.neon.dist"
        do
            if [ -f "$c" ]; then conf=$c; break; fi
        done

        log "phpstan analyse ${conf:+-c $conf}"
        (cd "$project" && as_web "$phpstan" analyse --no-progress \
            --memory-limit=2G ${conf:+-c "$conf"} "${paths[@]}") || rc=$?
    else
        log "no vendor/bin/phpstan; skipping phpstan"
    fi

    return $rc
}


cmd_shell()
{
    local project

    project=$(project_root)
    cd "${project:-$DRUPAL_BASE}" 2>/dev/null || cd /

    match_owner
    unit_running || log "FreeUnit is not running here (start the container with 'serve')"

    if [ "$(id -u)" = 0 ] && [ "${SHELL_USER:-$WEB_USER}" != root ]; then
        export HOME=/tmp
        exec setpriv --reuid="${SHELL_USER:-$WEB_USER}" \
            --regid="${SHELL_USER:-$WEB_USER}" --init-groups -- bash -l
    fi

    exec bash -l
}


usage()
{
    sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//'
}


cmd=${1:-serve}
[ $# -gt 0 ] && shift

case "$cmd" in
    serve)
        # DRUPAL_AUTO_INSTALL=1: one `docker run` gives an installed site.
        if [ "${DRUPAL_AUTO_INSTALL:-0}" = 1 ] && ! site_installed; then
            SI_NO_UNIT=1 cmd_si
        fi

        prepare
        exec /usr/local/bin/docker-entrypoint.sh "$UNITD" --no-daemon \
            --control "unix:$UNIT_CONTROL"
        ;;
    unitd|unitd-debug)
        prepare
        exec /usr/local/bin/docker-entrypoint.sh "$cmd" "$@"
        ;;
    si)
        cmd_si "$@"
        ;;
    test)
        cmd_test "$@"
        ;;
    lint)
        cmd_lint "$@"
        ;;
    shell)
        cmd_shell
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        exec "$cmd" "$@"
        ;;
esac
