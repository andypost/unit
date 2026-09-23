#!/usr/bin/env bash
#
# Local gate runner: G1 (hardened build, gcc+clang), G2 (build + a pytest
# slice), G3 (ASan+UBSan build + tests), G4 (short fuzz run, optional), G5
# (ast-grep baseline + disasm-diff, optional), G6 (@contract stub check).
#
# Usage:
#   tools/gates/run_gates.sh                      # G1 G2 G3 G5 G6 (G4 opt-in)
#   tools/gates/run_gates.sh -g 1,5,6              # only the named gates
#   tools/gates/run_gates.sh -g 2 -t test_foo.py   # G2 with a specific slice
#   tools/gates/run_gates.sh -g 4 --fuzz-seconds 30
#
# Exits non-zero if any selected gate fails. Each gate prints a
# "==> G<n> ..." header and a "G<n>: PASS|FAIL" line so a single run's
# output can be grepped for the verdict.
#
# Run from the repository root, or anywhere -- REPO_ROOT is derived from
# this script's own path.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
cd "$REPO_ROOT"

GATES="1,2,3,5,6"
PYTEST_SLICE="test_asgi_websockets.py test_java_websockets.py"
FUZZ_SECONDS=20
JOBS=2
OVERALL_FAILED=0
GATE_FAILED=0
RAN_ANY=0

usage() {
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        -g|--gates)       GATES="$2"; shift 2 ;;
        -t|--tests)       PYTEST_SLICE="$2"; shift 2 ;;
        --fuzz-seconds)   FUZZ_SECONDS="$2"; shift 2 ;;
        -j|--jobs)        JOBS="$2"; shift 2 ;;
        -h|--help)        usage; exit 0 ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
        ;;
    esac
done

want() {
    case ",$GATES," in
        *",$1,"*) return 0 ;;
        *)        return 1 ;;
    esac
}

pass() { echo "G$1: PASS"; }
fail() { echo "G$1: FAIL -- $2"; GATE_FAILED=1; OVERALL_FAILED=1; }

# --- G1: build under --hardening=strict with gcc and with clang ----------
gate_1() {
    GATE_FAILED=0
    echo "==> G1: --hardening=strict build (gcc, clang)"
    # ./configure's build tree (NXT_BUILD_DIR) always lands at ./build, and
    # the top-level Makefile is generated to "include build/Makefile", so
    # the two compilers are built one at a time in the same tree rather than
    # in parallel side trees.
    for cc in gcc clang; do
        if ! command -v "$cc" >/dev/null 2>&1; then
            echo "G1 ($cc): SKIP -- $cc not found"
            continue
        fi
        rm -rf build
        if ! (JAVA_TOOL_OPTIONS= CC="$cc" ./configure --zlib --openssl \
                --hardening=strict >"/tmp/g1-$cc-configure.log" 2>&1); then
            tail -n 40 "/tmp/g1-$cc-configure.log"
            fail 1 "$cc: configure failed, see /tmp/g1-$cc-configure.log"
            continue
        fi
        if ! (JAVA_TOOL_OPTIONS= make -j"$JOBS" >"/tmp/g1-$cc-build.log" 2>&1); then
            tail -n 60 "/tmp/g1-$cc-build.log"
            fail 1 "$cc: build failed, see /tmp/g1-$cc-build.log"
            continue
        fi
        echo "G1 ($cc): built OK"
    done
    [ "$GATE_FAILED" = 0 ] && pass 1
}

# --- G2: build + a named pytest slice -------------------------------------
gate_2() {
    echo "==> G2: build + pytest slice ($PYTEST_SLICE)"
    if ! (JAVA_TOOL_OPTIONS= make -j"$JOBS" >/tmp/g2-build.log 2>&1); then
        tail -n 60 /tmp/g2-build.log
        fail 2 "build failed, see /tmp/g2-build.log"
        return
    fi
    ( cd test && JAVA_TOOL_OPTIONS= /root/.local/bin/pytest $PYTEST_SLICE \
        >/tmp/g2-pytest.log 2>&1 )
    rc=$?
    tail -n 40 /tmp/g2-pytest.log
    if [ $rc -ne 0 ]; then
        fail 2 "pytest slice failed (exit $rc), see /tmp/g2-pytest.log"
        return
    fi
    pass 2
}

# --- G3: ASan+UBSan build + tests (mirrors .github/workflows/sanitize.yml) -
gate_3() {
    GATE_FAILED=0
    echo "==> G3: ASan+UBSan build + tests"
    rm -rf build
    JAVA_TOOL_OPTIONS= ASAN_OPTIONS="detect_leaks=0" ./configure \
        --debug --tests --openssl \
        --cc-opt="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
        --ld-opt="-fsanitize=address,undefined" \
        >/tmp/g3-configure.log 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        tail -n 40 /tmp/g3-configure.log
        fail 3 "configure failed, see /tmp/g3-configure.log"
        return
    fi
    if ! (JAVA_TOOL_OPTIONS= make -j"$JOBS" unitd >/tmp/g3-build.log 2>&1); then
        tail -n 60 /tmp/g3-build.log
        fail 3 "build failed, see /tmp/g3-build.log"
        return
    fi
    (
        cd test && JAVA_TOOL_OPTIONS= ASAN_OPTIONS="detect_leaks=0" \
            UBSAN_OPTIONS="print_stacktrace=1" \
            /root/.local/bin/pytest test_static.py >/tmp/g3-pytest.log 2>&1
    )
    rc=$?
    tail -n 40 /tmp/g3-pytest.log
    if [ $rc -ne 0 ]; then
        fail 3 "sanitized test run failed (exit $rc), see /tmp/g3-pytest.log"
        return
    fi
    pass 3
}

