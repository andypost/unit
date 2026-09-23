#!/bin/sh
#
# Copyright (C) NGINX, Inc.
#
# Struct-layout regression gate: dump `pahole -C <struct>` (size,
# field offsets, holes, cacheline boundaries) for the shared-memory
# (SHM) structs and the key process-local structs listed below, and
# diff the result against a stored baseline (or write a new one).
#
# A layout change to an SHM struct is a hard failure -- these structs
# are read/written across process boundaries by binaries that may
# have been built at different times, so a silent layout change is a
# live ABI break. Pass --allow-abi-bump when the change is an
# intentional, reviewed ABI bump (coordinate it across the
# router/app boundary); it downgrades the SHM diff to a warning, same
# as a process-local diff always is.
#
# Usage:
#   tools/perf/layout-check.sh --cc clang [--update]
#   tools/perf/layout-check.sh --cc musl-gcc --update [--configure-opt=--no-regex]
#   tools/perf/layout-check.sh --cc clang --allow-abi-bump
#
#   --cc <compiler>         compiler to build unitd/libunit with (required)
#   --pahole <tool>         pahole binary to use (default: pahole)
#   --update                write/replace the stored baseline instead of
#                            diffing against it
#   --allow-abi-bump        a diff in an SHM struct's layout is expected
#                            and reviewed; downgrade it to a warning
#                            instead of failing
#   --configure-opt <opt>   extra option passed through to ./configure
#                            (repeatable; e.g. --configure-opt=--no-regex
#                            for a musl-gcc build with no PCRE)
#   --build-dir <dir>       scratch build directory
#                            (default: build-layout-<cc>)
#
# Baseline layout: tools/perf/layout-baseline/<cc>-<libc>.txt
# e.g. tools/perf/layout-baseline/clang-18.1.3-glibc-2.39.txt
#
# SHM structs (hard failure without --allow-abi-bump):
#   nxt_nncq_t, nxt_app_nncq_t, nxt_port_queue_t, nxt_app_queue_t,
#   nxt_port_queue_item_t, nxt_app_queue_item_t, nxt_port_mmap_header_t,
#   nxt_unit_request_t
#
# Process-local structs (warning only, never a hard failure):
#   struct nxt_port_s, nxt_unit_ctx_impl_t

set -eu

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT_DIR"

CC=""
PAHOLE="${PAHOLE:-pahole}"
UPDATE=0
ALLOW_ABI_BUMP=0
BUILD_DIR=""
CONFIGURE_OPTS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --cc)              CC="$2"; shift 2 ;;
        --cc=*)            CC="${1#--cc=}"; shift ;;
        --pahole)          PAHOLE="$2"; shift 2 ;;
        --pahole=*)        PAHOLE="${1#--pahole=}"; shift ;;
        --build-dir)       BUILD_DIR="$2"; shift 2 ;;
        --build-dir=*)     BUILD_DIR="${1#--build-dir=}"; shift ;;
        --configure-opt)   CONFIGURE_OPTS="$CONFIGURE_OPTS $2"; shift 2 ;;
        --configure-opt=*) CONFIGURE_OPTS="$CONFIGURE_OPTS ${1#--configure-opt=}"; shift ;;
        --update)          UPDATE=1; shift ;;
        --allow-abi-bump)  ALLOW_ABI_BUMP=1; shift ;;
        -h|--help)
            sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [ -z "$CC" ]; then
    echo "error: --cc <compiler> is required (e.g. --cc clang, --cc musl-gcc)" >&2
    exit 1
fi

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "error: compiler '$CC' not found on PATH" >&2
    exit 1
fi

if ! command -v "$PAHOLE" >/dev/null 2>&1; then
    echo "error: '$PAHOLE' not found on PATH (apt install pahole?)" >&2
    exit 1
fi

cc_name=$(basename "$CC")
cc_version=$("$CC" --dumpversion 2>/dev/null || "$CC" --version | head -1 | \
             grep -oE '[0-9]+(\.[0-9]+)+' | head -1)
