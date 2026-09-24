#!/usr/bin/env bash
#
# Compare DDEV's default nginx-fpm with the freeunit add-on on a fresh
# Drupal site. Run from an empty directory; needs ddev, hey, curl, python3.
#
#   metrics.sh <add-on dir> <out.json>
#
# Env: PROJECT_TYPE (drupal11), DRUPAL_CONSTRAINT (^11), DRUPAL_STABILITY,
#      PHP_VERSION (8.5),
#      DURATION (10s), CONCURRENCY (10), REPEAT (3),
#      FREEUNIT_SRC_TARBALL (optional: build FreeUnit from this source).
# Every measurement runs REPEAT times; report.py takes the median.

set -euo pipefail

ADDON=$(cd "$1" && pwd)
OUT=$2
PROJ=fu-bench
BASE="http://${PROJ}.ddev.site"
RAW=$(pwd)/.metrics
export PROJECT_TYPE=${PROJECT_TYPE:-drupal11} DRUPAL_CONSTRAINT=${DRUPAL_CONSTRAINT:-^11}
export PHP_VERSION=${PHP_VERSION:-8.5}
export DURATION=${DURATION:-10s} CONCURRENCY=${CONCURRENCY:-10} REPEAT=${REPEAT:-3}
export DDEV_NONINTERACTIVE=true DDEV_NO_INSTRUMENTATION=true
mkdir -p "$RAW"

log() { echo "::group::$*" >&2; }
end() { echo "::endgroup::" >&2; }

# ---- Drupal site -----------------------------------------------------------
log "create Drupal project"
if ! ddev config --project-name="$PROJ" --project-type="$PROJECT_TYPE" \
        --docroot=web --php-version="$PHP_VERSION"; then
    echo "project type ${PROJECT_TYPE} unsupported by this DDEV, using drupal11" >&2
    ddev config --project-name="$PROJ" --project-type=drupal11 \
        --docroot=web --php-version="$PHP_VERSION"
fi
ddev start -y
# Drupal 12 has pre-releases only so far: DRUPAL_STABILITY=alpha.
ddev composer create-project ${DRUPAL_STABILITY:+--stability="$DRUPAL_STABILITY"} \
    "drupal/recommended-project:$DRUPAL_CONSTRAINT"
if [ -n "${DRUPAL_STABILITY:-}" ]; then
    ddev composer config minimum-stability "$DRUPAL_STABILITY"
    ddev composer config prefer-stable true
fi
ddev composer require -W drush/drush
ddev drush site:install standard -y --account-pass=admin --site-name=bench
ddev drush php:eval '\Drupal\node\Entity\Node::create(["type" => "article", "title" => "Bench", "body" => str_repeat("Lorem ipsum dolor sit amet. ", 200), "status" => 1])->save();'
ddev drush status --fields=drupal-version,php-version | tee "$RAW/versions.txt"
end

# ---- helpers ---------------------------------------------------------------
# bench <server> <case> <path> [expected X-Drupal-Cache: HIT, MISS, none]
bench() {
    local server=$1 case=$2 url="$BASE$3" want=${4:-}
    for _ in 1 2 3 4 5; do curl -s -o /dev/null "$url"; done
    if [ -n "$want" ]; then
        # Oracle: the case measures what it claims to measure ("none": the
        # page cache did not handle the request at all).
        got=$(curl -sI "$url" | tr -d '\r' | awk -F': ' 'tolower($1)=="x-drupal-cache"{print $2}')
        got=${got:-none}
        if [ "$got" != "$want" ]; then
            echo "$server/$case: X-Drupal-Cache is '${got}', expected '${want}'" >&2
            exit 1
        fi
    fi
    for i in $(seq 1 "$REPEAT"); do
        hey -z "$DURATION" -c "$CONCURRENCY" "$url" > "$RAW/$server.$case.$i.txt"
        grep -E 'Requests/sec|50% in|99% in' "$RAW/$server.$case.$i.txt" | tr -s ' \t' ' ' >&2
    done
}

measure() {
    local server=$1
    log "measure $server"
    # Cold start: `ddev restart` of an already-built image, then the first
    # request (empty opcache, empty worker pool).
    local t0 t1 first
    t0=$(date +%s.%N)
    ddev restart -y
    t1=$(date +%s.%N)
    first=$(curl -s -o /dev/null -w '%{http_code} %{time_total}' "$BASE/node/1")
    echo "restart_s=$(awk "BEGIN{printf \"%.1f\", $t1 - $t0}")" > "$RAW/$server.cold.txt"
    echo "first_request=$first" >> "$RAW/$server.cold.txt"
    cat "$RAW/$server.cold.txt" >&2

    ddev drush cr
    curl -sI "$BASE/" | tr -d '\r' | grep -i '^server:' > "$RAW/$server.server.txt" || true
    bench "$server" anon_front / HIT
    bench "$server" anon_node /node/1 HIT
    bench "$server" static /core/misc/druplicon.png
    docker stats --no-stream --format '{{.MemUsage}}' "ddev-${PROJ}-web" > "$RAW/$server.mem.txt"
    ddev drush pm:uninstall -y page_cache
    bench "$server" uncached_front / none
    bench "$server" uncached_node /node/1 none
    ddev drush pm:install -y page_cache
    end
}

# ---- a) nginx-fpm, b) freeunit ----------------------------------------------
measure nginx-fpm

log "install freeunit add-on"
ddev add-on get "$ADDON"
if [ -n "${FREEUNIT_SRC_TARBALL:-}" ]; then
    cp "$FREEUNIT_SRC_TARBALL" .ddev/web-build/freeunit-src.tar.gz
fi
ddev restart -y   # builds the image; not part of the cold start
end
measure freeunit
if ! grep -qi 'unit' "$RAW/freeunit.server.txt"; then
    echo "freeunit run was not served by FreeUnit" >&2
    exit 1
fi

python3 "$ADDON/tests/metrics-report.py" "$RAW" "$OUT"
ddev delete -Oy "$PROJ"
