#!/bin/bash
# bench-engines.sh — A/B benchmark harness comparing two unitd builds' event engines.
#
# WHAT IT DOES
#   Runs an identical request-hot-path matrix against two unitd build dirs
#   (e.g. an epoll-default build vs an io_uring-default build) and reports RPS,
#   p50/p99 latency, error counts and router CPU microseconds-per-request for
#   each. It is BUILD-AGNOSTIC: it only needs <builddir>/sbin/unitd (and, for the
#   PHP scenario, <builddir>/lib/unit/modules/php.so). It does not care how the
#   builds were configured, so pointing both slots at the same epoll build turns
#   this into a null A/A drift test.
#
# USAGE
#   bench-engines.sh <labelA> <builddirA> <labelB> <builddirB> [rounds]
#     rounds default 4. Builds are interleaved within every round, and the
#     order alternates per round (A/B, B/A, A/B, ...) so monotonic warmup or
#     thermal drift cannot stay correlated with one build label. Keep rounds
#     EVEN: with an odd count one build starts first more often than the
#     other, so a repeatable position effect would not cancel out of the means.
#
#   ENV OVERRIDES
#     RUNBASE=/path      base dir for per-run scratch (default under scratch/tmp)
#     RESULTS=file       machine-parsable results file (default ./bench-engines-results.txt)
#     PHPAPP=dir         PHP app root for the php scenario; must already contain
#                        index.php (validated up front; the harness never writes
#                        into it). Auto-created under RUNBASE only when unset.
#     ALLOW_SAME=1       permit both builds to report the SAME engine (A/A mode)
#     SKIP_AB=1          skip the secondary ApacheBench pass
#     MAX_FAILED=N       max failed requests a single measurement may record
#                        before it fails the round/run (default 0)
#     N_HIGH, N_LOW, N_CLOSE, N_PROXY, N_PHP   request counts (reproducibility knob)
#     OHA_TIMEOUT=dur    per-request oha timeout (default 30s; e.g. 5s, 500ms)
#     CURL_MAXTIME=secs  control-socket PUT transfer timeout (default 30)
#     SERVER_CORES=list  taskset CPU list for the server tree (default 0-3)
#     LOAD_CORES=list    taskset CPU list for the load generators (default 4-7)
#
#   FLAGS
#     --allow-same       same as ALLOW_SAME=1
#     --skip-ab          same as SKIP_AB=1
#
# ENGINE ASSERTION
#   After each build starts, the harness greps the unitd log for
#     using "<name>" event engine
#   and records the engine per build. If the two builds report the SAME engine
#   the whole run ABORTS unless --allow-same/ALLOW_SAME=1 is set. If the log line
#   is absent (older builds that predate it) the engine is recorded as "unknown"
#   with a warning, and the same-engine check is skipped (can't prove them equal).
#
# SCENARIOS (per round, per build)
#   ret200_ka_hi   return-200 keepalive, HIGH concurrency (-c 256)
#   ret200_ka_lo   return-200 keepalive, LOW  concurrency (-c 8)   <- herd detector
#   ret200_close   return-200, Connection: close                  (-c 16)
#   proxy_ka       proxy->self keepalive                          (-c 64)
#   php_ka         PHP hello app (SKIP unless php module + app available)
#
# OUTPUT
#   Appends `<label> <scenario> <tool> rps=.. p50ms=.. p99ms=.. failed=.. \
#   cpu_us_per_req=.. engine=..` lines to RESULTS, saves raw tool output per
#   measurement under the run dir, and prints a markdown summary table
#   (mean +/- stdev per scenario per build + delta% B vs A) at the end.
#
# NOTE: functional harness only. Real measurement runs are done serialized by the
# coordinator; the numbers from a shared/loaded box are not measurement-grade.

set -u

# Force C locale: printf %f and float parsing must use '.' decimals, not the
# locale's ',' (otherwise result lines and the summarizer break).
export LC_ALL=C

# ---- args -------------------------------------------------------------------
ALLOW_SAME=${ALLOW_SAME:-0}
SKIP_AB=${SKIP_AB:-0}
POS=()
for a in "$@"; do
    case "$a" in
        --allow-same) ALLOW_SAME=1 ;;
        --skip-ab)    SKIP_AB=1 ;;
        --*)          echo "unknown flag: $a" >&2; exit 2 ;;
        *)            POS+=("$a") ;;
    esac
done

if [ "${#POS[@]}" -lt 4 ]; then
    echo "usage: bench-engines.sh <labelA> <builddirA> <labelB> <builddirB> [rounds]" >&2
    exit 2
fi

LABEL_A=${POS[0]}; BUILD_A=${POS[1]}
LABEL_B=${POS[2]}; BUILD_B=${POS[3]}
ROUNDS=${POS[4]:-4}

# Labels become whitespace-delimited result-row fields; whitespace or glob chars
# silently break the summarizer (rows skipped, run still exits 0). Reject early.
for l in "$LABEL_A" "$LABEL_B"; do
    case "$l" in
        *[!A-Za-z0-9._-]*|'') echo "invalid label '$l' (allowed: A-Za-z0-9._-)" >&2; exit 2 ;;
    esac
