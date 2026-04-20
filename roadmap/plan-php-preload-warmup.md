# PHP Preload / Warmup Hook — Implementation Plan

## Context

FreeUnit's PHP roadmap item **P3** (`roadmap/unit-php.md:72-77`) calls for a
first-class preload/warmup hook. Today users can get opcache preload only by
hand-writing a `php.ini` file and pointing `options.file` at it; there is no
way to eagerly compile a curated script list at worker startup. Frameworks
(Symfony, Laravel, Drupal) suffer a visible cold-start spike on the first
request after every reload because each worker's opcache starts empty.

This plan adds two new PHP config keys:

- `"preload": "/abs/path/preload.php"` — mapped automatically to PHP's native
  `opcache.preload`, injected **before** `php_module_startup` so the engine
  actually honors it.
- `"warmup": ["a.php", "b.php", …]` — eager `opcache_compile_file()` pass run
  after targets resolve but before the worker accepts requests. Compile-only,
  never execute.

Outcome: deterministic p99 on the first request after reload; ergonomic config
that removes the ini-file detour; no change to extension compatibility.

The work is co-designed with the `/run` + scheduler plan
(`roadmap/plan-run.md`) so shared scaffolding (validators, control API
dispatcher, config-reload generation bump, status surface) is reused — but the
on-demand `/control/applications/<name>/warmup` endpoint is **deliberately
deferred** to P7 to keep P3 landable in ~3 days.

---

## High-Level Approach

**Preload plumbing.** PHP reads `opcache.preload` during
`php_module_startup()`. `sapi_module_struct.ini_entries` is a string PHP
appends to the parsed ini at startup. We synthesize a small blob
(`"opcache.preload=<path>\nopcache.preload_user=<user>\n"`) inside
`nxt_php_setup()` **between** `sapi_startup()` (line 443) and `nxt_php_startup()`
(line 460), and stash it in `nxt_php_sapi_module.ini_entries` (currently `NULL`
at line 368). Path is resolved against `conf->working_directory` the same way
`nxt_php_set_ini_path()` already handles relative ini paths (lines 1050-1086).
`opcache.preload_user` defaults to `conf->user`; overridable via `options.admin`
if the user wants a different uid. If `conf->user` is empty, fall back to the
worker's effective uid. **If the resolved user would be root** (effective uid 0),
emit a loud WARN and skip the injection entirely — PHP would hard-fail at
startup otherwise. Addresses PR #4 gemini review c1.

**Warmup plumbing.** In `nxt_php_start()`, after targets resolve (line 735) and
before `nxt_unit_init()` call path (line 774), a new
`nxt_php_warmup_targets()` helper looks up `opcache_compile_file` via
`zend_hash_str_find_ptr(CG(function_table), "opcache_compile_file", ...)`. For
each entry in `c->warmup` we invoke it with a `zval` path argument, save/restore
cwd around the loop, soft-fail on each entry. Relative paths resolve against
the **first target's root** (matches `opcache.preload`'s own behavior); absolute
paths used verbatim. The `zend_try { … } zend_catch { … } zend_end_try();`
wraps **each loop iteration individually** — *not* the outer loop — so a
bailout from one `opcache_compile_file` call skips only the offending entry
and sibling entries still compile. Addresses PR #4 gemini review c2.

**Config reload** already forces fresh workers via `app->generation++`
(`src/nxt_router.c:917`). Preload/warmup automatically re-runs on each new
worker; no extra plumbing.

**Error policy.** Preload failure is PHP-native (module-startup FAILURE → worker
exits → Unit restarts); we add a log hint referencing `opcache.preload`.
Warmup failures are soft: missing file → WARN + skip; syntax error → catch via
`zend_try`/`zend_catch`, clear, WARN + skip; `opcache_compile_file` symbol
absent (opcache disabled) → one WARN, skip whole list.

---

## File-by-File Changes

### `src/nxt_application.h:62-65`
Extend `nxt_php_app_conf_t`:
```
typedef struct {
    nxt_conf_value_t  *targets;
    nxt_conf_value_t  *options;
    nxt_conf_value_t  *preload;   /* string */
    nxt_conf_value_t  *warmup;    /* array of strings */
} nxt_php_app_conf_t;
```

