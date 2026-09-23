#!/usr/bin/env bash
#ddev-generated
#
# Render a FreeUnit configuration for this DDEV project and print it to
# stdout. Adapted from pkg/docker/drupal/unit-drupal.json in the FreeUnit
# source tree: same deny rules, same share+fallback routing, same
# limits.timeout treatment for Xdebug.
#
# Called by start-freeunit.sh on every (re)start, and by
# `ddev freeunit reload`. Safe to call any number of times: it only reads
# the environment and writes to stdout, so re-running it is idempotent.

set -euo pipefail

DOCROOT="${DDEV_APPROOT:-/var/www/html}/${DDEV_DOCROOT:-}"
DOCROOT="${DOCROOT%/}"
PROJECT_TYPE="${DDEV_PROJECT_TYPE:-php}"

# limits.timeout: an Xdebug session paused on a breakpoint looks like an
# application that stopped answering. FreeUnit's router would then abandon
# the request with a 503 while you are still stepping through it, so give
# it much longer whenever Xdebug is loaded for the embedded SAPI.
XDEBUG_ENABLED=0
if php -m 2>/dev/null | grep -qi '^xdebug$'; then
    XDEBUG_ENABLED=1
fi
TIMEOUT=${FREEUNIT_APP_TIMEOUT:-60}
if [ "$XDEBUG_ENABLED" = 1 ]; then
    TIMEOUT=${FREEUNIT_APP_TIMEOUT_XDEBUG:-3600}
fi

PROCESSES_MAX=${FREEUNIT_PROCESSES_MAX:-4}
PROCESSES_SPARE=${FREEUNIT_PROCESSES_SPARE:-1}

# Drupal-flavored deny/route rules for drupal, drupal7..drupalN, backdrop.
# Anything else (plain php, laravel, symfony, wordpress, ...) gets the
# plain share+fallback-to-index.php rule further down, which works for any
# front-controller-style app that expects /index.php to run for
# non-file requests.
drupal_routes() {
    cat <<JSON
        {
            "match": {
                "uri": [
                    "*/.ht*",
                    "/sites/*/files/*.php",
                    "/sites/*/private/*"
                ]
            },
            "action": { "return": 403 }
        },
        {
            "match": {
                "uri": [
                    "/vendor/*",
                    "!*/.well-known/*",
                    "*/.*",
                    "*.engine",
                    "*.inc",
                    "*.install",
                    "*.make",
                    "*.module",
                    "*.po",
                    "*.profile",
                    "*.sh",
                    "*.theme",
                    "*.twig",
                    "*.xtmpl",
                    "*.yml",
                    "*.sql",
                    "*/composer.json",
                    "*/composer.lock",
                    "*/web.config",
                    "*.bak",
                    "*.orig",
                    "*.save",
                    "*.swo",
                    "*.swp",
                    "*~"
                ]
            },
            "action": { "return": 404 }
        },
        {
            "match": {
                "uri": [
                    "/core/install.php",
                    "/core/install.php/*",
                    "/core/rebuild.php",
                    "/update.php",
                    "/update.php/*",
                    "/core/modules/statistics/statistics.php"
                ]
            },
            "action": { "pass": "applications/app/direct" }
        },
        {
            "match": {
                "uri": [
                    "!/index.php",
                    "!/index.php/*",
                    "*.php",
                    "*.php/*"
                ]
            },
            "action": { "return": 404 }
        },
JSON
}

case "$PROJECT_TYPE" in
    drupal|drupal6|drupal7|drupal8|drupal9|drupal10|drupal11|drupal12|backdrop)
        IS_DRUPAL=1
        ;;
    *)
        IS_DRUPAL=0
        ;;
esac

# An optional cron schedule, driven by the same key Drupal's automated_cron
# and drush cron use: set FREEUNIT_CRON_KEY (and, optionally,
# FREEUNIT_CRON_INTERVAL, default 300s) in .ddev/config.yaml's
# web_environment, and FreeUnit's own "schedules" feature (see
# docs/adr/0004-schedules.md upstream) issues the request itself, with no
# extra cron daemon in the container.
SCHEDULES=""
if [ -n "${FREEUNIT_CRON_KEY:-}" ]; then
    CRON_INTERVAL=${FREEUNIT_CRON_INTERVAL:-300}
    # jitter must not exceed interval (FreeUnit's config validator rejects
    # it otherwise) -- keep it small but always valid for a short interval.
    CRON_JITTER=15
    if [ "$CRON_JITTER" -gt "$CRON_INTERVAL" ]; then
        CRON_JITTER=$CRON_INTERVAL
    fi
    if [ "$IS_DRUPAL" = 1 ]; then
        CRON_URI="/cron/${FREEUNIT_CRON_KEY}"
    else
        CRON_URI="/${FREEUNIT_CRON_KEY}"
    fi
    SCHEDULES=$(cat <<JSON
,
    "schedules": {
        "cron": {
            "pass": "applications/app/index",
            "uri": "${CRON_URI}",
            "interval": ${CRON_INTERVAL},
            "jitter": ${CRON_JITTER},
            "timeout": ${TIMEOUT},
            "overlap": "skip",
            "run_on_start": false
        }
    }
JSON
)
fi

cat <<JSON
{
    "listeners": {
        "*:80": { "pass": "routes/app" }
    },

    "routes": {
        "app": [
JSON

if [ "$IS_DRUPAL" = 1 ]; then
    drupal_routes
fi

cat <<JSON
            {
                "action": {
                    "share": "${DOCROOT}\$uri",
                    "fallback": { "pass": "applications/app/index" }
                }
            }
        ]
    },

    "applications": {
        "app": {
            "type": "php",
            "user": "www-data",
            "group": "www-data",
            "processes": {
                "max": ${PROCESSES_MAX},
                "spare": ${PROCESSES_SPARE},
                "idle_timeout": 60
            },
            "limits": {
                "timeout": ${TIMEOUT}
            },
            "options": {
                "admin": {
                    "memory_limit": "${FREEUNIT_MEMORY_LIMIT:-256M}",
                    "opcache.enable": "1",
                    "opcache.validate_timestamps": "1",
                    "opcache.revalidate_freq": "0"
                }
            },
            "targets": {
                "direct": {
                    "root": "${DOCROOT}/"
                },
                "index": {
                    "root": "${DOCROOT}/",
                    "script": "index.php"
                }
            }
        }
    }${SCHEDULES}
}
JSON
