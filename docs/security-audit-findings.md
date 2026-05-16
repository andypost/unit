# Security Audit Findings

This document captures follow-up findings from a targeted manual security review of
recent security-audit changes and adjacent Unit code paths. These items are
**potential vulnerabilities** until each has a reproducer, impact analysis, and
fix or explicit dismissal.

## Scope

Reviewed areas:

- Configuration validation and process isolation paths.
- Static file serving controls related to chroot, symlinks, and mount traversal.
- GitHub Actions and Docker release-image supply-chain paths.
- New Trivy audit workflow and report-generation script.

## Candidate vulnerabilities

| ID | Severity | Confidence | Area | Summary | Primary evidence | Suggested next step |
| --- | --- | --- | --- | --- | --- | --- |
| SA-001 | High | Medium | Process isolation | `rootfs` accepts length-tracked config strings but later stores and uses them as NUL-terminated C strings. Embedded NUL bytes could truncate the effective root path passed to `chroot()`/`pivot_root()` and related mount setup. | `rootfs` is declared as a plain string without a NUL-byte validator in `src/nxt_conf_validation.c:1352-1356`; `nxt_isolation_set_rootfs()` copies the configured bytes and appends `\0` in `src/nxt_isolation.c:501-525`; the path is later used as a C string in `src/nxt_isolation.c:817-825`. | Add a rootfs validator that rejects empty, non-absolute, `/`, embedded-NUL, and path-normalization edge cases; add regression tests mirroring the cgroup embedded-NUL test. |
| SA-002 | Medium | Medium | Configuration validation | Several application path-like fields are validated only as strings even though downstream code commonly treats them as filesystem paths, module names, or C strings. This may create truncation, path confusion, or audit-bypass behavior for embedded NUL input. | Common fields such as `working_directory`, `stdout`, and `stderr` are plain strings in `src/nxt_conf_validation.c:1274-1291`; language-specific path/module fields are also plain strings, e.g. Ruby `script` and `hooks` in `src/nxt_conf_validation.c:1134-1148`, Java `webapp` and `unit_jars` in `src/nxt_conf_validation.c:1152-1170`, and Wasm module/component names in `src/nxt_conf_validation.c:1184-1239`. | Introduce reusable validators for C-string-backed paths and module identifiers, then apply them consistently to all fields consumed by C APIs. |
| SA-003 | Medium | Low | User namespace isolation | `uidmap`/`gidmap` entries require integer fields but do not appear to enforce non-negative values, kernel UID/GID-map bounds, overlap checks, or a maximum number of entries during configuration validation. Invalid or pathological maps could produce confusing authorization decisions or resource-exhaustion behavior before kernel rejection. | Required integer fields are defined in `src/nxt_conf_validation.c:1470-1485`; arrays are copied with `map->size * sizeof(nxt_clone_map_entry_t)` in `src/nxt_isolation.c:346-362`; map output is sized around 32-bit decimal entries in `src/nxt_clone.c:128-147`. | Add validation for non-negative 32-bit IDs, positive sizes, no arithmetic overflow, no overlaps, and a practical entry-count limit. Add tests for negative, oversized, overlapping, and excessive maps. |
| SA-004 | Medium | Medium | Release supply chain | Docker images clone source by mutable branch/tag name inside the Docker build rather than using the already-checked-out commit or verifying a commit SHA. A moved tag or manual workflow input could build and publish an image from code different from the workflow revision under review. | `pkg/docker/template.Dockerfile:38` runs `git clone --depth 1 -b @@VERSION@@ https://github.com/freeunitorg/freeunit unit`; `.github/workflows/docker.yml:131-147` derives a version string and rewrites Dockerfiles before publishing. | Build from the checked-out workspace, or pass and verify an immutable commit SHA; record the source revision in image labels and provenance. |
| SA-005 | Medium | Medium | CI/CD supply chain | Security-sensitive workflows use mutable action tags while granting permissions such as `security-events: write`. A compromised or retagged third-party action could affect audit integrity or code-scanning uploads. | The new audit workflow grants `security-events: write` in `.github/workflows/security-audit.yml:43-45` and uses version tags for actions in `.github/workflows/security-audit.yml:53-104`; the Docker publisher similarly uses mutable action tags in `.github/workflows/docker.yml:127-160`. | Pin third-party actions to full commit SHAs, enable Dependabot updates for GitHub Actions SHAs, and keep write permissions scoped to the minimal job/step that needs them. |
| SA-006 | Low | Medium | HTTP compatibility/security hardening | `discard_unsafe_fields` can be disabled, allowing non-token header names such as underscores and punctuation to reach applications. This is intentional configurability, but can reintroduce header-smuggling or auth-bypass risk behind intermediaries with different header-name normalization. | The parser skips unsafe fields only when `discard_unsafe_fields` is enabled in `src/nxt_http_parse.c:539-550`; tests confirm unsafe names are passed through when disabled in `test/test_http_header.py:484-490`. | Document this as a dangerous compatibility mode, consider warning on disable, and add proxy/intermediary threat-model notes. |

## Recently addressed during this audit thread

| ID | Status | Summary | Evidence |
| --- | --- | --- | --- |
| SA-FIXED-001 | Fixed in `b928b85` | Cgroup isolation path validation now rejects embedded NUL bytes before constructing a slash-wrapped validation path. | `src/nxt_conf_validation.c:3367-3375` and `test/test_python_isolation.py:225-228`. |

## Recommended prioritization

1. Fix SA-001 first because it is adjacent to privileged isolation setup and is
   structurally similar to the cgroup NUL issue already fixed.
2. Triage SA-002 by enumerating every config string that is later passed to a C
   API expecting NUL termination.
3. Harden CI/CD supply-chain items SA-004 and SA-005 before relying on the new
   audit workflow as a release gate.
4. Treat SA-003 and SA-006 as defense-in-depth unless a concrete reproducer
   shows direct privilege escalation or request smuggling.