### `src/nxt_main_process.c:236-248`
Add two entries to `nxt_php_app_conf[]`:
```
{ nxt_string("preload"), NXT_CONF_MAP_PTR,
  offsetof(nxt_common_app_conf_t, u.php.preload) },
{ nxt_string("warmup"),  NXT_CONF_MAP_PTR,
  offsetof(nxt_common_app_conf_t, u.php.warmup) },
```

### `src/nxt_conf_validation.c`
- **Line 1049** (`nxt_conf_vldt_php_common_members[]`): add two members before
  `NXT_CONF_VLDT_NEXT`. Placing them in `_common_members` means both the
  targets and notargets tables inherit them (no duplication).
  - `preload` → `NXT_CONF_VLDT_STRING`, validator
    `nxt_conf_vldt_php_preload_path` (new): rejects empty string and embedded
    null byte.
  - `warmup` → `NXT_CONF_VLDT_ARRAY`, validator `nxt_conf_vldt_array_iterator`
    (existing at line 3238) with element handler `nxt_conf_vldt_php_warmup`
    (new): copy the pattern of `nxt_conf_vldt_java_classpath` at lines
    3499-3518 — rejects non-string, rejects null byte, s/classpath/warmup/.

### `src/nxt_php_sapi.c`
Three insertion sites:

1. **Between lines 443 and 460** in `nxt_php_setup()`, guarded by
   `#if (PHP_VERSION_ID >= 70400)`: call a new
   `nxt_php_build_ini_entries(task, c, conf->working_directory, &conf->user)`
   that mallocs the `opcache.preload=…\nopcache.preload_user=…\n` blob if
   `c->preload` is non-null, assigns to `nxt_php_sapi_module.ini_entries`.
   **Ownership**: the SAPI consumes the pointer read-only during
   `php_init_config()`; PHP never frees it. We keep a module-scope static
   `u_char *nxt_php_ini_entries_buf` so `nxt_php_cleanup_targets()` can free
   it **after** `php_module_shutdown()`, matching existing teardown order
   (see `nxt_php_cleanup_targets()` at line 1014). On PHP < 7.4 the block
   compiles out; `preload` key validated-but-no-op + one INFO log.

2. **Between lines 735 and the `nxt_unit_default_init` call** in
   `nxt_php_start()`: invoke `nxt_php_warmup_targets(task, c)`. Returns void
   (never fatal). Forward-declared near line 85. **Per-iteration**
   `zend_try { … } zend_catch { … } zend_end_try();` around each
   `opcache_compile_file()` call — *not* wrapping the outer loop — so one
   bailout only skips the failing entry, siblings still compile. See PR #4
   gemini review c2.

3. **Line 1043** (`nxt_php_cleanup_targets()`): `nxt_free` the static
   `nxt_php_ini_entries_buf` and reset `nxt_php_sapi_module.ini_entries = NULL`
   so a subsequent setup in the same process (TrueAsync re-init path) starts
   clean.

### `auto/modules/php`
No changes. `opcache_compile_file` is resolved dynamically from the Zend
function table; if opcache is disabled we degrade gracefully. The
`ini_entries` SAPI field exists in every supported PHP (5.4+).

### New test files and fixtures
- `test/test_php_preload.py` (new)
- `test/test_php_warmup.py` (new)
- `test/php/preload/` (new): `hello_preload.php` (defines constant), root
  `index.php` that reports `defined('PRELOAD_OK')`.
- `test/php/warmup/` (new): `a.php`, `b.php`, `c.php` trivial;
  `broken.php` with a syntax error; `large/` with 200 tiny scripts; root
  `index.php` reporting `opcache_is_script_cached($_GET['f'])` so tests can
  assert warmup ran **before any execution** of the file.

Existing helpers reused: `client.load`, `client.conf`, `check_opcache` and
`set_opcache` from `test/test_php_application.py:20-93`.

---

## Config Schema Example

```json
{
  "applications": {
    "hot": {
      "type": "php",
      "root": "/var/www/hot",
      "preload": "/var/www/hot/bootstrap/preload.php",
      "warmup": [
        "/var/www/hot/src/Kernel.php",
        "/var/www/hot/src/Router.php",
        "vendor/autoload.php"
      ],
      "options": {
        "admin": { "opcache.enable": "1", "opcache.memory_consumption": "256" }
      }
    }
  }
}
```

---

## Error Policy Summary

