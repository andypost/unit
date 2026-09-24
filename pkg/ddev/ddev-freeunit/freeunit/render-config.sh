#!/usr/bin/env bash
#ddev-generated
#
# Print the FreeUnit configuration for this DDEV project as JSON. Reads only
# DDEV_DOCROOT and DDEV_PROJECT_TYPE; run by start-freeunit.sh on every
# start. To customise routing, remove the #ddev-generated line above and
# edit; `ddev freeunit restart` applies the result.
#
# Drupal-family projects get the deny rules of pkg/docker/drupal/unit-drupal.json
# from the FreeUnit repository; every other type gets DDEV's generic rules
# (dotfiles denied, any *.php runs, the rest is served from the docroot with
# index.php as the front controller).

set -euo pipefail

DOCROOT="/var/www/html/${DDEV_DOCROOT:-}"
DOCROOT="${DOCROOT%/}"

case "${DDEV_PROJECT_TYPE:-php}" in
    drupal*|backdrop)
        ROUTES=$(cat <<'JSON'
        {
            "match": { "uri": ["*/.ht*", "/sites/*/files/*.php", "/sites/*/private/*"] },
            "action": { "return": 403 }
        },
        {
            "match": { "uri": [
                "/vendor/*", "!*/.well-known/*", "*/.*",
                "*.engine", "*.inc", "*.install", "*.make", "*.module", "*.po",
                "*.profile", "*.sh", "*.theme", "*.twig", "*.xtmpl", "*.yml", "*.sql",
                "*/composer.json", "*/composer.lock", "*/web.config",
                "*.bak", "*.orig", "*.save", "*.swo", "*.swp", "*~"
            ] },
            "action": { "return": 404 }
        },
        {
            "match": { "uri": [
                "/core/install.php", "/core/install.php/*", "/core/rebuild.php",
                "/update.php", "/update.php/*", "/core/modules/statistics/statistics.php"
            ] },
            "action": { "pass": "applications/app/direct" }
        },
        {
            "match": { "uri": ["!/index.php", "!/index.php/*", "*.php", "*.php/*"] },
            "action": { "return": 404 }
        },
JSON
        )
        ;;
    *)
        ROUTES=$(cat <<'JSON'
        {
            "match": { "uri": ["!*/.well-known/*", "*/.*"] },
            "action": { "return": 404 }
        },
        {
            "match": { "uri": ["*.php", "*.php/*"] },
            "action": { "pass": "applications/app/direct" }
        },
JSON
        )
        ;;
esac

cat <<JSON
{
    "listeners": {
        "*:80": {
            "pass": "routes",
            "forwarded": {
                "protocol": "X-Forwarded-Proto",
                "client_ip": "X-Forwarded-For",
                "source": ["127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"]
            }
        }
    },

    "routes": [
${ROUTES}
        {
            "action": {
                "share": "${DOCROOT}\$uri",
                "fallback": { "pass": "applications/app/index" }
            }
        }
    ],

    "applications": {
        "app": {
            "type": "php",
            "processes": { "max": 4, "spare": 1, "idle_timeout": 60 },
            "limits": { "timeout": 3600 },
            "targets": {
                "direct": { "root": "${DOCROOT}/" },
                "index": { "root": "${DOCROOT}/", "script": "index.php" }
            }
        }
    }
}
JSON
