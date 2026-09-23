#!/bin/sh
#
# Copyright (C) NGINX, Inc.
#
# O3: build unitd with a given compiler, extract normalized disassembly
# of the hot functions listed in tools/perf/hot-functions.txt, and diff
# it against a stored baseline (or write a new baseline).
#
# The disassembler is a variable (default: objdump) so this script stays
# usable with llvm-objdump or a cross toolchain's objdump; only the
# address/offset normalization below assumes GNU-objdump-compatible
# `-d --no-show-raw-insn` output (llvm-objdump accepts the same flags and
# produces a compatible enough format for this purpose).
#
# Usage:
#   tools/perf/disasm-diff.sh --cc gcc [--objdump objdump] [--update]
#   tools/perf/disasm-diff.sh --cc clang --update
#
#   --cc <compiler>        compiler to build unitd with (required)
#   --objdump <tool>       disassembler to use (default: objdump, or
#                           $OBJDUMP if set)
#   --hot-functions <file> list of functions to track
#                           (default: tools/perf/hot-functions.txt)
#   --update               write/replace the stored baseline instead of
#                           diffing against it
#   --build-dir <dir>      scratch build directory
#                           (default: build-disasm-<cc>)
#
# Baseline layout: tools/perf/baseline/<cc>-<version>-<libc>/<func>.s
# e.g. tools/perf/baseline/gcc-13.3.0-glibc-2.39/nxt_port_read_msg_process.s

set -eu

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT_DIR"

CC=""
OBJDUMP="${OBJDUMP:-objdump}"
HOT_FUNCTIONS="tools/perf/hot-functions.txt"
UPDATE=0
BUILD_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --cc)             CC="$2"; shift 2 ;;
        --cc=*)           CC="${1#--cc=}"; shift ;;
        --objdump)        OBJDUMP="$2"; shift 2 ;;
        --objdump=*)      OBJDUMP="${1#--objdump=}"; shift ;;
        --hot-functions)  HOT_FUNCTIONS="$2"; shift 2 ;;
        --hot-functions=*) HOT_FUNCTIONS="${1#--hot-functions=}"; shift ;;
        --build-dir)      BUILD_DIR="$2"; shift 2 ;;
        --build-dir=*)    BUILD_DIR="${1#--build-dir=}"; shift ;;
        --update)         UPDATE=1; shift ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [ -z "$CC" ]; then
    echo "error: --cc <compiler> is required (e.g. --cc gcc, --cc clang)" >&2
    exit 1
fi

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "error: compiler '$CC' not found on PATH" >&2
    exit 1
fi

if ! command -v "$OBJDUMP" >/dev/null 2>&1; then
    echo "error: disassembler '$OBJDUMP' not found on PATH" >&2
    exit 1
fi

if [ ! -f "$HOT_FUNCTIONS" ]; then
    echo "error: hot-functions list not found: $HOT_FUNCTIONS" >&2
    exit 1
fi

cc_name=$(basename "$CC")
cc_version=$("$CC" --dumpversion 2>/dev/null || "$CC" --version | head -1 | \
             grep -oE '[0-9]+(\.[0-9]+)+' | head -1)
cc_version="${cc_version:-unknown}"