| Failure | Behavior |
|---|---|
| `preload` path missing/unreadable | PHP module-startup FAILURE → worker exit → Unit restart (existing path). Alert references `opcache.preload`. |
| `preload` relative, no workdir | Validation error. |
| `options.file` also sets `opcache.preload` | **Unit `preload` key wins** — matches PHP SAPI semantics (`ini_entries` parsed after `php.ini`, so it overrides) and Unit convention (config-key-wins-over-file for `user`/`group`/etc). INFO log on detected conflict. Addresses PR #4 gemini review c3. |
| `preload` resolves to user=root | WARN and skip the injection — worker stays up without preload rather than crashing on PHP startup. |
| `warmup` item missing / syntax error | WARN + skip. Worker stays up. |
| `opcache_compile_file` symbol absent | Single WARN, skip entire warmup. |
| Warmup wall time > 5s | WARN (not an error). |

---

## Test Matrix (extensive — this is the bulk of the work)

### Validation (`test_php_preload.py`, `test_php_warmup.py`)
- `preload`: integer, object, boolean, empty string, null byte → 4xx.
- `warmup`: string-not-array, array-of-ints, element null byte → 4xx.
- `warmup`: empty array accepted.

### Preload behavior
- **Happy path**: `hello_preload.php` defines `PRELOAD_OK`; index echoes
  `defined('PRELOAD_OK')`; first request returns `true` — proves preload ran
  before any request.
- **Missing preload file**: apply config succeeds; first request returns 503;
  unit log grep for `opcache.preload`.
- **Relative preload rejected**: no `/` prefix and no workdir → validation
  error.
- **Conflict with `options.file`**: ini file sets `opcache.preload=A`, key
  sets `preload=B`; assert A wins; assert INFO log line.

### Warmup behavior
- **All compile**: 3 files; index probes `opcache_is_script_cached(...)` on
  first request → all `true`.
- **One missing**: `a.php`, `MISSING.php`, `c.php`; a+c cached; MISSING logged;
  worker alive.
- **Syntax error**: `broken.php` logged; siblings cached; worker alive.
- **Opcache disabled** (`opcache.enable=0`): warmup no-ops; single WARN; app
  serves normally.
- **Targets interaction**: two targets with distinct roots; relative entry
  resolves against first target only — documented and asserted.
- **Reload picks up new list**: apply `warmup=[a]`, hit, apply `warmup=[a,b]`,
  assert fresh worker (generation bump) has both cached on first request.
- **Visibility at t=0**: `opcache_get_status()['opcache_statistics']['num_cached_scripts']`
  > 0 on the first request before user code runs.

### Combined
- Preload + warmup together; both observable on first request.

### Edges
- Unicode path.
- Symlink chain (2 hops).
- 200-script warmup (assert wall < 5s).
- Duplicate entries (idempotent).
- `max_requests: 1` churn (every new worker re-warms).

---

## Shared with Scheduler / `/run` (from `roadmap/plan-run.md`)

| Primitive | Status in P3 |
|---|---|
| Array-of-string validator helper (`nxt_conf_vldt_array_iterator`, `*_java_classpath` pattern at `src/nxt_conf_validation.c:3499-3518`) | **Reused** for `warmup`. |
| Config reload → `app->generation++` at `src/nxt_router.c:917` | **Reused** — no new code needed. |
| Control API dispatcher at `src/nxt_controller.c` around line 2294 (`/restart` pattern) | **Extension point documented** (future `/control/applications/<name>/warmup`); not built here. |
| Port RPC (`nxt_port_rpc_register_handler`) and a paired `NXT_PORT_MSG_APP_WARMUP`/`WARMUP_DONE` shape | **Deferred** to P7 alongside `RUN_TASK`/`RUN_DONE`. Three-line comment in `nxt_php_warmup_targets()` points at the shape. |
| Status counters (`num_warmup_ok`, `num_warmup_fail`) on `nxt_status_app_t` | **Deferred** to P2 status surface. |

Explicit non-goal for P3: no new control endpoints, no new port messages, no
new status fields. Ship config-driven only.

---

## Risks & Mitigations

- **`ini_entries` timing regression across PHP versions.** Mitigation:
  post-`nxt_php_startup` assertion that `zend_ini_get_value("opcache.preload")`
  matches what we wrote; log ERR on mismatch.
- **Preload-as-root refusal.** Default `opcache.preload_user` to `conf->user`
  at inject time rather than rely on runtime uid.
- **CWD leaks from warmup into request handling.** Save/restore via
  `getcwd`/`chdir` around the warmup loop (same pattern tests in
  `test/test_php_application.py:37-83` already exercise).
