#!/usr/bin/env bash
#ddev-generated
#
# web_extra_daemons entrypoint: render the configuration into unitd's
# state directory and run unitd in the foreground. supervisord restarts
# this on `ddev restart`, `ddev freeunit restart` and on failure, so every
# start re-renders from the current environment and nothing accumulates.
# unitd loads <statedir>/conf.json itself; no control-API call is needed.

set -euo pipefail

mkdir -p /run/freeunit/state
bash /mnt/ddev_config/freeunit/render-config.sh > /run/freeunit/state/conf.json
exec unitd --no-daemon