done

# rounds feeds seq and arithmetic below; a bad value would silently run zero
# rounds (empty results, exit 0) instead of erroring.
case "$ROUNDS" in
    ''|*[!0-9]*) echo "rounds must be a positive integer, got '$ROUNDS'" >&2; exit 2 ;;
esac
[ "$ROUNDS" -gt 0 ] || { echo "rounds must be a positive integer, got '$ROUNDS'" >&2; exit 2; }
# An odd count leaves the A-first/B-first positions unbalanced (e.g. 5 rounds =
# A first 3x, B first 2x), so a repeatable position effect stays correlated
# with build identity. Allow it (quick smoke tests use 1), but say so.
[ $((ROUNDS % 2)) = 0 ] || \
    echo "NOTE: odd rounds=$ROUNDS leaves A/B start positions unbalanced; use an even count for measurement runs"

for b in "$BUILD_A" "$BUILD_B"; do
    [ -x "$b/sbin/unitd" ] || { echo "no executable $b/sbin/unitd" >&2; exit 2; }
done
[ "$LABEL_A" != "$LABEL_B" ] || { echo "labels must differ ($LABEL_A)" >&2; exit 2; }

# ---- config -----------------------------------------------------------------
CLK=$(getconf CLK_TCK)
# Keep RUNBASE short: the control socket path must fit AF_UNIX sun_path (~108
# bytes); deep scratch paths overflow it. Default to a shallow /tmp dir.
RUNBASE=${RUNBASE:-/tmp/claude-1000/bench-engines}
RESULTS=${RESULTS:-$PWD/bench-engines-results.txt}
PHPAPP=${PHPAPP:-}
# An explicit PHPAPP is caller-owned: require it to be usable NOW and never
# write into it (ensure_phpapp auto-creates an app only when PHPAPP is unset).
if [ -n "$PHPAPP" ] && [ ! -f "$PHPAPP/index.php" ]; then
    echo "PHPAPP=$PHPAPP has no index.php" >&2
    exit 2
fi

# Distinct port slots per build so an A and B never collide.
PORT_A=8910; PORT_A_PROXY=8911; PORT_A_PHP=8912
PORT_B=8920; PORT_B_PROXY=8921; PORT_B_PHP=8922

# Per-request timeout for every oha invocation (measurements AND warmups). oha's
# -t defaults to INFINITE, so a build that accepts a connection then stalls would
# hang the harness forever. A timed-out request is counted as an error (feeds
# `failed`), so a real stall surfaces as a failed measurement instead of a hang.
# Override via env; use a humantime duration (e.g. 5s, 500ms).
OHA_TIMEOUT=${OHA_TIMEOUT:-30s}

# Transfer timeout (seconds) for the control-socket PUTs in put_config_*. A wedged
# controller would otherwise block the script indefinitely before it can fail the
# round and clean up. Override via env.
CURL_MAXTIME=${CURL_MAXTIME:-30}

# Request counts. -n (not -z) for reproducibility. Scaled so each measurement is
# ~10-30s on a real run; override via env for the smoke test.
N_HIGH=${N_HIGH:-200000}
N_LOW=${N_LOW:-20000}
N_CLOSE=${N_CLOSE:-20000}
N_PROXY=${N_PROXY:-50000}
N_PHP=${N_PHP:-50000}

# Max failed requests a measurement may record before it fails the round/run.
# The summary tables don't surface `failed`, so without this an all-non-2xx run
# looks green as long as the tool exits 0.
MAX_FAILED=${MAX_FAILED:-0}
# A non-numeric MAX_FAILED makes the `-gt` comparison in oha_one/ab_one print an
# error and evaluate false, silently accepting a measurement that DID record
# failures. Require a non-negative integer up front, like rounds above.
case "$MAX_FAILED" in
    ''|*[!0-9]*) echo "MAX_FAILED must be a non-negative integer, got '$MAX_FAILED'" >&2; exit 2 ;;
esac

# CPU partition: server tree vs load generators.  The defaults encode the
# reference-box methodology (8 cores, 0-3/4-7); override via env on other
# hosts.  Validate up front: on a smaller box or a cpuset-limited runner an
# invalid list would otherwise fail every taskset invocation mid-run (or
# silently record empty measurements).
SERVER_CORES=${SERVER_CORES:-0-3}
LOAD_CORES=${LOAD_CORES:-4-7}
for cores in "$SERVER_CORES" "$LOAD_CORES"; do
    if ! taskset -c "$cores" true 2>/dev/null; then
        echo "cannot set CPU affinity '$cores' on this host" \
             "(set SERVER_CORES/LOAD_CORES to available CPUs)" >&2
        exit 2
    fi