- **Forgetting to list new fields in both targets + notargets tables.** Added
  only to `_common_members` (inherited by both).
- **Static-opcache distro builds.** Function table lookup still works; builds
  *without* opcache entirely: warmup no-ops cleanly (covered by test).

---

## Phased Ship (if 3 days compress to 1)

- **Day-1 minimum**: struct fields, schema validation, `ini_entries` preload
  injection, one happy-path preload test, one negative validation test.
  Delivers the documented `opcache.preload` ergonomic win.
- **Day-2 adds**: warmup loop, soft failure handling, fixture tree, five
  warmup tests.
- **Day-3 adds**: `options.file` conflict detection, reload test, large-list
  stress, unicode/symlink edges, user-facing doc page.
- **Cut line if 1 day only**: ship preload; stub `warmup` as
  validated-but-no-op + TODO. Preload alone covers ~80% of user value.

---

## Critical Files

- `src/nxt_php_sapi.c` (lines 313-487, 690-780, 1014-1045)
- `src/nxt_application.h:62-65`
- `src/nxt_main_process.c:236-248`
- `src/nxt_conf_validation.c:1049, 3238, 3499-3518`
- `test/test_php_application.py:20-108` (reference for helpers)
- New: `test/test_php_preload.py`, `test/test_php_warmup.py`,
  `test/php/preload/`, `test/php/warmup/`
- `roadmap/unit-php.md:72-77` (roadmap entry to mark shipped)

---

## Verification

End-to-end before opening a PR:

1. Build against PHP 8.3+ ZTS and NTS: `./configure php --module=php && make`.
2. Unit run on branch `claude/php-preload-warmup-EA2PX`:
   ```
   pytest test/test_php_preload.py test/test_php_warmup.py -v
   ```
3. Regression: existing preload tests still pass:
   ```
   pytest test/test_php_application.py -k "preload or opcache" -v
   ```
4. Manual smoke: configure the example schema above on a Symfony app, reload,
   hit `/`, confirm first-request latency matches subsequent requests within
   ±10% (was 2-5× pre-change).
5. Config validation: `PUT /config` with each negative case from §Test Matrix
   §Validation expects 4xx with a helpful message.
6. Roadmap hygiene: flip P3 row in `roadmap/unit-php.md:152` from "Near" to
   "Shipped" once merged.

---

## Pedantic Review — Gaps to Close Before Implementation

Second-pass review of the plan against the actual code surfaces. Everything
below is additive to the sections above.

### Documentation surfaces the first pass missed

| Surface | Current state | Update needed |
|---|---|---|
| `docs/unit-openapi.yaml:6887-6959` (`configApplicationPHP` schema) | Defines `root`, `index`, `options`, `script`, `targets` only | Add `preload` (string, description + example) and `warmup` (array of strings, description + example). Public API contract — **mandatory**. |
| `docs/changes.xml` | Release-note source for `CHANGES` | Add one `<change type="feature">` under the current unreleased entry describing `preload`/`warmup` keys. |
| `CHANGES` (top of file, under `FreeUnit 1.35.4 xx xxx 2026`) | Human-readable changelog | Mirror the `changes.xml` entry. |
| `roadmap/unit-php.md:41-42` (§Known gaps item 3) | "Opcache not shared across processes. … preloading is possible but not primed by Unit." | Replace last sentence with "Unit primes opcache.preload via the `preload` config key." Keep the "not shared across processes" half. |
| `roadmap/unit-php.md:152` (short roadmap table, P3 row) | "Near" | "Shipped" once merged. |
| `roadmap/unit-todos.md` | Tracks cross-refs | Add entry pointing at this plan and to the resulting test files. |
| `docs/unit-openapi.yaml` status examples (~line 6173) | `applications.wp.processes`/`requests` only | No change unless P2 status API is co-shipped; explicitly **not** in P3 scope. |

No user-facing man pages under `docs/man/man8/` currently document PHP config
keys — no work there.

### Test harness specifics the plan didn't pin down

- **Log assertions** must use the existing helpers in `test/unit/log.py`:
  - `Log.findall(r'pattern', ...)` at line 56 for substring checks (WARN
    lines).
  - `Log.wait_for_record(r'pattern', wait=150)` at line 103 for
    await-then-assert flows (preload-user default, missing-file WARN).
  - `Log.check_alerts()` at line 27 runs globally at teardown — tests that
    intentionally provoke alerts (missing preload file) must register an
    exclusion via `option.skip_alerts` (see conftest).