libc_desc="glibc-unknown"
if command -v ldd >/dev/null 2>&1; then
    libc_ver=$(ldd --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+$')
    if [ -n "$libc_ver" ]; then
        libc_desc="glibc-$libc_ver"
    fi
fi

toolchain_id="${cc_name}-${cc_version}-${libc_desc}"
BUILD_DIR="${BUILD_DIR:-build-disasm-$cc_name}"
BASELINE_DIR="tools/perf/baseline/$toolchain_id"

echo "=== toolchain ==="
echo "cc:        $CC ($cc_name $cc_version)"
echo "objdump:   $OBJDUMP ($($OBJDUMP --version | head -1))"
echo "libc:      $libc_desc"
echo "build dir: $BUILD_DIR"
echo "baseline:  $BASELINE_DIR"
echo

echo "=== building unitd ==="
# configure only takes NXT_BUILD_DIR as an env var (no --build-dir flag);
# it (re)writes the top-level ./Makefile to 'include $NXT_BUILD_DIR/Makefile',
# so the plain `make` right after picks up this build dir. Re-run
# `./configure --tests` (default build dir) afterwards if you need the
# normal build/ tree to be the active one again.
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

NXT_BUILD_DIR="$BUILD_DIR" ./configure --cc="$CC" --tests \
    >"$BUILD_DIR.configure.log" 2>&1 || {
        echo "configure failed, see $BUILD_DIR.configure.log" >&2
        exit 1
    }
make -j2 >"$BUILD_DIR.build.log" 2>&1 || {
    echo "build failed, see $BUILD_DIR.build.log" >&2
    exit 1
}
make -j2 tests >>"$BUILD_DIR.build.log" 2>&1 || {
    echo "'tests' build failed, see $BUILD_DIR.build.log" >&2
    exit 1
}

UNITD="$BUILD_DIR/sbin/unitd"
if [ ! -f "$UNITD" ]; then
    echo "error: $UNITD not found after build" >&2
    exit 1
fi

# nxt_port_queue.h/nxt_app_queue.h are used from both unitd (router/main)
# and libunit (the app-facing library, e.g. src/nxt_unit.c); their
# symbols only show up in unit_app_test, which links libunit statically.
UNIT_APP_TEST="$BUILD_DIR/unit_app_test"
BINARIES="$UNITD"
[ -f "$UNIT_APP_TEST" ] && BINARIES="$BINARIES $UNIT_APP_TEST"

WORK=$(mktemp -d)

# tools/perf/harness/mca_harness.c wraps the header-only `static inline`
# hot functions (nxt_port_queue_*, nxt_app_queue_*, nxt_nncq_*,
# nxt_app_nncq_*, nxt_port_mmap_get_free_chunk) in noinline wrappers, so
# each one gets its own stable ELF symbol instead of only showing up
# folded into whatever caller happens to survive at -O1/-O2 (the
# hot-functions.txt entries above). It is compiled here with the exact
# CC/CFLAGS/-I the real build just used (pulled from $BUILD_DIR/Makefile,
# so --hardening and any other configure flag stay in sync automatically)
# and only ever disassembled, never linked into unitd or libunit.
echo "=== building harness ==="
HARNESS_SRC="tools/perf/harness/mca_harness.c"
HARNESS_OBJ="$WORK/mca_harness.o"
if [ -f "$HARNESS_SRC" ]; then
    mk_cflags=$(sed -n 's/^CFLAGS = \(.*\) \$(EXTRA_CFLAGS)$/\1/p' \
                "$BUILD_DIR/Makefile" | head -1)
    # shellcheck disable=SC2086
    "$CC" -c $mk_cflags -I src -I "$BUILD_DIR/include" \
        -o "$HARNESS_OBJ" "$HARNESS_SRC" \
        >"$BUILD_DIR.harness.log" 2>&1 || {
            echo "harness build failed, see $BUILD_DIR.harness.log" >&2
            exit 1
        }
    BINARIES="$BINARIES $HARNESS_OBJ"
else
    echo "note: $HARNESS_SRC not found, skipping harness symbols" >&2
fi

# Normalize a disassembly block so unrelated diffs (absolute addresses,
# per-build symbol offsets) don't drown out real instruction-level
# changes:
#   - drop the leading "<addr>:" column
#   - collapse jump/call targets to "<symbol+off>" without the address
#   - blank out the literal displacement on `N(%rip)` operands: two
#     builds from the very same source and compiler can still lay out
#     unrelated .rodata (format strings, etc.) differently -- e.g. under
#     -j2 parallel compilation and/or a linker that doesn't guarantee a
#     stable string-pool order -- which shifts these displacements
#     without the referenced symbol or the code around it changing at
#     all. What we care about is *which* symbol is referenced, not the
#     exact byte distance to it, so the symbol name in the trailing
#     "# <sym+off>" comment is kept (also collapsed to just "<sym>")
#     while the raw hex offset before "(%rip)" is masked.
#   - collapse the synthetic ".rodata" anchor comment
#     ("# <_IO_stdin_used+0x...>", used for objects with no real name,
#     e.g. embedded log format strings) to a fixed placeholder for the
#     same reason.
#   - drop trailing padding whitespace
normalize() {
    sed -E \
        -e 's/^[[:space:]]*[0-9a-f]+:[[:space:]]*//' \
        -e 's/^([a-z0-9]+) <([^>]+)>:/\2:/' \
        -e 's/-?0x[0-9a-f]+\(%rip\)/OFF(%rip)/' \
        -e 's/<_IO_stdin_used\+0x[0-9a-f]+>/<rodata>/' \
        -e 's/\b[0-9a-f]{4,}[[:space:]]*<([^>]+)>/<\1>/' \
        -e 's/[[:space:]]+$//'
}

# Full disassembly is cached per binary rather than re-invoked per
# function: some binutils versions silently return nothing for
# `--disassemble=<name>` against a PIE binary's local (static-function)
# symbols, so we always dump the whole .text and slice it with awk
# instead of relying on that filter.
bin_index=0
for bin in $BINARIES; do
    bin_index=$((bin_index + 1))
    dump="$WORK/dump.$bin_index"
    "$OBJDUMP" -d --no-show-raw-insn "$bin" > "$dump" 2>/dev/null
    eval "DUMP_$bin_index=\$dump"
done

extract_function() {
    func="$1"
    i=1

    while [ "$i" -le "$bin_index" ]; do
        eval "dump=\$DUMP_$i"

        # objdump -d --no-show-raw-insn prints one section per function as
        #   0000000000401234 <func_name>:
        #   ...instructions...
        #   (blank line before the next symbol)
        #
        # NB: this must not be named the same as the caller's `out`
        # (the destination file path) -- POSIX sh functions share the
        # caller's variable namespace, so reusing the name here would
        # silently clobber it.
        text=$(awk -v f="$func" '
                $0 ~ "<" f ">:" { grab=1 }
                grab { print }
                grab && /^$/ && NR>1 { exit }
            ' "$dump")

        if [ -n "$text" ]; then
            echo "$text" | normalize
            return 0
        fi

        i=$((i + 1))
    done
}

status=0
missing=""
diffs=""

mkdir -p "$BASELINE_DIR"

while IFS= read -r func || [ -n "$func" ]; do
    case "$func" in
        ''|'#'*) continue ;;
    esac

    out="$WORK/$func.s"
    extract_function "$func" > "$out"

    if [ ! -s "$out" ]; then
        missing="$missing $func"
        continue
    fi

    baseline_file="$BASELINE_DIR/$func.s"

    if [ "$UPDATE" -eq 1 ]; then
        cp "$out" "$baseline_file"
        continue
    fi

    if [ ! -f "$baseline_file" ]; then
        echo "NEW (no baseline yet): $func"
        status=1
        continue
    fi

    if ! diff -u "$baseline_file" "$out" > "$WORK/$func.diff"; then
        echo "=== $func differs from baseline ($BASELINE_DIR) ==="
        cat "$WORK/$func.diff"
        echo
        diffs="$diffs $func"
        status=1
    fi
done < "$HOT_FUNCTIONS"

if [ "$UPDATE" -eq 1 ]; then
    echo "Baseline written to $BASELINE_DIR"
fi

if [ -n "$missing" ]; then
    echo "warning: no disassembly found (inlined away, renamed, or" \
         "optimized out) for:$missing" >&2
fi

if [ -n "$diffs" ]; then
    echo "functions with disassembly changes:$diffs"
fi

exit $status
