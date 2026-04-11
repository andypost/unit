#!/usr/bin/env bash
# run-local.sh — runs FreeUnit tests inside a Docker container
# mirrors pkg/docker/template.Dockerfile for build process
#
# Usage:
#   ./run-local.sh [OPTIONS] [MODULE...]
#
# Options:
#   -m MODULE   Module to test (unit, python, php, go, java, node, perl,
#               ruby, wasm, wasm-wasi-component)
#               Default: unit (runs full test suite)
#   -t TEST     Specific test file or test function to run
#               Examples: test_tls.py  test_tls.py::test_tls_certificate_change
#   -v VERSION  Python version for tests (default: 3.12)
#   -n          Dry-run — print commands, do not execute
#   -h          Show this help
#
# Examples:
#   ./run-local.sh                          # full test suite
#   ./run-local.sh python                   # Python tests only
#   ./run-local.sh php                      # PHP tests only
#   ./run-local.sh -t test_tls.py           # single test file
#   ./run-local.sh -t test_tls.py::test_tls_certificate_change  # single test
#   ./run-local.sh unit python php          # multiple modules
#
# To force rebuild: docker rmi freeunit-test:local

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="freeunit-test:local"
DRY_RUN=false

# ---------------------------------------------------------------------------
# Known modules and their test paths
# ---------------------------------------------------------------------------
declare -A MODULE_TESTS=(
    [unit]="test"
    [python]="test"
    [go]="test/test_go*"
    [java]="test/test_java*"
    [node]="test/test_node*"
    [perl]="test/test_perl*"
    [php]="test/test_php*"
    [ruby]="test/test_ruby*"
    [wasm]="test/test_wasm*"
    [wasm-wasi-component]="test/test_wasm-wasi-component*"
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { echo "[$(date '+%H:%M:%S')] $*"; }
info() { log "INFO  $*"; }
warn() { log "WARN  $*"; }
err()  { log "ERROR $*" >&2; }

usage() {
    sed -n '/^# Usage:/,/^[^#]/{ /^#/s/^# \{0,3\}//p }' "$0"
    exit 0
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
MODULES=()
TEST_PATH=""

while getopts ":m:t:v:nh" opt; do
    case $opt in
        m) MODULES+=("$OPTARG") ;;
        t) TEST_PATH="$OPTARG" ;;
        v) PYTHON_VERSION="$OPTARG" ;;
        n) DRY_RUN=true ;;
        h) usage ;;
        :) err "Option -$OPTARG requires an argument."; exit 1 ;;
       \?) err "Unknown option: -$OPTARG"; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

MODULES+=("$@")