- **Opcache skip guard**: there is no `features.opcache` flag in
  `test/unit/check/`; reuse the `X-OPcache: -1` sentinel from
  `test/php/opcache/index.php:12` via the existing `check_opcache()` helper at
  `test/test_php_application.py:26` rather than a prerequisites block.
- **Reusable fixtures** to share, not rewrite: `set_opcache(app, val)` at
  `test/test_php_application.py:86`, `check_opcache()` at line 26. Factor the
  two new test files around these.
- **Log-assertion caveat**: the preload-fatal test expects an alert line; add
  its regex to the test's local `skip_alerts` list so the global teardown
  doesn't fail the suite.

### Missing tests to add to the matrix

**Preload edges**
- Preload script declares class + function + constant — all three visible on
  first request (covers full symbol-table preload contract).
- Preload script itself calls `opcache_compile_file()` (mirrors existing
  `test/php/opcache/preload/chdir.php` behavior) — must continue to work.
- Preload script calls `fastcgi_finish_request()` (covers
  `test/php/opcache/preload/fastcgi_finish_request.php` pattern, plus
  `docs/changes.xml:1320` regression).
- Compile-time syntax error in the preload file — PHP startup FAILURE, alert
  matches `opcache.preload` + the PHP line number.
- `preload` set to non-absolute path, `working_directory` set → resolves
  relative to `working_directory` (symmetry with `nxt_php_set_ini_path`).
- `preload` configured alongside legacy `options.admin.opcache.preload` →
  legacy value wins; INFO log line. Same for `options.file` containing
  `opcache.preload=…`.
- `preload` + `options.file` with **other** opcache directives — both apply;
  preload still primes.
- Preload under `isolation.rootfs` — path must resolve **inside** the rootfs,
  not against the host tree. Assert a preload script living at
  `{rootfs}/app/preload.php` is reachable.
- Preload under `max_requests: 1` — every new worker re-preloads (assert via
  `X-Pid` rotation across N requests).
- Removing `preload` on config `PUT` — next-generation worker starts without
  preload (generation bump observable via `X-Pid` change at
  `test/php/opcache/index.php:5`).

**Warmup edges**
- ZTS build, `processes.max: 4`: every worker independently warms. Spray
  requests, collect distinct `X-Pid` values, each reports all files cached
  on its first request.
- Warmup entry not-readable by worker user (exists but permission-denied).
  Distinct WARN from "missing".
- Warmup entry is a directory, not a file. WARN, skip.
- Warmup entry larger than `PATH_MAX`. Validator rejects OR runtime WARNs —
  decide and test.
- Warmup ordering: `b.php` declares class used by `c.php`. Submit `[c, b]`
  and `[b, c]`; document that compile order matters (opcache links on
  first reference of a declared symbol) and assert the known-good order
  succeeds.
- Warmup script declares the same symbol as the preload script — PHP fatal
  during warmup compile; warmup must catch and soft-fail (zend_try/catch),
  worker stays alive.
- Warmup entry with BOM / CRLF line endings — compiles.
- Warmup entry with `..` traversal (`../../etc/passwd.php`). Policy: allow
  (absolute resolve) but document; no special handling — test for no
  surprises.
- Warmup entry under a target-scoped rootfs — resolve against that target's
  root, not the app's top-level. (Reaffirms first-target rule under
  isolation.)
- Warmup with `opcache.enable_cli=0` but `opcache.enable=1` — decide
  whether warmup runs on CLI-mode SAPI instances; document result.
- Warmup atomic update: `PUT /config/applications/<name>/warmup` — field
  addressable directly (per OpenAPI spec at openapi.yaml:22).

**Cross-cutting**
- `Log.check_alerts()` sanity: full run of test matrix produces zero
  unexpected alerts.
- Fixture cleanup: warmup `large/` dir with 200 scripts is generated at
  test-setup time (conftest) not committed — avoid repo bloat.

### Plan-level clarifications

1. **`preload_user` when `conf->user` is empty.** Fall back to the current
   effective uid of the worker process (`getuid()` at warmup entry, same
   moment we build the ini blob). Validator does **not** reject empty
   `user`; plan needs this nuance spelled out.
