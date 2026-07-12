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
#     rounds default 5. Builds are interleaved A/B/A/B across rounds to cancel
#     thermal/cache drift.
#
#   ENV OVERRIDES
#     RUNBASE=/path      base dir for per-run scratch (default under scratch/tmp)
#     RESULTS=file       machine-parsable results file (default ./bench-engines-results.txt)
#     PHPAPP=dir         PHP app root for the php scenario; must already contain
#                        index.php (validated up front; the harness never writes
#                        into it). Auto-created under RUNBASE only when unset.
#     ALLOW_SAME=1       permit both builds to report the SAME engine (A/A mode)
#     SKIP_AB=1          skip the secondary ApacheBench pass
#     N_HIGH, N_LOW, N_CLOSE, N_PROXY, N_PHP   request counts (reproducibility knob)
#     OHA_TIMEOUT=dur    per-request oha timeout (default 30s; e.g. 5s, 500ms)
#     CURL_MAXTIME=secs  control-socket PUT transfer timeout (default 30)
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

SERVER_CORES=0-3
LOAD_CORES=4-7

mkdir -p "$RUNBASE"
: > "$RESULTS"

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
[ "$SKIP_AB" = 1 ] || [ "$HAVE_AB" = 1 ] || { echo "ab not found (use --skip-ab)" >&2; SKIP_AB=1; }

# ---- state ------------------------------------------------------------------
UNITD_PID=""
ROUTER=""
declare -A ENGINE   # engine name per label

port_busy() {  # port -> 0 if in use
    if command -v ss >/dev/null 2>&1; then
        ss -ltn "sport = :$1" 2>/dev/null | grep -q LISTEN
    else
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3>&- 3<&-; return 0; }
        return 1
    fi
}

