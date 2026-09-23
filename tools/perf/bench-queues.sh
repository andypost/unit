#!/bin/sh
#
# Copyright (C) NGINX, Inc.
#
# Run the queue_bench microbenchmark (O1) for every mode, optionally under
# `perf stat`, and print an environment header so the numbers can be
# compared across runs/machines.
#
# Usage: tools/perf/bench-queues.sh [build-dir] [-n ops]
#
# `perf` is used automatically when it is on PATH and its hardware
# counters actually work in this environment (many containers/VMs expose
# the `perf` binary but have no access to the real PMU, in which case
# `perf stat` reports every hardware event as "<not supported>"). When
# that is the case, or when perf is missing entirely, this script says so
# and falls back to the benchmark's own clock_gettime-based timings.

set -eu

BUILD_DIR="${1:-build}"
shift 2>/dev/null || true

BENCH="$BUILD_DIR/queue_bench"

if [ ! -x "$BENCH" ]; then
    echo "error: $BENCH not found; build it first with:" >&2
    echo "  ./configure --tests && make -j\$(nproc) tests" >&2
    exit 1
fi

echo "=== environment ==="
echo "date:      $(date -u +%FT%TZ)"
echo "host:      $(uname -a)"
echo "cpus:      $(nproc)"
echo "loadavg:   $(cat /proc/loadavg 2>/dev/null || echo n/a)"
echo

PERF=""
PERF_EVENTS="instructions,cycles,cache-misses,branch-misses"

if command -v perf >/dev/null 2>&1; then
    # `perf` on PATH is often a distro wrapper script that execs a
    # kernel-version-specific binary (see /usr/bin/perf on Debian/Ubuntu);
    # it can be present yet unusable (wrong kernel package installed,
    # exits non-zero with a warning), and even when it does run, the
    # container/VM may have no access to the real PMU, in which case
    # every hardware event comes back as "<not supported>" although the
    # command exits 0. Probe for both failure modes before trusting it.
    probe=$(perf stat -e instructions -- /bin/true 2>&1) && probe_rc=0 \
        || probe_rc=$?

    if [ $probe_rc -ne 0 ]; then
        echo "note: 'perf' is on PATH but failed to run in this container" \
             "(exit $probe_rc):"
        echo "$probe" | sed 's/^/  /'
        echo "Falling back to clock_gettime timings reported by" \
             "queue_bench itself."
    else
        case "$probe" in
            *"<not supported>"*|*"not counted"*)
                echo "note: perf runs, but hardware counters (instructions/" \
                     "cycles/cache-misses/branch-misses) are not available" \
                     "in this environment -- no PMU access:"
                echo "$probe" | sed 's/^/  /'
                echo "Falling back to clock_gettime timings reported by" \
                     "queue_bench itself."
                ;;
            *)
                PERF="perf stat -e $PERF_EVENTS --"
                ;;
        esac
    fi
else
    echo "note: 'perf' not found on PATH; falling back to clock_gettime" \
         "timings reported by queue_bench itself."
fi

echo

run() {
    echo "--- $* ---"
    if [ -n "$PERF" ]; then
        $PERF "$BENCH" "$@" 2>&1
    else
        "$BENCH" "$@"
    fi
    echo
}

run nncq "$@"
run app_nncq "$@"
run port_queue "$@"
run app_queue "$@"
run port_queue_mt --producers 1 --consumers 1 "$@"
run port_queue_mt --producers 2 --consumers 2 "$@"