2. **Rootfs path semantics.** Preload and warmup paths are always resolved
   **after** rootfs pivot — i.e., they are jail-local. Document in the
   changelog and OpenAPI description. Add one rootfs test in
   `test_php_preload.py` (guarded by `is_su` like existing
   `test/test_php_isolation.py:11`).
3. **Existing preload tests stay.** `test_php_application_opcache_preload_chdir`
   and `_ffr` at `test/test_php_application.py:879-898` verify the legacy
   `options.file` path. Do not move them. New tests go in
   `test_php_preload.py` / `test_php_warmup.py`.
4. **Addressable endpoints.** Because Unit exposes every config key as an
   endpoint (openapi.yaml:22), `PUT /config/applications/<name>/preload` and
   `DELETE /config/applications/<name>/warmup/2` become first-class. Add
   at least one test per.
5. **`working_directory` as preload resolution root** (not just
   `conf->working_directory` for relative ini): align with the existing
   helper `nxt_php_set_ini_path()` at `src/nxt_php_sapi.c:1050-1086` — use
   the same prefix logic, not a hand-rolled join.
6. **TODO.md entry.** The repo tracks follow-up work in `TODO.md`. Add an
   item for the P7-deferred on-demand `/warmup` endpoint and port RPC so it
   isn't forgotten when scheduler work lands.

### Updated file list (delta from first pass)

Add to §File-by-File:
- `docs/unit-openapi.yaml` (update `configApplicationPHP` schema at line
  6887).
- `docs/changes.xml` + `CHANGES` (release note).
- `roadmap/unit-php.md` (known-gap correction + shipping table flip).
- `roadmap/unit-todos.md` (cross-ref).
- `TODO.md` (P7-deferred follow-up item).

### Updated verification (delta)

Add to §Verification:
7. `Log.check_alerts()` passes on a clean run; intentional-alert tests have
   local `skip_alerts` entries.
8. OpenAPI schema validates: parse `docs/unit-openapi.yaml` with a YAML/JSON
   schema tool and confirm no new warnings around `configApplicationPHP`.
9. `CHANGES` diff is human-readable and matches `docs/changes.xml` entry.
10. Rootfs isolation smoke: build a minimal rootfs, configure a preload
    script inside it, confirm worker boots and first request sees preload
    symbols.

---

## Reviewer Burden & Binary Compatibility

Scoping pass: what this PR touches vs. leaves alone, and the guarantees that
keep the change a no-op for anyone who doesn't opt in.

### Diff shape (smallest-viable)

- **No new `.c` / `.h` files in `src/`.** All C code is inline in
  `src/nxt_php_sapi.c`. Net source delta: ~150 lines in one translation
  unit, which is smaller than the existing `nxt_php_set_target()` helper.
- **No build-system changes.** `auto/modules/php` is not modified — no new
  feature probes, no new link dependencies, no new compile flags. The
  `opcache_compile_file` symbol is resolved at runtime via
  `zend_hash_str_find_ptr(CG(function_table), …)`, so builds on a PHP that
  omits opcache still link.
- **Tests consolidated in one new file**: `test/test_php_preload.py` covers
  both `preload` and `warmup` (merged from the earlier two-file proposal).
  Reduces duplicated imports/fixtures and keeps the review narrow.
  Fixtures live in `test/php/preload/` only; the `large/` stress tree is
  **generated at test-setup time** by the test itself (`conftest.py`
  scoped `tmp_path` fixture) — zero bytes committed for bulk fixtures.
- **Doc diffs are localized**: OpenAPI adds ~20 lines in one block
  (`configApplicationPHP`); `CHANGES` + `docs/changes.xml` add one entry
  each; `roadmap/unit-php.md` is a 1-word change (Near → Shipped) plus a
  single sentence in §Known gaps.
- **Single-commit revert restores pre-change behavior.** The struct
  additions are additive; config without the new keys lands the same
  code paths as today. Reviewers can `git revert` this one commit
  without follow-up cleanups.

### Binary & wire compatibility