wait_port() {  # host-port timeout_s -> 0 when listening
    local p=$1 timeout=${2:-10} i=0
    while [ "$i" -lt $((timeout*10)) ]; do
        if command -v ss >/dev/null 2>&1; then
            ss -ltn "sport = :$p" 2>/dev/null | grep -q LISTEN && return 0
        else
            (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null && { exec 3>&- 3<&-; return 0; }
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
trap cleanup EXIT INT TERM

router_cpu_ticks() {  # utime+stime of the router process; comm has a space
    sed 's/.*) //' "/proc/$ROUTER/stat" | awk '{print $12+$13}'
}

tree_cpu_ticks() {  # unitd main + all descendants (router + app workers)
    local total=0 pid
    for pid in $UNITD_PID $(pgrep -P "$UNITD_PID" 2>/dev/null) \
               $(pgrep -P "$UNITD_PID" 2>/dev/null | xargs -rn1 pgrep -P 2>/dev/null); do
        [ -r "/proc/$pid/stat" ] || continue
        total=$((total + $(sed 's/.*) //' "/proc/$pid/stat" | awk '{print $12+$13}')))
    done
    echo "$total"
}

# ---- oha JSON parsing -------------------------------------------------------
# oha --no-tui --output-format json emits: .summary.requestsPerSec, .summary.successRate,
# .latencyPercentiles.p50 / .p99 (seconds), .statusCodeDistribution.
parse_oha() {  # jsonfile -> "rps p50ms p99ms failed"
    local f=$1
    if [ "$HAVE_JQ" = 1 ]; then
        jq -r '
          def ms(x): (x*1000);
          [ (.summary.requestsPerSec // 0),
            (ms(.latencyPercentiles."p50" // 0)),
            (ms(.latencyPercentiles."p99" // 0)),
            ( (( [ .statusCodeDistribution // {} | to_entries[]
                   | select((.key|tonumber) >= 400 or (.key|tonumber) < 200) | .value ]
                 | add) // 0)
              + (( [ .errorDistribution // {} | to_entries[] | .value ] | add) // 0) )
          ] | @tsv' "$f"
    else
        python3 - "$f" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
s=d.get("summary",{})
lp=d.get("latencyPercentiles",{})
rps=s.get("requestsPerSec",0) or 0
p50=(lp.get("p50",0) or 0)*1000
p99=(lp.get("p99",0) or 0)*1000
scd=d.get("statusCodeDistribution",{}) or {}
failed=sum(v for k,v in scd.items() if str(k).isdigit() and (int(k)>=400 or int(k)<200))
failed+=sum((d.get("errorDistribution",{}) or {}).values())
print(f"{rps}\t{p50}\t{p99}\t{failed}")
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
    local t0 t1 js="$run/oha-$scen.json"
    local ohaka=()
    [ "$ka" = 1 ] && ohaka=(-c "$c") || ohaka=(-c "$c" --disable-keepalive)
    if [ "$mode" = tree ]; then t0=$(tree_cpu_ticks); else t0=$(router_cpu_ticks); fi
    taskset -c "$LOAD_CORES" "$OHA" --no-tui --output-format json -t "$OHA_TIMEOUT" \
        -n "$n" "${ohaka[@]}" "$url" \
        > "$js" 2>"$run/oha-$scen.err"
    if [ "$mode" = tree ]; then t1=$(tree_cpu_ticks); else t1=$(router_cpu_ticks); fi
    local rps p50 p99 failed
    IFS=$'\t' read -r rps p50 p99 failed < <(parse_oha "$js")
    local us
    us=$(LC_ALL=C awk -v a="$t0" -v b="$t1" -v clk="$CLK" -v n="$n" \
        'BEGIN{printf "%.2f", (b-a)*1000000/clk/n}')
    printf '%s %s oha rps=%.0f p50ms=%.3f p99ms=%.3f failed=%s cpu_us_per_req=%s engine=%s\n' \
        "$label" "$scen" "$rps" "$p50" "$p99" "${failed:-0}" "$us" "${ENGINE[$label]}" \
        | tee -a "$RESULTS"
}

# ab_one: secondary continuity metric (keepalive matching bench-ab.sh numbers)
ab_one() {
    local run=$1 label=$2 scen=$3 url=$4 n=$5 c=$6 ka=$7 mode=$8
    [ "$SKIP_AB" = 1 ] && return 0
    local t0 t1 out="$run/ab-$scen.txt" extra=""
    [ "$ka" = 1 ] && extra="-k"
    if [ "$mode" = tree ]; then t0=$(tree_cpu_ticks); else t0=$(router_cpu_ticks); fi
    # shellcheck disable=SC2086
    taskset -c "$LOAD_CORES" ab $extra -n "$n" -c "$c" -q "$url" > "$out" 2>&1
    if [ "$mode" = tree ]; then t1=$(tree_cpu_ticks); else t1=$(router_cpu_ticks); fi
    local rps p99 failed us
    rps=$(awk '/Requests per second/{print $4}' "$out")
    p99=$(awk '/ 99%/{print $2}' "$out")
    failed=$(awk '/Failed requests/{print $3}' "$out")
    us=$(LC_ALL=C awk -v a="$t0" -v b="$t1" -v clk="$CLK" -v n="$n" \
        'BEGIN{printf "%.2f", (b-a)*1000000/clk/n}')
    printf '%s %s ab rps=%s p50ms=NA p99ms=%s failed=%s cpu_us_per_req=%s engine=%s\n' \
        "$label" "$scen" "${rps:-0}" "${p99:-NA}" "${failed:-0}" "$us" "${ENGINE[$label]}" \
        | tee -a "$RESULTS"
}

# ---- per-build round --------------------------------------------------------
run_build_round() {
    local label=$1 build=$2 slot=$3 rnd=$4
    local port pport pphp mods run
    if [ "$slot" = A ]; then
        port=$PORT_A; pport=$PORT_A_PROXY; pphp=$PORT_A_PHP
    else
        port=$PORT_B; pport=$PORT_B_PROXY; pphp=$PORT_B_PHP
    fi
    run="$RUNBASE/$label-r$rnd"
    mods="$build/lib/unit/modules"
    [ -d "$mods" ] || mods="$run/modules"

    # refuse if ports busy
    for p in "$port" "$pport"; do
        if port_busy "$p"; then echo "port $p busy, aborting" >&2; return 1; fi
    done

    echo "== round $rnd  build=$label ($build) engine-slot=$slot =="
    start_unitd "$label" "$build" "$run" "$mods" || return 1

    # --- HTTP scenarios ---
    put_config_http "$run/control.sock" "$port" "$pport" "$run/conf-http.json" \
        || { echo "CONF FAILED $label"; cat "$run/conf-http.json"; return 1; }
    wait_port "$port" 10   || { echo "listener $port never came up"; return 1; }
    pin_router "$run" || return 1

    # warmup
    taskset -c "$LOAD_CORES" "$OHA" --no-tui --output-format json -t "$OHA_TIMEOUT" -n 2000 -c 16 \
        "http://127.0.0.1:$port/" >/dev/null 2>&1

    oha_one "$run" "$label" ret200_ka_hi "http://127.0.0.1:$port/"  "$N_HIGH"  256 1 router
    ab_one  "$run" "$label" ret200_ka_hi "http://127.0.0.1:$port/"  "$N_HIGH"  256 1 router
    oha_one "$run" "$label" ret200_ka_lo "http://127.0.0.1:$port/"  "$N_LOW"     8 1 router
    ab_one  "$run" "$label" ret200_ka_lo "http://127.0.0.1:$port/"  "$N_LOW"     8 1 router
    oha_one "$run" "$label" ret200_close "http://127.0.0.1:$port/"  "$N_CLOSE"  16 0 router
    ab_one  "$run" "$label" ret200_close "http://127.0.0.1:$port/"  "$N_CLOSE"  16 0 router
    oha_one "$run" "$label" proxy_ka     "http://127.0.0.1:$pport/" "$N_PROXY"  64 1 router
    ab_one  "$run" "$label" proxy_ka     "http://127.0.0.1:$pport/" "$N_PROXY"  64 1 router

    # --- PHP scenario (optional) ---
    local php_mod=""
    [ -d "$build/lib/unit/modules" ] && \
        php_mod=$(find "$build/lib/unit/modules" -maxdepth 1 -name 'php*.so' 2>/dev/null | head -1)
    if [ -n "$php_mod" ]; then
        local root
        root=$(ensure_phpapp) || root=""
        if [ -n "$root" ]; then
            if cleanup_http_reconfig "$run" "$pphp" "$root"; then
                taskset -c "$LOAD_CORES" "$OHA" --no-tui --output-format json -t "$OHA_TIMEOUT" -n 5000 -c 8 \
                    "http://127.0.0.1:$pphp/" >/dev/null 2>&1
                oha_one "$run" "$label" php_ka "http://127.0.0.1:$pphp/" "$N_PHP" 8 1 tree
                ab_one  "$run" "$label" php_ka "http://127.0.0.1:$pphp/" "$N_PHP" 8 1 tree
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
        echo "preflight: $label failed to start" >&2; return 1; }
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
for r in $(seq 1 "$ROUNDS"); do
    # interleave A then B each round to cancel drift
    run_build_round "$LABEL_A" "$BUILD_A" A "$r" || { echo "round $r A failed" >&2; }
    run_build_round "$LABEL_B" "$BUILD_B" B "$r" || { echo "round $r B failed" >&2; }
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

exit 0