if [[ ${#MODULES[@]} -eq 0 ]] && [[ -z "$TEST_PATH" ]]; then
    TEST_PATH="test"
    MODULES=("unit")
fi

if [[ -z "$TEST_PATH" ]] && [[ ${#MODULES[@]} -gt 0 ]]; then
    PATHS=()
    for m in "${MODULES[@]}"; do
        if [[ -n "${MODULE_TESTS[$m]:-}" ]]; then
            PATHS+=("${MODULE_TESTS[$m]}")
        else
            err "Unknown module: '$m'. Known: ${!MODULE_TESTS[*]}"
            exit 1
        fi
    done
    TEST_PATH="${PATHS[*]}"
fi

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------
if ! command -v docker &>/dev/null; then
    err "docker not found in PATH"; exit 1
fi

info "============================================================"
info "FreeUnit local test run"
info "  Modules  : ${MODULES[*]:-all}"
info "  Test path: ${TEST_PATH}"
info "  Dry-run  : ${DRY_RUN}"
info "============================================================"

# ---------------------------------------------------------------------------
# Build the test image if it doesn't exist
# ---------------------------------------------------------------------------
build_image() {
    if docker image inspect "$IMAGE_NAME" &>/dev/null; then
        info "Using cached image: $IMAGE_NAME"
        return 0
    fi

    info "Building test image: $IMAGE_NAME"

    # Generate a temporary Dockerfile that mirrors template.Dockerfile.
    # njs is built via `make -C pkg/contrib .njs` (same as production
    # Dockerfiles).  Source code is mounted via volume at runtime.
    local DOCKERFILE
    DOCKERFILE="$(mktemp /tmp/Dockerfile.test.XXXXXX)"

    cat > "$DOCKERFILE" <<'EOF'
FROM python:3.14-slim-trixie

LABEL org.opencontainers.image.title="FreeUnit (test)"
LABEL org.opencontainers.image.vendor="FreeUnit Community <team@freeunit.org>"

ENV DEBIAN_FRONTEND=noninteractive

# System deps + Rust (always required for otel/unitctl)
RUN set -ex \
    && savedAptMark="$(apt-mark showmanual)" \
    && apt-get update \
    && apt-get install --no-install-recommends --no-install-suggests -y \
         ca-certificates git build-essential libssl-dev openssl libpcre2-dev \
         zlib1g-dev libzstd-dev libbrotli-dev curl wget pkg-config pkgconf \
         libclang-dev cmake python3-pytest python3-openssl sudo procps \
    && export RUST_VERSION=1.94.1 \
    && export RUSTUP_HOME=/usr/src/unit/rustup \
    && export CARGO_HOME=/usr/src/unit/cargo \
    && export PATH=/usr/src/unit/cargo/bin:$PATH \
    && dpkgArch="$(dpkg --print-architecture)" \
    && case "${dpkgArch##*-}" in \
         amd64) rustArch="x86_64-unknown-linux-gnu"; rustupSha256="6aeece6993e902708983b209d04c0d1dbb14ebb405ddb87def578d41f920f56d" ;; \
         arm64) rustArch="aarch64-unknown-linux-gnu"; rustupSha256="1cffbf51e63e634c746f741de50649bbbcbd9dbe1de363c9ecef64e278dba2b2" ;; \
         *) echo >&2 "unsupported architecture: ${dpkgArch}"; exit 1 ;; \
       esac \
    && url="https://static.rust-lang.org/rustup/archive/1.27.1/${rustArch}/rustup-init" \
    && curl -L -O "$url" \
    && echo "${rustupSha256} *rustup-init" | sha256sum -c - \
    && chmod +x rustup-init \
    && ./rustup-init -y --no-modify-path --profile minimal --default-toolchain $RUST_VERSION --default-host ${rustArch} \
    && rm rustup-init \
    && rustup --version && cargo --version && rustc --version \
    && mkdir -p /usr/lib/unit/modules /usr/lib/unit/debug-modules

WORKDIR /unit

# Build entrypoint — source mounted via -v, built at runtime
ENTRYPOINT ["bash", "-c", "\
    set -ex && \
    NCPU=$(getconf _NPROCESSORS_ONLN) && \
    DEB_HOST_MULTIARCH=$(dpkg-architecture -q DEB_HOST_MULTIARCH) && \
    CC_OPT=$(DEB_BUILD_MAINT_OPTIONS='hardening=+all,-pie' DEB_CFLAGS_MAINT_APPEND='-fPIC' dpkg-buildflags --get CFLAGS) && \
    LD_OPT=$(DEB_BUILD_MAINT_OPTIONS='hardening=+all,-pie' DEB_LDFLAGS_MAINT_APPEND='-Wl,--as-needed -pie' dpkg-buildflags --get LDFLAGS) && \
    CONFIGURE_ARGS='--prefix=/usr \
                --statedir=/var/lib/unit \
                --control=unix:/var/run/control.unit.sock \
                --runstatedir=/var/run \
                --pid=/var/run/unit.pid \
                --logdir=/var/log \
                --log=/var/log/unit.log \
                --tmpdir=/var/tmp \
                --user=unit \
                --group=unit \
                --openssl \
                --njs \
                --otel \
                --zlib \
                --zstd \
                --brotli \
                --libdir=/usr/lib/$DEB_HOST_MULTIARCH' && \
    make -j $NCPU -C pkg/contrib .njs && \
    export PKG_CONFIG_PATH=$(pwd)/pkg/contrib/njs/build && \
    ./configure $CONFIGURE_ARGS \
        --cc-opt=\"$CC_OPT\" \
        --ld-opt=\"$LD_OPT\" \
        --tests \
        --modulesdir=/usr/lib/unit/debug-modules \
        --debug && \
    make -j $NCPU unitd -k || make unitd && \
    ./configure python --config=/usr/local/bin/python3-config && \
    make python3 && \
    chmod -R +x /unit && \
    sudo -E pytest-3 --print-log ${TEST_PATH:-test} \
"]
EOF

    if $DRY_RUN; then
        info "Dry-run: would build with:"
        cat "$DOCKERFILE"
        rm -f "$DOCKERFILE"
        return 0
    fi

    docker build --file "$DOCKERFILE" --tag "$IMAGE_NAME" "$PROJECT_DIR"
    rm -f "$DOCKERFILE"
}

# ---------------------------------------------------------------------------
# Run tests
# ---------------------------------------------------------------------------
run_tests() {
    info "Running tests: ${TEST_PATH}"

    if $DRY_RUN; then
        info "Dry-run: would execute:"
        echo "  docker run --rm --privileged -v ${PROJECT_DIR}:/unit -w /unit -e TEST_PATH='${TEST_PATH}' ${IMAGE_NAME}"
        return 0
    fi

    docker run --rm --privileged \
        -v "${PROJECT_DIR}:/unit" \
        -w /unit \
        -e TEST_PATH="${TEST_PATH}" \
        "${IMAGE_NAME}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
build_image
run_tests
