#!/usr/bin/env bash
#ddev-generated
#
# Entrypoint for the "freeunit" web_extra_daemons entry in
# config.freeunit.yaml. supervisord (inside the web container) runs this in
# the foreground and restarts it if it exits, so it does the same three
# things every time it (re)starts: render the configuration, start unitd,
# PUT the configuration over the control socket. That makes a restart of
# this daemon -- `ddev restart`, `supervisorctl restart 'webextradaemons:*'`,
# or a container recreate -- pick up any change with no extra step, and
# running it twice in a row is harmless.

set -euo pipefail

CONTROL_SOCK=/var/run/freeunit-control.sock
STATE_DIR=/var/lib/freeunit
RUN_DIR=/var/run
LOG_FILE=/var/log/freeunit.log
CONF_FILE="${RUN_DIR}/freeunit.json"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$STATE_DIR" "$RUN_DIR"
rm -f "$CONTROL_SOCK"

"${SELF_DIR}/render-config.sh" > "$CONF_FILE"

/usr/sbin/unitd-freeunit \
    --no-daemon \
    --control "unix:${CONTROL_SOCK}" \
    --pid "${RUN_DIR}/freeunit.pid" \
    --log "$LOG_FILE" \
    --statedir "$STATE_DIR" &
UNITD_PID=$!

trap 'kill "$UNITD_PID" 2>/dev/null || true; wait "$UNITD_PID" 2>/dev/null || true' TERM INT

# Wait for the control API, then push the rendered configuration. 30 * 0.5s
# is generous; a slow first start (cold filesystem cache) is the only
# realistic reason this would take more than a second or two.
ready=0
for _ in $(seq 1 30); do
    if curl -fs --unix-socket "$CONTROL_SOCK" http://localhost/status >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.5
done

if [ "$ready" != 1 ]; then
    echo "start-freeunit.sh: unitd did not open its control socket in time" >&2
    kill "$UNITD_PID" 2>/dev/null || true
    exit 1
fi

if ! curl -fsS -X PUT --data-binary @"$CONF_FILE" --unix-socket "$CONTROL_SOCK" http://localhost/config; then
    echo "start-freeunit.sh: applying ${CONF_FILE} failed, see ${LOG_FILE}" >&2
    kill "$UNITD_PID" 2>/dev/null || true
    exit 1
fi

wait "$UNITD_PID"