# --- G4: short fuzz run (opt-in; not in the default gate set) -------------
gate_4() {
    echo "==> G4: short fuzz run (${FUZZ_SECONDS}s)"
    if [ ! -x fuzzing/build-fuzz.sh ]; then
        fail 4 "fuzzing/build-fuzz.sh not found"
        return
    fi
    (
        JAVA_TOOL_OPTIONS= bash fuzzing/build-fuzz.sh
    ) >/tmp/g4-build.log 2>&1
    if [ $? -ne 0 ]; then
        tail -n 60 /tmp/g4-build.log
        fail 4 "fuzz build failed, see /tmp/g4-build.log"
        return
    fi
    fuzzer="build/fuzz_basic"
    if [ ! -x "$fuzzer" ]; then
        fail 4 "$fuzzer not built"
        return
    fi
    "$fuzzer" -max_total_time="$FUZZ_SECONDS" \
        fuzzing/fuzz_basic_seed_corpus >/tmp/g4-fuzz.log 2>&1
    rc=$?
    tail -n 40 /tmp/g4-fuzz.log
    if [ $rc -ne 0 ]; then
        fail 4 "fuzzer found a crash (exit $rc), see /tmp/g4-fuzz.log"
        return
    fi
    pass 4
}

# --- G5: ast-grep baseline gate (+ disasm-diff if a baseline exists) -----
gate_5() {
    GATE_FAILED=0
    echo "==> G5: ast-grep baseline"
    if ! command -v ast-grep >/dev/null 2>&1; then
        fail 5 "ast-grep not installed"
        return
    fi
    if ! python3 tools/ast-grep/check_baseline.py; then
        fail 5 "new ast-grep violations (see above)"
    else
        echo "G5 (ast-grep): PASS"
    fi

    if [ -x tools/perf/disasm-diff.sh ] && command -v objdump >/dev/null 2>&1 \
       && [ -d tools/perf/baseline ] && [ -n "$(ls -A tools/perf/baseline 2>/dev/null)" ]; then
        # disasm-diff.sh's own ./configure rewrites the top-level ./Makefile
        # to "include <its build dir>/Makefile" (it says so in its own
        # header), which would silently redirect every later `make` in this
        # script -- and any G1/G2/G3 gate re-run after this one in the same
        # tree -- away from the default ./build the test suite expects.
        # Save and restore it so disasm-diff is isolated from every other
        # gate regardless of run order.
        makefile_snapshot="$(mktemp)"
        cp -p Makefile "$makefile_snapshot" 2>/dev/null || true
        if tools/perf/disasm-diff.sh --cc gcc >/tmp/g5-disasm.log 2>&1; then
            echo "G5 (disasm-diff): PASS"
        else
            tail -n 40 /tmp/g5-disasm.log
            fail 5 "disasm-diff reported a change, see /tmp/g5-disasm.log"
        fi
        if [ -s "$makefile_snapshot" ]; then
            cp -p "$makefile_snapshot" Makefile
        fi
        rm -f "$makefile_snapshot"
    else
        echo "G5 (disasm-diff): SKIP -- no stored baseline or objdump missing"
    fi

    [ "$GATE_FAILED" = 0 ] && pass 5
}

# --- G6: @contract stub check ---------------------------------------------
gate_6() {
    echo "==> G6: @contract comment check"
    list="tools/gates/contract_functions.txt"
    if [ ! -f "$list" ]; then
        fail 6 "$list not found"
        return
    fi
    missing=0
    while IFS= read -r fn; do
        case "$fn" in
            ''|'#'*) continue ;;
        esac
        # Find the function's definition line(s) under src/, then look for
        # "@contract" within the 15 lines immediately above it.
        hit=0
        while IFS=: read -r file line _; do
            start=$((line - 15))
            [ "$start" -lt 1 ] && start=1
            if sed -n "${start},${line}p" "$file" | grep -q '@contract'; then
                hit=1
                break
            fi
        done < <(grep -rn -E "^[A-Za-z_][A-Za-z0-9_ \*]*\b${fn}\(" src --include='*.c' 2>/dev/null)
        if [ "$hit" -eq 0 ]; then
            echo "  missing @contract for: $fn"
            missing=1
        fi
    done < "$list"
    if [ "$missing" -eq 1 ]; then
        fail 6 "one or more listed functions have no @contract comment"
        return
    fi
    pass 6
}

for g in 1 2 3 4 5 6; do
    if want "$g"; then
        RAN_ANY=1
        "gate_$g"
        echo
    fi
done

if [ "$RAN_ANY" = 0 ]; then
    echo "no gates matched selection '$GATES'" >&2
    exit 2
fi

if [ "$OVERALL_FAILED" -ne 0 ]; then
    echo "one or more gates FAILED"
    exit 1
fi

echo "all selected gates PASSED"
exit 0
