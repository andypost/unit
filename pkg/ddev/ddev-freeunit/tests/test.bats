#!/usr/bin/env bats

# Bats is a testing framework for Bash
# Documentation https://bats-core.readthedocs.io/en/stable/
# Bats libraries documentation https://github.com/ztombol/bats-docs
#
# For local tests, install bats-core, bats-assert, bats-file, bats-support
# And run this in the add-on root directory:
#   bats ./tests/test.bats
# To exclude release tests:
#   bats ./tests/test.bats --filter-tags '!release'
# For debugging:
#   bats ./tests/test.bats --show-output-of-passing-tests --verbose-run --print-output-on-failure

setup() {
  set -eu -o pipefail

  # The release test can only pass once the add-on lives in this repository.
  export GITHUB_REPO=freeunitorg/ddev-freeunit

  TEST_BREW_PREFIX="$(brew --prefix 2>/dev/null || true)"
  export BATS_LIB_PATH="${BATS_LIB_PATH}:${TEST_BREW_PREFIX}/lib:/usr/lib/bats"
  bats_load_library bats-assert
  bats_load_library bats-file
  bats_load_library bats-support

  export DIR="$(cd "$(dirname "${BATS_TEST_FILENAME}")/.." >/dev/null 2>&1 && pwd)"
  export PROJNAME="test-$(basename "${GITHUB_REPO}")"
  mkdir -p "${HOME}/tmp"
  export TESTDIR="$(mktemp -d "${HOME}/tmp/${PROJNAME}.XXXXXX")"
  export DDEV_NONINTERACTIVE=true
  export DDEV_NO_INSTRUMENTATION=true
  ddev delete -Oy "${PROJNAME}" >/dev/null 2>&1 || true
  cd "${TESTDIR}"

  # A plain "php" project exercises the generic routing: front controller,
  # a second PHP script, a static file, a dotfile, HTTPS detection, Xdebug.
  mkdir -p web
  cat <<'PHP' > web/index.php
<?php
echo "freeunit-test-ok\n";
echo "HTTPS=" . ($_SERVER['HTTPS'] ?? 'off') . "\n";
echo "xdebug=" . (extension_loaded('xdebug') ? 'on' : 'off') . "\n";
PHP
  echo '<?php echo "php-direct-ok\n";' > web/info.php
  echo "static-ok" > web/robots.txt
  echo "SECRET=1" > web/.env

  run ddev config --project-name="${PROJNAME}" --project-type=php --docroot=web --project-tld=ddev.site \
    ${FREEUNIT_TEST_PHP_VERSION:+--php-version="${FREEUNIT_TEST_PHP_VERSION}"}
  assert_success
}

# Install the add-on from ${1}; with FREEUNIT_SRC_TARBALL set (CI), build
# FreeUnit from that source tarball instead of the pinned release.
install_addon() {
  run ddev add-on get "$1"
  assert_success
  if [ -n "${FREEUNIT_SRC_TARBALL:-}" ]; then
    cp "${FREEUNIT_SRC_TARBALL}" .ddev/web-build/freeunit-src.tar.gz
  fi
}

# Wait for FreeUnit to answer after a (re)start.
wait_for_site() {
  for _ in $(seq 1 30); do
    curl -sf "https://${PROJNAME}.ddev.site/" >/dev/null 2>&1 && return 0
    sleep 1
  done
  return 1
}

health_checks() {
  wait_for_site

  # Front controller, through PHP, with HTTPS detected from X-Forwarded-Proto.
  run curl -sf "https://${PROJNAME}.ddev.site/"
  assert_success
  assert_output --partial "freeunit-test-ok"
  assert_output --partial "HTTPS=on"

  # Any other PHP script runs too instead of being served as a file.
  run curl -sf "https://${PROJNAME}.ddev.site/info.php"
  assert_success
  assert_output "php-direct-ok"

  # Static files are served directly.
  run curl -sf "https://${PROJNAME}.ddev.site/robots.txt"
  assert_success
  assert_output "static-ok"

  # Dotfiles are denied.
  run curl -s -o /dev/null -w "%{http_code}" "https://${PROJNAME}.ddev.site/.env"
  assert_output "404"

  # The response comes from FreeUnit, not nginx/apache.
  run curl -sfI "https://${PROJNAME}.ddev.site/"
  assert_success
  assert_output --regexp "^[Ss]erver: [Uu]nit"

  # The control command works.
  run ddev freeunit status
  assert_success
  assert_output --partial "connections"

  # Xdebug: DDEV's stock `ddev xdebug` edits the php-fpm configuration,
  # which the embed SAPI shares; a daemon restart loads it.
  run ddev xdebug on
  assert_success
  run ddev freeunit restart
  assert_success
  wait_for_site
  run curl -sf "https://${PROJNAME}.ddev.site/"
  assert_output --partial "xdebug=on"

  run ddev xdebug off
  assert_success
  run ddev freeunit restart
  assert_success
  wait_for_site
  run curl -sf "https://${PROJNAME}.ddev.site/"
  assert_output --partial "xdebug=off"
}

teardown() {
  set -eu -o pipefail
  ddev delete -Oy "${PROJNAME}" >/dev/null 2>&1
  # Persist TESTDIR if running inside GitHub Actions. Useful for uploading test result artifacts
  # See example at https://github.com/ddev/github-action-add-on-test#preserving-artifacts
  if [ -n "${GITHUB_ENV:-}" ]; then
    [ -e "${GITHUB_ENV:-}" ] && echo "TESTDIR=${HOME}/tmp/${PROJNAME}" >> "${GITHUB_ENV}"
  else
    [ "${TESTDIR}" != "" ] && rm -rf "${TESTDIR}"
  fi
}

@test "install from directory" {
  set -eu -o pipefail
  echo "# ddev add-on get ${DIR} with project ${PROJNAME} in $(pwd)" >&3
  install_addon "${DIR}"
  run ddev restart -y
  assert_success
  health_checks
}

# bats test_tags=release
@test "install from release" {
  set -eu -o pipefail
  echo "# ddev add-on get ${GITHUB_REPO} with project ${PROJNAME} in $(pwd)" >&3
  install_addon "${GITHUB_REPO}"
  run ddev restart -y
  assert_success
  health_checks
}

@test "remove the add-on" {
  set -eu -o pipefail
  install_addon "${DIR}"
  run ddev restart -y
  assert_success

  run ddev add-on remove freeunit
  assert_success
  assert_file_not_exists .ddev/config.freeunit.yaml
  assert_file_not_exists .ddev/web-build/Dockerfile.freeunit
  assert_file_not_exists .ddev/commands/web/freeunit

  # Back on nginx-fpm, the page still works.
  run ddev restart -y
  assert_success
  run curl -sf "https://${PROJNAME}.ddev.site/"
  assert_success
  assert_output --partial "freeunit-test-ok"
  run curl -sfI "https://${PROJNAME}.ddev.site/"
  refute_output --regexp "^[Ss]erver: [Uu]nit"
}
