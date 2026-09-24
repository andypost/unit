#!/usr/bin/env bash
#
# Compare DDEV's default nginx-fpm with the freeunit add-on on a fresh
# Drupal site. Run from an empty directory; needs ddev, hey, curl, python3.
# Drupal chores run through metrics-site.php, not drush.
#
#   metrics.sh <add-on dir> <out.json>
#
# Env: PROJECT_TYPE (drupal11), DRUPAL_CONSTRAINT (^11), DRUPAL_STABILITY,
#      PHP_VERSION (8.5),
#      DURATION (10s), CONCURRENCY (10), REPEAT (3),
#      FREEUNIT_SRC_TARBALL (optional: build FreeUnit from this source),
#      METRICS_DIR (raw outputs; default a temporary directory).
# Every measurement runs REPEAT times; report.py takes the median.

set -euo pipefail

ADDON=$(cd "$1" && pwd)
OUT=$2
PROJ=fu-bench
BASE="http://${PROJ}.ddev.site"
# Raw outputs go outside the project: composer create-project needs it empty.
RAW=${METRICS_DIR:-$(mktemp -d)}
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
# No drush: it does not install on Drupal 12 pre-releases yet.
cp "$ADDON/tests/metrics-site.php" .
site() { ddev exec php metrics-site.php "$@"; }
site install
NODE=$(site node | tail -n1 | tr -d "\r")
site version | tee "$RAW/versions.txt"
# Oracle: the pages exist for anonymous users before anything is measured.
for path in / "/node/$NODE" /core/misc/druplicon.png; do
    code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE$path")
    if [ "$code" != 200 ]; then
        echo "$path returned $code" >&2
        site log >&2 || true
        exit 1
    fi
done
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
    first=$(curl -s -o /dev/null -w '%{http_code} %{time_total}' "$BASE/node/$NODE")
    echo "restart_s=$(awk "BEGIN{printf \"%.1f\", $t1 - $t0}")" > "$RAW/$server.cold.txt"
    echo "first_request=$first" >> "$RAW/$server.cold.txt"
    cat "$RAW/$server.cold.txt" >&2
    case "$first" in 200\ *) ;; *) site log >&2 || true; exit 1 ;; esac

    curl -sI "$BASE/" | tr -d '\r' | grep -i '^server:' > "$RAW/$server.server.txt" || true
    bench "$server" anon_front / HIT
    bench "$server" anon_node "/node/$NODE" HIT
    bench "$server" static /core/misc/druplicon.png
    docker stats --no-stream --format '{{.MemUsage}}' "ddev-${PROJ}-web" > "$RAW/$server.mem.txt"
    site module-off page_cache
    bench "$server" uncached_front / none
    bench "$server" uncached_node "/node/$NODE" none
    site module-on page_cache
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