done
# The two masks must also be DISJOINT: any shared CPU makes the load generators
# contend directly with the measured server tree, silently invalidating both
# the RPS and the CPU-per-request comparison the partition exists to protect.
expand_cores() {  # taskset CPU list -> one cpu id per line
    # Handles every taskset list form: single "N", range "N-M", and strided
    # range "N-M:S" (taskset(1): 0-31:2 = every 2nd cpu). Missing the stride
    # form made a strided mask expand to NOTHING, so the disjoint check silently
    # passed two overlapping strided masks.
    local part range stride lo hi
    echo "$1" | tr ',' '\n' | while IFS= read -r part; do
        [ -n "$part" ] || continue
        stride=1; range=$part
        case "$part" in *:*) stride=${part##*:}; range=${part%:*} ;; esac
        lo=${range%%-*}; hi=${range##*-}
        case "$lo$hi$stride" in ''|*[!0-9]*) continue ;; esac
        [ "$stride" -ge 1 ] || continue
        seq "$lo" "$stride" "$hi"
    done
}
CORE_OVERLAP=$(comm -12 <(expand_cores "$SERVER_CORES" | sort -u) \
                        <(expand_cores "$LOAD_CORES" | sort -u) | tr '\n' ' ')
if [ -n "${CORE_OVERLAP% }" ]; then
    echo "SERVER_CORES ($SERVER_CORES) and LOAD_CORES ($LOAD_CORES) overlap" \
         "on CPU(s) ${CORE_OVERLAP% }; the masks must be disjoint" >&2
    exit 2
fi

mkdir -p "$RUNBASE"
# fail fast: an unwritable results file would otherwise only surface after
# the first measurement (or lose every tee -a silently under set +e)
: > "$RESULTS" || { echo "cannot write results file $RESULTS" >&2; exit 2; }

# ---- tool discovery ---------------------------------------------------------
HAVE_JQ=0; command -v jq >/dev/null 2>&1 && HAVE_JQ=1
HAVE_PY=0; command -v python3 >/dev/null 2>&1 && HAVE_PY=1
HAVE_AB=0; command -v ab >/dev/null 2>&1 && HAVE_AB=1
OHA=${OHA:-/home/andy/bin/oha}
command -v "$OHA" >/dev/null 2>&1 || OHA=$(command -v oha 2>/dev/null || true)
[ -n "$OHA" ] || { echo "oha not found" >&2; exit 2; }
if [ "$HAVE_JQ" = 0 ] && [ "$HAVE_PY" = 0 ]; then
    echo "need jq or python3 to parse oha JSON" >&2; exit 2
fi
# Don't silently halve the matrix: a missing ab must abort unless the caller
# explicitly opted out via --skip-ab/SKIP_AB=1.
[ "$SKIP_AB" = 1 ] || [ "$HAVE_AB" = 1 ] || { echo "ab not found (use --skip-ab)" >&2; exit 2; }

# ---- state ------------------------------------------------------------------
UNITD_PID=""
ROUTER=""
declare -A ENGINE   # engine name per label

port_busy() {  # port -> 0 if in use
    if command -v ss >/dev/null 2>&1; then
        ss -ltn "sport = :$1" 2>/dev/null | grep -q LISTEN
    else
        # fd 3 lives only inside the subshell; nothing to close here
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
        return 1
    fi
}