cc_version="${cc_version:-unknown}"

# musl-gcc reports glibc's own gcc --dumpversion (it's a wrapper around
# the system gcc), and links against musl's libc.so, not glibc's, so
# `ldd --version` (glibc's) is the wrong libc identity for it -- name
# it explicitly instead of probing.
case "$cc_name" in
    *musl*)
        libc_desc="musl-$(ldconfig -p 2>/dev/null | grep -oE 'libc\.musl-[a-z0-9_]+\.so\.1' | head -1 | grep -oE '[0-9]+' | head -1)"
        [ "$libc_desc" = "musl-" ] && libc_desc="musl"
        ;;
    *)
        libc_desc="glibc-unknown"
        if command -v ldd >/dev/null 2>&1; then
            libc_ver=$(ldd --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+$')
            [ -n "$libc_ver" ] && libc_desc="glibc-$libc_ver"
        fi
        ;;
esac

toolchain_id="${cc_name}-${cc_version}-${libc_desc}"
BUILD_DIR="${BUILD_DIR:-build-layout-$cc_name}"
BASELINE_DIR="tools/perf/layout-baseline"
BASELINE_FILE="$BASELINE_DIR/$toolchain_id.txt"

echo "=== toolchain ==="
echo "cc:        $CC ($cc_name $cc_version)"
echo "pahole:    $PAHOLE ($($PAHOLE --version 2>&1 | head -1))"
echo "libc:      $libc_desc"
echo "build dir: $BUILD_DIR"
echo "baseline:  $BASELINE_FILE"
echo

echo "=== building unitd + libunit ==="
# shellcheck disable=SC2086
# ./configure rewrites the top-level ./Makefile to point at $BUILD_DIR;
# put the caller's Makefile back on exit so a later plain `make` keeps
# building the tree it built before this script ran.
MAKEFILE_SAVE=
if [ -f Makefile ]; then
    MAKEFILE_SAVE=$(mktemp)
    cp -p Makefile "$MAKEFILE_SAVE"
fi
WORK=

cleanup() {
    if [ -n "$MAKEFILE_SAVE" ]; then
        mv -f "$MAKEFILE_SAVE" Makefile
    fi
    if [ -n "$WORK" ]; then
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

NXT_BUILD_DIR="$BUILD_DIR" ./configure --cc="$CC" --tests $CONFIGURE_OPTS \
    >"$BUILD_DIR.configure.log" 2>&1 || {
        echo "configure failed, see $BUILD_DIR.configure.log" >&2
        exit 1
    }
make -j2 >"$BUILD_DIR.build.log" 2>&1 || {
    echo "build failed, see $BUILD_DIR.build.log" >&2
    exit 1
}
# Only libunit.a's nxt_unit.o is needed (for nxt_unit_ctx_impl_t, which
# lives in the app-facing library, not the daemon); this also drags in
# the rest of the `tests` target, which is harmless.
make -j2 tests >>"$BUILD_DIR.build.log" 2>&1 || {
    echo "'tests' build failed, see $BUILD_DIR.build.log" >&2
    exit 1
}

UNITD="$BUILD_DIR/sbin/unitd"
UNIT_O="$BUILD_DIR/src/nxt_unit.o"

[ -f "$UNITD" ]  || { echo "error: $UNITD not found after build" >&2; exit 1; }
[ -f "$UNIT_O" ] || { echo "error: $UNIT_O not found after build" >&2; exit 1; }

# name:binary:kind  -- kind is "shm" (hard-fail gate) or "local" (warn only)
STRUCTS="
nxt_nncq_t:$UNITD:shm
nxt_app_nncq_t:$UNITD:shm
nxt_port_queue_t:$UNITD:shm
nxt_app_queue_t:$UNITD:shm
nxt_port_queue_item_t:$UNITD:shm
nxt_app_queue_item_t:$UNITD:shm
nxt_port_mmap_header_s:$UNITD:shm
nxt_unit_request_s:$UNITD:shm
nxt_port_s:$UNITD:local
nxt_unit_ctx_impl_s:$UNIT_O:local
"

WORK=$(mktemp -d)

dump_all() {
    out="$1"
    : > "$out"
    for entry in $STRUCTS; do
        [ -z "$entry" ] && continue
        name=${entry%%:*}
        rest=${entry#*:}
        binary=${rest%%:*}
        kind=${rest#*:}

        echo "### $name ($kind)" >> "$out"
        if ! "$PAHOLE" -C "$name" "$binary" 2>>"$out" >> "$out.tmp"; then
            echo "(pahole found no output for $name in $binary)" >> "$out"
        else
            cat "$out.tmp" >> "$out"
        fi
        rm -f "$out.tmp"
        echo >> "$out"
    done
}

dump="$WORK/layout.txt"
dump_all "$dump"

mkdir -p "$BASELINE_DIR"

if [ "$UPDATE" -eq 1 ]; then
    cp "$dump" "$BASELINE_FILE"
    echo "Baseline written to $BASELINE_FILE"
    exit 0
fi

if [ ! -f "$BASELINE_FILE" ]; then
    echo "NEW (no baseline yet): $BASELINE_FILE"
    echo "run with --update to create it"
    exit 1
fi

if diff -u "$BASELINE_FILE" "$dump" > "$WORK/diff.txt"; then
    echo "no layout changes ($toolchain_id)"
    exit 0
fi

echo "=== layout differs from baseline ($BASELINE_FILE) ==="
cat "$WORK/diff.txt"
echo

# Which struct sections actually changed? Walk the diff's hunk headers
# for "### <name> (<kind>)" lines touched by a hunk, both removed (-)
# and added (+) sides.
changed_shm=""
changed_local=""
for entry in $STRUCTS; do
    [ -z "$entry" ] && continue
    name=${entry%%:*}
    rest=${entry#*:}
    kind=${rest#*:}

    # Extract just this struct's section from both files and compare;
    # cheaper and less fragile than parsing unified-diff hunk ranges.
    old_section="$WORK/old.$name.txt"
    new_section="$WORK/new.$name.txt"
    awk -v hdr="### $name (" '
        index($0, hdr) == 1 { grab=1; print; next }
        grab && index($0, "### ") == 1 { exit }
        grab { print }
    ' "$BASELINE_FILE" > "$old_section"
    awk -v hdr="### $name (" '
        index($0, hdr) == 1 { grab=1; print; next }
        grab && index($0, "### ") == 1 { exit }
        grab { print }
    ' "$dump" > "$new_section"

    if ! diff -q "$old_section" "$new_section" >/dev/null 2>&1; then
        if [ "$kind" = "shm" ]; then
            changed_shm="$changed_shm $name"
        else
            changed_local="$changed_local $name"
        fi
    fi
done

status=0

if [ -n "$changed_local" ]; then
    echo "warning: process-local struct layout changed (never crosses a" \
         "process boundary by value, so this is not an ABI break on its" \
         "own):$changed_local"
fi

if [ -n "$changed_shm" ]; then
    if [ "$ALLOW_ABI_BUMP" -eq 1 ]; then
        echo "warning: SHM struct layout changed, but --allow-abi-bump was" \
             "passed -- treating as an acknowledged, reviewed ABI bump:" \
             "$changed_shm"
    else
        echo "FAIL: SHM struct layout changed without --allow-abi-bump:" \
             "$changed_shm"
        echo "This struct is shared between processes/binaries built at" \
             "different times; a silent layout change is a live ABI break."
        echo "If this is an intentional, reviewed ABI bump: re-run with" \
             "--update --allow-abi-bump (and coordinate the bump across" \
             "the router/app boundary before merging)."
        status=1
    fi
fi

exit $status