| Surface | Impact | Notes |
|---|---|---|
| `nxt_php_app_conf_t` struct layout (`src/nxt_application.h:62-65`) | +16 bytes (two pointers) at end of struct. | Field ordering preserved; new fields appended. Not exported across `.so` boundaries — consumed only by `nxt_php_sapi.c` and the conf-map in `nxt_main_process.c`. |
| `nxt_common_app_conf_t.u` union size | Unchanged. | `u.java` (5 members) remains larger than `u.php` (4 members after this change). Add a compile-time sanity check `NXT_STATIC_ASSERT(sizeof(nxt_java_app_conf_t) >= sizeof(nxt_php_app_conf_t))` — or just confirm and note in the commit. |
| `nxt_app_module_t` vtable (`src/nxt_application.h:152-164`) | Untouched. | PHP `.unit.so` ABI unchanged; a pre-built `php.unit.so` from 1.35.4 loads unchanged against an unmodified host. |
| libunit public ABI (`src/nxt_unit.h`) | Untouched. | No new symbols, no removed symbols. |
| Port message table (`src/nxt_port.h`) | Untouched. | No new `NXT_PORT_MSG_*` values — the deferred on-demand `/warmup` message is P7 scope. |
| Config JSON wire format | Additive. | Absent `preload`/`warmup` keys → `c->preload == NULL`, `c->warmup == NULL` → identical to today's code path. No migration, no config rewrite. |
| `sapi_module_struct.ini_entries` | Written once per process. | Not a Unit ABI, a PHP SAPI contract. Field exists in every PHP ≥ 5.4 (`Zend/zend_API.h` — historic). Our write is a one-time `const char *` assignment PHP reads during `php_init_config()`; we own the buffer and free only after `php_module_shutdown`. |

### Version matrix behavior

| Build | `preload` key | `warmup` key |
|---|---|---|
| PHP 5.4 – 7.3 | Validated; injection compiled out (version guard); one INFO log "opcache.preload requires PHP 7.4+, ignoring". | `opcache_compile_file` lookup returns null → single WARN, no-op. |
| PHP 7.4 – 8.5, opcache enabled | Primed via `ini_entries`. | Eagerly compiled; soft-fail per entry. |
| PHP 7.4 – 8.5, opcache disabled at build | `ini_entries` still written (ignored by PHP). | Lookup returns null → single WARN, no-op. |
| PHP 7.4 – 8.5, opcache enabled at build, disabled at runtime (`opcache.enable=0`) | PHP logs its own "preload requires opcache" error; our injected ini is accurate but inert. | Same null-lookup path → WARN + no-op. |
| ZTS build | Identical to NTS. `ini_entries` is written before TSRM resource allocation consumes it. | Identical; `opcache_compile_file` is TSRM-safe. |
| TrueAsync build (`NXT_PHP_TRUEASYNC`) | Runs unchanged in async path; preload happens before the async scope init. | Runs unchanged; warmup happens on the main worker thread before the async scheduler starts. |

### Opt-in guarantees

- **Zero-config behavior is byte-identical to today.** If a user upgrades
  without adding `preload` or `warmup` to their config, the PHP module
  takes the same code paths (including the legacy
  `options.file` / `options.admin` preload). No startup log changes
  unless the user opts in.
- **Existing preload tests unchanged.**
  `test_php_application_opcache_preload_chdir` and
  `test_php_application_opcache_preload_ffr` at
  `test/test_php_application.py:879-898` verify the legacy path and must
  continue to pass without edits. Running them is part of CI regression.
- **Precedence when both are set.** Unit's `preload` key wins — PHP SAPI
  parses `ini_entries` *after* `php.ini`, so our injection overrides the
  user's `options.file`. This matches Unit convention (config-key beats
  file for `user`, `group`, `working_directory`) and removes the need to
  pre-parse the ini file to detect conflicts. One INFO log on conflict
  detection. Legacy-only configs (no `preload` key) see no new log lines.

### Reviewer checklist (paste into PR description)

1. [ ] Struct addition additive, union size unchanged (`u.java` still largest).
2. [ ] No `auto/modules/php` changes (no new probes).
3. [ ] No new files under `src/` (all C code is in-place in `nxt_php_sapi.c`).
4. [ ] PHP < 7.4 compiles; preload no-ops with INFO log.
5. [ ] `ini_entries` buffer freed in `nxt_php_cleanup_targets()` after
       `php_module_shutdown()`, not before.
6. [ ] Warmup wrapped in `zend_try`/`zend_end_try` — no longjmp escape.
7. [ ] OpenAPI schema updated with descriptions and an example.
8. [ ] `CHANGES` + `docs/changes.xml` carry a `<change type="feature">`.
9. [ ] Single-commit `git revert` restores pre-change behavior on a test app.
10. [ ] ZTS + NTS builds both pass the new test file.