wait_port() {  # host-port timeout_s -> 0 when listening
    local p=$1 timeout=${2:-10} i=0
    while [ "$i" -lt $((timeout*10)) ]; do
        if command -v ss >/dev/null 2>&1; then
            ss -ltn "sport = :$p" 2>/dev/null | grep -q LISTEN && return 0
        else
            (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null && return 0
        fi
        i=$((i+1)); sleep 0.1
    done
    return 1
}

cleanup() {
    if [ -n "$UNITD_PID" ] && kill -0 "$UNITD_PID" 2>/dev/null; then
        kill "$UNITD_PID" 2>/dev/null
        wait "$UNITD_PID" 2>/dev/null
    fi
    UNITD_PID=""
}
# INT/TERM must EXIT, not just clean up: after the handler returns bash would
# otherwise resume the script into later measurements with unitd already killed.
# cleanup is idempotent (clears UNITD_PID), so the subsequent EXIT trap is a
# harmless no-op.
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

router_cpu_ticks() {  # utime+stime of the router process; comm has a space
    # Default to 0 if the process vanished between checks: an empty value
    # would blow up the callers' $((...)) arithmetic.
    local ticks
    ticks=$(sed 's/.*) //' "/proc/$ROUTER/stat" 2>/dev/null | awk '{print $12+$13}')
    echo "${ticks:-0}"
}

tree_cpu_ticks() {  # unitd main + all descendants (router + app workers)
    # utime+stime of each live member PLUS its cutime+cstime: an app worker
    # that exits mid-measurement is reaped by its parent, which folds the dead
    # child's ticks into the parent's cutime/cstime. Without those fields a
    # worker exit (or churn/replacement) removes its CPU from the t1 snapshot
    # and the interval delta understates — or even goes negative. Live children
    # are never in a parent's cutime, so nothing is double-counted.
    local total=0 pid ticks
    for pid in $UNITD_PID $(pgrep -P "$UNITD_PID" 2>/dev/null) \
               $(pgrep -P "$UNITD_PID" 2>/dev/null | xargs -rn1 pgrep -P 2>/dev/null); do
        [ -r "/proc/$pid/stat" ] || continue
        ticks=$(sed 's/.*) //' "/proc/$pid/stat" 2>/dev/null | awk '{print $12+$13+$14+$15}')
        total=$((total + ${ticks:-0}))
    done
    echo "$total"
}

# ---- oha JSON parsing -------------------------------------------------------
# oha --no-tui --output-format json emits: .summary.requestsPerSec, .summary.successRate,
# .latencyPercentiles.p50 / .p99 (seconds), .statusCodeDistribution.
# failed counts every non-2xx status (3xx included) plus transport errors, so
# it matches ab's "Non-2xx responses" accounting below.
# The REQUIRED fields (requestsPerSec, p50, p99, statusCodeDistribution) must be
# present and numeric/object: a valid-JSON document without them (e.g. after an
# oha output-schema change) prints NOTHING, so the caller's empty-rps check
# fails the measurement instead of recording an all-zero row.
parse_oha() {  # jsonfile -> "rps p50ms p99ms failed"
    local f=$1
    if [ "$HAVE_JQ" = 1 ]; then
        jq -r '
          def ms(x): (x*1000);
          if   (.summary.requestsPerSec  | type) != "number"
            or (.latencyPercentiles."p50" | type) != "number"
            or (.latencyPercentiles."p99" | type) != "number"
            or (.statusCodeDistribution   | type) != "object"
          then empty
          else
          [ .summary.requestsPerSec,
            (ms(.latencyPercentiles."p50")),
            (ms(.latencyPercentiles."p99")),
            ( (( [ .statusCodeDistribution | to_entries[]
                   | select((.key|tonumber) >= 300 or (.key|tonumber) < 200) | .value ]
                 | add) // 0)
              + (( [ .errorDistribution // {} | to_entries[] | .value ] | add) // 0) )
          ] | @tsv
          end' "$f"
    else
        python3 - "$f" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
s=d.get("summary",{})
lp=d.get("latencyPercentiles",{})
rps=s.get("requestsPerSec")
p50=lp.get("p50")
p99=lp.get("p99")
scd=d.get("statusCodeDistribution")
def num(x): return isinstance(x,(int,float)) and not isinstance(x,bool)
if not (num(rps) and num(p50) and num(p99) and isinstance(scd,dict)):
    sys.exit(0)   # print nothing: required fields missing -> caller fails
failed=sum(v for k,v in scd.items() if str(k).isdigit() and (int(k)>=300 or int(k)<200))
failed+=sum((d.get("errorDistribution",{}) or {}).values())
print(f"{rps}\t{p50*1000}\t{p99*1000}\t{failed}")
PY
    fi
}

# ---- config payloads --------------------------------------------------------
json_string() {  # value -> JSON string literal (with surrounding quotes)
    # The php app root comes from PHPAPP or a RUNBASE-derived path; a '"' or '\'
    # (or any control char) in it would break the raw-interpolated control-socket
    # payload. Encode via the JSON tool we already require (jq or python3).
    if [ "$HAVE_JQ" = 1 ]; then
        jq -n --arg s "$1" '$s'
    else
        python3 -c 'import json,sys; sys.stdout.write(json.dumps(sys.argv[1]))' "$1"
    fi
}

put_config_http() {  # runsock port proxyport -> writes conf result, returns rc
    local sock=$1 port=$2 pport=$3 out=$4
    curl -s --max-time "$CURL_MAXTIME" --unix-socket "$sock" -X PUT -d '{
      "listeners": {
        "*:'"$port"'":  {"pass": "routes/direct"},
        "*:'"$pport"'": {"pass": "routes/proxy"}
      },
      "routes": {
        "direct": [{"action": {"return": 200}}],
        "proxy":  [{"action": {"proxy": "http://127.0.0.1:'"$port"'"}}]
      }
    }' http://localhost/config > "$out"
    grep -q '"success"' "$out"
}

put_config_php() {  # runsock port phproot -> writes conf result, returns rc
    local sock=$1 port=$2 root=$3 out=$4 rootjson
    rootjson=$(json_string "$root")
    curl -s --max-time "$CURL_MAXTIME" --unix-socket "$sock" -X PUT -d '{
      "listeners": {"*:'"$port"'": {"pass": "applications/hello"}},
      "applications": {"hello": {
          "type": "php", "root": '"$rootjson"', "script": "index.php",
          "processes": 2
      }}
    }' http://localhost/config > "$out"
    grep -q '"success"' "$out"
}

# ---- engine assertion -------------------------------------------------------
detect_engine() {  # rundir -> engine name (or "unknown")
    local run=$1 name
    # implementation branch logs:  using "epoll" event engine
    # The main process emits it on stderr (before the log file opens); router
    # workers emit it into unit.log later. Check both.
    name=$(cat "$run/stderr.log" "$run/unit.log" 2>/dev/null \
           | grep -oE 'using "[^"]+" event engine' | head -1 \
           | sed -E 's/using "([^"]+)".*/\1/')
    [ -n "$name" ] && echo "$name" || echo "unknown"
}

# ---- lifecycle --------------------------------------------------------------
start_unitd() {  # label build run -> sets UNITD_PID, ROUTER, ENGINE[label]
    local label=$1 build=$2 run=$3 mods=$4
    rm -rf "$run"; mkdir -p "$run" "$run/state" "$run/modules"
    # AF_UNIX sun_path is ~108 bytes; unitd binds "<run>/control.sock.tmp" (+17).
    if [ "$(( ${#run} + 17 ))" -ge 108 ]; then
        echo "run path too long for AF_UNIX socket ($run); set a shorter RUNBASE" >&2
        return 1
    fi
    # Pin the whole server tree at spawn (children inherit affinity); the
    # later router re-pin is then just a no-op safety net.
    # stderr must be captured: the main process logs the engine line before
    # the log file is opened, so it only ever appears on stderr.
    taskset -c "$SERVER_CORES" "$build/sbin/unitd" --no-daemon \
        --control "unix:$run/control.sock" \
        --pid "$run/unit.pid" --log "$run/unit.log" \
        --statedir "$run/state" --tmpdir "$run" \
        --modulesdir "$mods" \
        >/dev/null 2>"$run/stderr.log" &
    UNITD_PID=$!
    # wait for the control socket instead of a bare sleep
    local i=0
    while [ ! -S "$run/control.sock" ] && [ "$i" -lt 100 ]; do
        kill -0 "$UNITD_PID" 2>/dev/null || { echo "unitd died on start ($label)"; return 1; }
        i=$((i+1)); sleep 0.1
    done
    [ -S "$run/control.sock" ] || { echo "no control socket ($label)"; return 1; }
    ENGINE[$label]=$(detect_engine "$run")
    return 0
}

pin_router() {  # run -> sets ROUTER, pins to server cores
    ROUTER=$(pgrep -P "$UNITD_PID" -f router | head -1)
    [ -n "$ROUTER" ] || { echo "no router pid"; return 1; }
    taskset -pc "$SERVER_CORES" "$ROUTER" >/dev/null 2>&1
    return 0
}

# ---- measurement ------------------------------------------------------------
# oha_one: label scenario url nreq conc keepalive_flag mode
#   mode = router | tree   (which CPU accounting)
oha_one() {
    local run=$1 label=$2 scen=$3 url=$4 n=$5 c=$6 ka=$7 mode=$8
    local t0 t1 rc js="$run/oha-$scen.json"
    local ohaka=()
    [ "$ka" = 1 ] && ohaka=(-c "$c") || ohaka=(-c "$c" --disable-keepalive)
    if [ "$mode" = tree ]; then t0=$(tree_cpu_ticks); else t0=$(router_cpu_ticks); fi
    taskset -c "$LOAD_CORES" "$OHA" --no-tui --output-format json -t "$OHA_TIMEOUT" \
        -n "$n" "${ohaka[@]}" "$url" \
        > "$js" 2>"$run/oha-$scen.err"
    rc=$?
    if [ "$mode" = tree ]; then t1=$(tree_cpu_ticks); else t1=$(router_cpu_ticks); fi
    # Router mode: a crash/restart mid-measurement makes router_cpu_ticks default
    # to 0, yielding a bogus 0/negative CPU sample that would still pass. Fail if
    # the router is gone. (Tree mode: app worker churn is the daemon's business.)
    if [ "$mode" = router ] && [ ! -r "/proc/$ROUTER/stat" ]; then
        echo "router pid $ROUTER vanished mid-measurement: $label $scen" >&2
        return 1
    fi
    local rps p50 p99 failed
    IFS=$'\t' read -r rps p50 p99 failed < <(parse_oha "$js")
    # A dead/unstartable oha (or unparsable JSON) must fail the measurement:
    # otherwise printf records an all-zero row and the round looks green.
    if [ "$rc" -ne 0 ] || [ -z "$rps" ]; then
        echo "oha measurement failed: $label $scen (rc=$rc, see $run/oha-$scen.err)" >&2
        return 1
    fi
    local us
    us=$(LC_ALL=C awk -v a="$t0" -v b="$t1" -v clk="$CLK" -v n="$n" \
        'BEGIN{printf "%.2f", (b-a)*1000000/clk/n}')
    # A failing append (e.g. RESULTS on a full disk) must fail the measurement:
    # the trailing failed-check below would otherwise overwrite the pipeline
    # status and the row would be lost while oha_one still returned success.
    if ! printf '%s %s oha rps=%.0f p50ms=%.3f p99ms=%.3f failed=%s cpu_us_per_req=%s engine=%s\n' \
            "$label" "$scen" "$rps" "$p50" "$p99" "${failed:-0}" "$us" "${ENGINE[$label]}" \
            | tee -a "$RESULTS"; then
        echo "failed to append measurement row to $RESULTS: $label $scen" >&2
        return 1
    fi
    # Recorded the row for forensics; now fail the measurement if it saw request
    # failures (non-2xx/transport) — the summary tables don't surface `failed`.
    if [ "${failed:-0}" -gt "$MAX_FAILED" ]; then
        echo "oha measurement recorded ${failed:-0} failed request(s) (>MAX_FAILED=$MAX_FAILED): $label $scen" >&2
        return 1
    fi
}

# ab_one: secondary continuity metric (keepalive matching bench-ab.sh numbers)
ab_one() {
    local run=$1 label=$2 scen=$3 url=$4 n=$5 c=$6 ka=$7 mode=$8
    [ "$SKIP_AB" = 1 ] && return 0
    local t0 t1 rc out="$run/ab-$scen.txt" extra=""
    [ "$ka" = 1 ] && extra="-k"
    if [ "$mode" = tree ]; then t0=$(tree_cpu_ticks); else t0=$(router_cpu_ticks); fi
    # shellcheck disable=SC2086
    taskset -c "$LOAD_CORES" ab $extra -n "$n" -c "$c" -q "$url" > "$out" 2>&1
    rc=$?
    if [ "$mode" = tree ]; then t1=$(tree_cpu_ticks); else t1=$(router_cpu_ticks); fi
    # Router mode: a crash/restart mid-measurement makes router_cpu_ticks default
    # to 0, yielding a bogus 0/negative CPU sample that would still pass. Fail if
    # the router is gone. (Tree mode: app worker churn is the daemon's business.)
    if [ "$mode" = router ] && [ ! -r "/proc/$ROUTER/stat" ]; then
        echo "router pid $ROUTER vanished mid-measurement: $label $scen" >&2
        return 1
    fi
    local rps p99 failed non2xx us
    rps=$(awk '/Requests per second/{print $4}' "$out")
    # Same rationale as oha_one: a failed ab must not record a zero-value row.
    if [ "$rc" -ne 0 ] || [ -z "$rps" ]; then
        echo "ab measurement failed: $label $scen (rc=$rc, see $out)" >&2
        return 1
    fi
    p99=$(awk '/ 99%/{print $2}' "$out")
    # ab reports HTTP-status failures on a separate "Non-2xx responses" line,
    # NOT in "Failed requests"; count both or 5xx regressions record failed=0.
    failed=$(awk '/Failed requests/{print $3}' "$out")
    non2xx=$(awk '/Non-2xx responses/{print $3}' "$out")
    failed=$(( ${failed:-0} + ${non2xx:-0} ))
    us=$(LC_ALL=C awk -v a="$t0" -v b="$t1" -v clk="$CLK" -v n="$n" \
        'BEGIN{printf "%.2f", (b-a)*1000000/clk/n}')
    # Same rationale as oha_one: a failing append must fail the measurement, or
    # the trailing failed-check overwrites the pipeline status and the row is lost.
    if ! printf '%s %s ab rps=%s p50ms=NA p99ms=%s failed=%s cpu_us_per_req=%s engine=%s\n' \
            "$label" "$scen" "${rps:-0}" "${p99:-NA}" "${failed:-0}" "$us" "${ENGINE[$label]}" \
            | tee -a "$RESULTS"; then
        echo "failed to append measurement row to $RESULTS: $label $scen" >&2
        return 1
    fi
    # Recorded the row for forensics; now fail the measurement if it saw request
    # failures (Failed requests + Non-2xx) — the summary tables don't surface it.
    if [ "${failed:-0}" -gt "$MAX_FAILED" ]; then
        echo "ab measurement recorded ${failed:-0} failed request(s) (>MAX_FAILED=$MAX_FAILED): $label $scen" >&2
        return 1
    fi
}

# warmup_one: a VALIDATED warmup pass. oha's default discards status and JSON, so
# a warmup that errors or returns non-2xx would silently precede the first
# measurement, leaving it to run against a cold or still-recovering build. Apply
# the same request-failure policy as a measurement (exit status + failed-count vs
# MAX_FAILED); record no result row. Non-zero return fails the build-round.
warmup_one() {  # run label tag url nreq conc
    local run=$1 label=$2 tag=$3 url=$4 n=$5 c=$6
    local rc rps p50 p99 failed js="$run/warmup-$tag.json"
    taskset -c "$LOAD_CORES" "$OHA" --no-tui --output-format json -t "$OHA_TIMEOUT" \
        -n "$n" -c "$c" "$url" > "$js" 2>"$run/warmup-$tag.err"
    rc=$?
    IFS=$'\t' read -r rps p50 p99 failed < <(parse_oha "$js")
    if [ "$rc" -ne 0 ] || [ -z "$rps" ]; then
        echo "warmup failed: $label $tag (rc=$rc, see $run/warmup-$tag.err)" >&2
        return 1
    fi
    if [ "${failed:-0}" -gt "$MAX_FAILED" ]; then
        echo "warmup recorded ${failed:-0} failed request(s) (>MAX_FAILED=$MAX_FAILED): $label $tag" >&2
        return 1
    fi
    return 0
}

# ---- per-build round --------------------------------------------------------
run_build_round() {
    local label=$1 build=$2 slot=$3 rnd=$4
    local port pport pphp mods run meas_failed=0
    if [ "$slot" = A ]; then
        port=$PORT_A; pport=$PORT_A_PROXY; pphp=$PORT_A_PHP
    else
        port=$PORT_B; pport=$PORT_B_PROXY; pphp=$PORT_B_PHP
    fi
    run="$RUNBASE/$label-r$rnd"
    mods="$build/lib/unit/modules"
    [ -d "$mods" ] || mods="$run/modules"

    # refuse if ports busy (php port too: a bound $pphp would otherwise
    # degrade to the php SKIP path instead of aborting the round)
    for p in "$port" "$pport" "$pphp"; do
        if port_busy "$p"; then echo "port $p busy, aborting" >&2; return 1; fi
    done

    echo "== round $rnd  build=$label ($build) engine-slot=$slot =="
    # Every early-failure return must run cleanup first, or the half-started
    # unitd leaks: it holds the ports (aborting later rounds) and the EXIT
    # trap only knows the LAST spawned pid.
    start_unitd "$label" "$build" "$run" "$mods" || { cleanup; return 1; }

    # --- HTTP scenarios ---
    put_config_http "$run/control.sock" "$port" "$pport" "$run/conf-http.json" \
        || { echo "CONF FAILED $label"; cat "$run/conf-http.json"; cleanup; return 1; }
    wait_port "$port" 10 \
        || { echo "listener $port never came up"; cleanup; return 1; }
    pin_router "$run" || { cleanup; return 1; }

    # warmup (validated: a cold/erroring warmup fails the round instead of
    # silently preceding the first measurement)
    warmup_one "$run" "$label" http "http://127.0.0.1:$port/" 2000 16 || meas_failed=1

    # Collect measurement failures instead of aborting mid-round: the round
    # must still tear down cleanly, then report failure via FAILED_ROUNDS.
    oha_one "$run" "$label" ret200_ka_hi "http://127.0.0.1:$port/"  "$N_HIGH"  256 1 router || meas_failed=1
    ab_one  "$run" "$label" ret200_ka_hi "http://127.0.0.1:$port/"  "$N_HIGH"  256 1 router || meas_failed=1
    oha_one "$run" "$label" ret200_ka_lo "http://127.0.0.1:$port/"  "$N_LOW"     8 1 router || meas_failed=1
    ab_one  "$run" "$label" ret200_ka_lo "http://127.0.0.1:$port/"  "$N_LOW"     8 1 router || meas_failed=1
    oha_one "$run" "$label" ret200_close "http://127.0.0.1:$port/"  "$N_CLOSE"  16 0 router || meas_failed=1
    ab_one  "$run" "$label" ret200_close "http://127.0.0.1:$port/"  "$N_CLOSE"  16 0 router || meas_failed=1
    oha_one "$run" "$label" proxy_ka     "http://127.0.0.1:$pport/" "$N_PROXY"  64 1 router || meas_failed=1
    ab_one  "$run" "$label" proxy_ka     "http://127.0.0.1:$pport/" "$N_PROXY"  64 1 router || meas_failed=1

    # --- PHP scenario (optional) ---
    local php_mod=""
    [ -d "$build/lib/unit/modules" ] && \
        php_mod=$(find "$build/lib/unit/modules" -maxdepth 1 -name 'php*.so' 2>/dev/null | head -1)
    if [ -n "$php_mod" ]; then
        local root
        root=$(ensure_phpapp) || root=""
        if [ -n "$root" ]; then
            if cleanup_http_reconfig "$run" "$pphp" "$root"; then
                warmup_one "$run" "$label" php "http://127.0.0.1:$pphp/" 5000 8 || meas_failed=1
                oha_one "$run" "$label" php_ka "http://127.0.0.1:$pphp/" "$N_PHP" 8 1 tree || meas_failed=1
                ab_one  "$run" "$label" php_ka "http://127.0.0.1:$pphp/" "$N_PHP" 8 1 tree || meas_failed=1
            else
                # module + app dir are BOTH present, so a config/listener
                # failure is a real regression in this build, not a missing
                # optional dependency: fail the round, never degrade to a
                # SKIP that leaves one side without PHP samples at exit 0.
                echo "php config/listener FAILED for $label (see $run/conf-php.json)" >&2
                meas_failed=1
            fi
        else
            echo "$label php_ka SKIP php (no app dir)" | tee -a "$RESULTS"
        fi
    else
        echo "$label php_ka SKIP php (no php module in $build)" | tee -a "$RESULTS"
    fi

    cleanup
    # Fail the round if any measurement failed, so the caller's FAILED_ROUNDS
    # accounting (and the final non-zero exit) catches incomplete data.
    [ "$meas_failed" -eq 0 ]
}

cleanup_http_reconfig() {  # reuse the running unitd, swap to php config on pphp
    local run=$1 pphp=$2 root=$3
    put_config_php "$run/control.sock" "$pphp" "$root" "$run/conf-php.json" || return 1
    wait_port "$pphp" 10 || return 1
    return 0
}

ensure_phpapp() {  # -> prints app root, or empty on failure
    local root=$PHPAPP
    if [ -n "$root" ]; then
        # explicit app root: validated at startup, caller-owned — NEVER write
        # into it (auto-creation applies only to the harness's own dir below)
        echo "$root"
        return 0
    fi
    root="$RUNBASE/phpapp"
    if [ ! -f "$root/index.php" ]; then
        mkdir -p "$root" 2>/dev/null || { return 1; }
        printf '%s\n' '<?php echo "hello\n";' > "$root/index.php" 2>/dev/null || return 1
    fi
    echo "$root"
}

# ---- engine preflight + assertion (before any measurement) ------------------
preflight_engine() {  # label build -> detects ENGINE[label], then kills
    local label=$1 build=$2
    local run="$RUNBASE/$label-preflight"
    local mods="$build/lib/unit/modules"
    [ -d "$mods" ] || mods="$run/modules"
    start_unitd "$label" "$build" "$run" "$mods" || {
        echo "preflight: $label failed to start" >&2; cleanup; return 1; }
    cleanup
}

echo "bench-engines: A=$LABEL_A ($BUILD_A)  B=$LABEL_B ($BUILD_B)  rounds=$ROUNDS"
echo "results -> $RESULTS   run dirs -> $RUNBASE"

preflight_engine "$LABEL_A" "$BUILD_A" || exit 1
preflight_engine "$LABEL_B" "$BUILD_B" || exit 1
EA=${ENGINE[$LABEL_A]:-unknown}
EB=${ENGINE[$LABEL_B]:-unknown}
echo "engines: $LABEL_A=$EA  $LABEL_B=$EB"
if [ "$EA" = unknown ] || [ "$EB" = unknown ]; then
    echo "WARNING: could not detect engine for at least one build (old build without the"
    echo "         'using \"<name>\" event engine' log line). Same-engine check skipped."
elif [ "$EA" = "$EB" ]; then
    if [ "$ALLOW_SAME" = 1 ]; then
        echo "NOTE: both builds report engine=$EA (A/A mode, --allow-same set)."
    else
        echo "ERROR: both builds report the SAME engine ($EA). This is not an A/B" >&2
        echo "       comparison. Re-run with --allow-same for an intentional A/A test." >&2
        exit 1
    fi
fi

# ---- main loop --------------------------------------------------------------
FAILED_ROUNDS=0
for r in $(seq 1 "$ROUNDS"); do
    # Interleave within the round AND alternate the order per round: with a
    # fixed A-then-B order any monotonic warmup/thermal drift stays correlated
    # with the build label; alternating puts each build in both positions.
    if [ $((r % 2)) = 1 ]; then
        run_build_round "$LABEL_A" "$BUILD_A" A "$r" \
            || { echo "round $r A failed" >&2; FAILED_ROUNDS=$((FAILED_ROUNDS+1)); }
        run_build_round "$LABEL_B" "$BUILD_B" B "$r" \
            || { echo "round $r B failed" >&2; FAILED_ROUNDS=$((FAILED_ROUNDS+1)); }
    else
        run_build_round "$LABEL_B" "$BUILD_B" B "$r" \
            || { echo "round $r B failed" >&2; FAILED_ROUNDS=$((FAILED_ROUNDS+1)); }
        run_build_round "$LABEL_A" "$BUILD_A" A "$r" \
            || { echo "round $r A failed" >&2; FAILED_ROUNDS=$((FAILED_ROUNDS+1)); }
    fi
done

# ---- summary ----------------------------------------------------------------
echo
SUMMARIZER="$(dirname "$0")/bench-engines-report.py"
if [ "$HAVE_PY" = 1 ] && [ -f "$SUMMARIZER" ]; then
    python3 "$SUMMARIZER" "$RESULTS" "$LABEL_A" "$LABEL_B"
else
    # inline awk fallback summary (mean only)
    echo "### summary (mean RPS, oha)"
    awk '$3=="oha"{
        split($4,a,"="); rps=a[2];
        key=$1" "$2; sum[key]+=rps; cnt[key]++
    } END{ for(k in sum) printf "  %s  mean_rps=%.0f (n=%d)\n", k, sum[k]/cnt[k], cnt[k] }' \
        "$RESULTS" | sort
fi

# A failed round means incomplete/contaminated results: still print the
# summary above for whatever data exists, but exit non-zero so automation
# never mistakes this for a good A/B run.
if [ "$FAILED_ROUNDS" -gt 0 ]; then
    echo "ERROR: $FAILED_ROUNDS build-round(s) failed; results are incomplete" >&2
    exit 1
fi

exit 0
