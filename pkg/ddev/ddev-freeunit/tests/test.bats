#!/usr/bin/env bats

# Bats is a testing framework for Bash
# Documentation https://bats-core.readthedocs.io/en/stable/
# Bats libraries documentation https://github.com/ztombol/bats-docs
#
# For local tests, install bats-core, bats-assert, bats-file, bats-support
# And run this in the add-on root directory:
#   bats ./tests/test.bats
# For debugging:
#   bats ./tests/test.bats --show-output-of-passing-tests --verbose-run --print-output-on-failure

setup() {
  set -eu -o pipefail

  # Override this variable once this add-on has its own repository:
  export GITHUB_REPO=freeunitorg/ddev-freeunit

  TEST_BREW_PREFIX="$(brew --prefix 2>/dev/null || true)"
  export BATS_LIB_PATH="${BATS_LIB_PATH}:${TEST_BREW_PREFIX}/lib:/usr/lib/bats"
  bats_load_library bats-assert
  bats_load_library bats-file
  bats_load_library bats-support

  export DIR="$(cd "$(dirname "${BATS_TEST_FILENAME}")/.." >/dev/null 2>&1 && pwd)"
  export PROJNAME="test-ddev-freeunit"
  mkdir -p "${HOME}/tmp"
  export TESTDIR="$(mktemp -d "${HOME}/tmp/${PROJNAME}.XXXXXX")"
  export DDEV_NONINTERACTIVE=true
  export DDEV_NO_INSTRUMENTATION=true
  ddev delete -Oy "${PROJNAME}" >/dev/null 2>&1 || true
  cd "${TESTDIR}"

  # A plain PHP project is enough to exercise the generic route (static +
  # PHP + Xdebug); Drupal-specific routing is exercised by render-config.sh
  # unit behavior, not re-tested here through a full Drupal install.
  mkdir -p web
  cat <<'PHP' > web/index.php
<?php
echo "freeunit-test-ok\n";
PHP
  echo "static-ok" > web/robots.txt

  run ddev config --project-name="${PROJNAME}" --project-type=php --docroot=web --project-tld=ddev.site
  assert_success
}

health_checks() {
  # The PHP route responds and actually ran through PHP.
  run curl -sf "https://${PROJNAME}.ddev.site/"
  assert_success
  assert_output --partial "freeunit-test-ok"

  # Static files are served directly (not through the PHP application).
  run curl -sf "https://${PROJNAME}.ddev.site/robots.txt"
  assert_success
  assert_output --partial "static-ok"

  # The response comes from FreeUnit, not nginx/apache.
  run curl -sfI "https://${PROJNAME}.ddev.site/"
  assert_success
  assert_output --regexp "^[Ss]erver: [Uu]nit"

  # The control command works and reports the "app" application.
  run ddev freeunit status
  assert_success
  assert_output --partial "applications"

  # Config re-apply works without disturbing the running site.
  run ddev freeunit reload
  assert_success

  run curl -sf "https://${PROJNAME}.ddev.site/"
  assert_success
  assert_output --partial "freeunit-test-ok"
}

teardown() {
  set -eu -o pipefail
  ddev delete -Oy "${PROJNAME}" >/dev/null 2>&1
  if [ -n "${GITHUB_ENV:-}" ]; then
    [ -e "${GITHUB_ENV:-}" ] && echo "TESTDIR=${HOME}/tmp/${PROJNAME}" >> "${GITHUB_ENV}"
  else
    [ "${TESTDIR}" != "" ] && rm -rf "${TESTDIR}"
  fi
}

@test "install from directory" {
  set -eu -o pipefail
  echo "# ddev add-on get ${DIR} with project ${PROJNAME} in $(pwd)" >&3
  run ddev add-on get "${DIR}"
  assert_success
  run ddev restart -y
  assert_success
  health_checks
}

# bats test_tags=release
@test "install from release" {
  set -eu -o pipefail
  echo "# ddev add-on get ${GITHUB_REPO} with project ${PROJNAME} in $(pwd)" >&3
  run ddev add-on get "${GITHUB_REPO}"
  assert_success
  run ddev restart -y
  assert_success
  health_checks
}

@test "remove the add-on" {
  set -eu -o pipefail
  run ddev add-on get "${DIR}"
  assert_success
  run ddev restart -y
  assert_success

  run ddev add-on remove freeunit
  assert_success

  run ddev restart -y
  assert_success

  # Back on the default webserver, the plain PHP page still works.
  run curl -sf "https://${PROJNAME}.ddev.site/"
  assert_success
  assert_output --partial "freeunit-test-ok"
}
