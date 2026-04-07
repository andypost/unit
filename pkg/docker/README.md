# FreeUnit Docker images

This directory contains Dockerfiles for all FreeUnit language variants and
a helper script (`build-local.sh`) for building them locally — mirroring
the behavior of `.github/workflows/docker.yml`.

## Prerequisites (Ubuntu 24.04 LTS)

### Docker Engine

```bash
# Remove any old Docker packages
sudo apt-get remove -y docker.io docker-doc docker-compose \
    docker-compose-v2 podman-docker containerd runc 2>/dev/null || true

# Add Docker's official apt repository
sudo apt-get update
sudo apt-get install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
    -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
  https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io \
    docker-buildx-plugin docker-compose-plugin

# Allow running docker without sudo (re-login required)
sudo usermod -aG docker "$USER"
```

### GNU parallel (optional, for `-j N` parallel builds)

```bash
sudo apt-get install -y parallel
```

### Verify installation

```bash
docker version
docker buildx version
```

## Building images locally

```bash
cd pkg/docker
```

| Command | Description |
|---------|-------------|
| `./build-local.sh` | Build **all** variants sequentially |
| `./build-local.sh minimal php8.5` | Build only the listed variants |
| `./build-local.sh -j4` | Build 4 variants in parallel (requires `parallel`) |
| `./build-local.sh -v 1.35.2` | Pin a specific FreeUnit version |
| `./build-local.sh -p linux/arm64` | Build for a specific platform |
| `./build-local.sh -p linux/amd64,linux/arm64 -j2` | Multi-arch build (requires buildx) |
| `./build-local.sh -n` | Dry-run — print commands without executing |

### Options

```
-v VERSION   FreeUnit version string to pin (default: current git branch name)
-p PLATFORM  Target platform (default: host arch, e.g. linux/amd64)
-j N         Number of parallel builds (default: 1)
-n           Dry-run — print commands, do not execute
-h           Show help
```

### Logs

Each build writes a log to `pkg/docker/build-logs/<variant>.log`.
A summary (OK / FAILED) is printed at the end.

## Available variants

| Variant | Base image |
|---------|-----------|
| `minimal` | debian:trixie-slim |
| `wasm` | debian:trixie-slim |
| `go1.24` | golang:1.24 |
| `go1.25` | golang:1.25 |
| `jsc17` | eclipse-temurin:17-jdk-noble |
| `jsc21` | eclipse-temurin:21-jdk-noble |
| `node20` | node:20 |
| `node22` | node:22 |
| `perl5.38` | perl:5.38 |
| `perl5.40` | perl:5.40 |
| `php8.3` | php:8.3-cli |
| `php8.4` | php:8.4-cli |
| `php8.5` | php:8.5-cli-trixie |
| `python3.12` | python:3.12 |
| `python3.12-slim` | python:3.12-slim |
| `python3.13` | python:3.13 |
| `python3.13-slim` | python:3.13-slim |
| `python3.14` | python:3.14 |
| `python3.14-slim` | python:3.14-slim |
| `ruby3.3` | ruby:3.3 |
| `ruby3.4` | ruby:3.4 |

## CI workflow

The GitHub Actions workflow (`.github/workflows/docker.yml`) builds and pushes
images to GHCR (`ghcr.io/freeunitorg/freeunit`) on every `v*` release tag or
via `workflow_dispatch`. It produces:

- Per-arch tags: `VERSION-VARIANT-amd64`, `VERSION-VARIANT-arm64`
- Multi-arch manifest: `VERSION-VARIANT`, `latest-VARIANT`
