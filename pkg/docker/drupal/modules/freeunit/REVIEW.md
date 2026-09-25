# Review of the `freeunit` module prototype

Adversarial review (Drupal core maintainer + security reviewer view) of
`pkg/docker/drupal/modules/freeunit/` as of `db54ba59`. Claims about FreeUnit
were checked against `src/`; claims about Drupal against the reviewer's
knowledge of 11.x/12.x (Drupal core itself is not installable here).

Severity: **blocking** (must not ship, security or data-integrity), **major**
(wrong behaviour in a common case, or an API misuse that breaks on a supported
core), **minor** (correctness edge, dead code, simplification).

## Round 1 (against `db54ba59`)

Counts: blocking 1, major 6, minor 12.

### Blocking

- **B1. Reproduced header values are never compared, so the router serves
  headers Drupal did not send (cache poisoning of headers).**
  `src/EventSubscriber/StaticCacheWriter.php:127-138`,
  `config/install/freeunit.settings.yml:12-20`,
  `src/Drush/Commands/FreeUnitCommands.php:72-77`.
  The writer only checks header *names* against `reproduced_headers`. The
  route (`RouteConfigGenerator`, `unit-drupal-freeunit.json:141-150`) sends
  one fixed value per header for every file. A French page
  (`Content-Language: fr`) is served as `en`; a page whose `Cache-Control`
  differs (a module that sets `private`/`max-age=0` on a `CacheableResponse`,
  or a site whose `cache.page.max_age` is not what the route was generated
  with) is served as `max-age=300, public`; a page with a custom `Vary` loses
  it. Fix: one config map `static_cache.headers` (name → exact value) that is
  the *single source* for both the writer (require equality, case-insensitive
  name, byte-exact value) and the route generator; hard-code the drop list;
  additionally require `Cache-Control` to contain `public` and no
  `private`/`no-cache`/`no-store` as defence in depth.

### Major

- **M1. Runtime requirements use `hook_requirements('runtime')` and the
  `REQUIREMENT_*` constants, which are deprecated in 11.2 and gone in 12,
  while `core_version_requirement` claims `^12`.** `freeunit.install:52-102`,
  `freeunit.info.yml:7`. On 12 the status report silently loses every
  FreeUnit row. Fix: `#[Hook('runtime_requirements')]` in `src/Hook` with
  `RequirementSeverity`, and `core_version_requirement: ^11.2 || ^12`.
- **M2. No `hook_uninstall`: the files stay on disk and the FreeUnit route
  keeps serving them, forever stale, after the module (and its invalidator)
  is gone.** `freeunit.install`. Fix: purge in `freeunit_uninstall()`.
- **M3. `purge()` renames the cache root aside, which needs write permission
  on the root's *parent* directory.** `src/StaticCache/StaticCacheFiles.php:71-83`.
  The README only requires the cache directory itself to be writable, so on
  the documented layout every full cache flush logs "Cannot rename" and
  leaves every file in place: stale pages after `drush cr`. Fix: delete the
  tree's contents, keep the root.
- **M4. Pages are written only on `X-Drupal-Cache: MISS`.**
  `src/EventSubscriber/StaticCacheWriter.php:118`. After a purge, an expiry
  delete, or enabling the module on a warm site, every page is a page_cache
  HIT and is never written again until page_cache is cleared, so the static
  cache silently stays empty. Fix: also write on HIT when the file is
  missing (the HIT response is the stored `CacheableResponse`, so tags and
  the cid are available).
- **M5. `GET /index.php` is served as a static file by the docroot `share`.**
  `examples/unit-drupal-freeunit.json:83-102` (inherited from
  `pkg/docker/drupal/unit-drupal.json`). The `*.php → 404` rule exempts
  `/index.php`, and the next rule is `share: /var/www/drupal/web$uri` with
  no `types`, so the file exists and the router sends `index.php` as a file
  instead of running Drupal. Not a secret (core file), but every
  `/index.php` URL (clean URLs off, some crawlers) is broken. Fix: pass
  `/index.php` and `/index.php/*` to the application before the share.
- **M6. Invalidator + `hook_cache_flush` purge twice, and one of them relies
  on an interface the reviewer cannot pin to a core version.**
  `src/Cache/StaticCacheTagsInvalidator.php:29,62-65`, `src/Hook/FreeUnitHooks.php:27-30`.
  `drupal_flush_all_caches()` always invokes `hook_cache_flush`; whether it
  also calls `CacheTagsPurgeInterface::purge()` on collected invalidators
  depends on the core version. Two purges are a second full tree walk per
  `drush cr`. Fix: keep the hook, drop the interface.

### Minor

- **m1.** Docblock claims Drupal's front controller uses Symfony Runtime;
  it does not (index.php calls `$response->send(); $kernel->terminate()`).
  The conclusion still holds because `Response::send()` itself calls
  `fastcgi_finish_request()` (which FreeUnit implements,
  `src/nxt_php_sapi.c:228-291`, and reports as detached work). Fix comment.
  `src/EventSubscriber/StaticCacheWriter.php:27-32`.
- **m2.** Mapper accepts 1024-byte paths but the index column `file` is 1024
  for the *absolute* file name, so a long URL makes `record()` throw inside
  `kernel.terminate`. `src/StaticCache/StaticCachePathMapper.php:89`,
  `freeunit.install:22-27`. Fix: cap the request path at 512 bytes.
- **m3.** Behind a TLS-terminating proxy Drupal writes under `https/<host>`
  while the route reads `http/<host>` (`build()` has a `$scheme` parameter
  that Drush never sets). `src/StaticCache/RouteConfigGenerator.php:48`,
  `src/Drush/Commands/FreeUnitCommands.php:68-83`. Fix: `--scheme` option.
- **m4.** A request with an `Authorization` header (basic_auth) is served the
  anonymous page by the router, while page_cache's request policy would have
  bypassed. Not a leak (anonymous content only) but wrong for basic_auth
  clients. Fix: one more bypass rule `headers: {Authorization: "*"}`.
- **m5.** `freeunit:routes` hard-codes `Vary`, `X-Frame-Options`, ... and
  takes hosts from `--host` instead of `static_cache.hosts`; the two can
  disagree with what the writer accepts. Fix: generate from config only.
- **m6.** `\Drupal::config()` static calls in injectable classes
  (`FreeUnitCommands.php:73,113`). Fix: inject `ConfigFactoryInterface`.
- **m7.** `CronController` uses the `ContainerInjectionInterface::create()`
  boilerplate; core's `AutowireTrait` (10.2+) does it. `src/Controller/CronController.php:24-33`.
- **m8.** `services.yml` sets `_defaults: autowire: true` but every service
  lists its arguments; the `cache.page` argument cannot be autowired anyway.
  Fix: drop `_defaults`, keep explicit arguments and the class aliases
  (needed by the Hook class and Drush's `AutowireTrait`).
- **m9.** Identity route has `types: [text/html]`; the file name always ends
  in `_.html` inside a `chroot`, so the rule can never fail. Fix: drop it.
- **m10.** `reproduced_headers` includes `x-drupal-cache` although the route
  sends a different value on purpose; handled by B1's redesign.
- **m11.** Narrow stale-file window: `drupal_flush_all_caches()` invokes
  `hook_cache_flush` *before* `cache.page->deleteAll()`; a writer that
  records, writes and re-checks between the two keeps its file while
  page_cache is emptied. Bounded by the next invalidation of the page's
  tags, and no wider than the window for pages rendered right after the
  flush. Documented, not fixed.
- **m12.** README/design text describe the pre-review header handling and
  the rename-based purge. Update.

### Verified as correct (no finding)

- Cron key: travels in `X-FreeUnit-Cron-Key`, compared with `hash_equals`,
  never logged; `ScheduleConfigGenerator::check()` prints "headers differ"
  only; unitd's schedule validators (`src/nxt_conf_validation.c:5277-5345`)
  never echo header values in errors. The schedule request's `REMOTE_ADDR`
  is `127.0.0.1` (`src/nxt_router_schedule.c:474`), and a schedule can only
  `pass` to an application (`nxt_conf_vldt_schedule_pass`), so listener
  routes answering 404 for `/freeunit/cron*` do keep clients out.
- Control socket: used from Drush only; the client is a plain Unix-socket
  HTTP/1.1 client; the socket path comes from config and stays root-only.
- Path traversal: the mapper accepts only an ASCII pchar subset without `%`,
  refuses empty and dot segments, and the file service refuses anything not
  under the root; the route adds `chroot` + `follow_symlinks: false`, and
  the router rejects `%2e%2e` targets and NUL bytes in `share`
  (`src/nxt_http_static.c:455-470`).
- Poisoning: only `GET`, status 200, `CacheableResponseInterface`, no
  `Set-Cookie`, no session, anonymous, no query string, `text/html`, a
  configured host, and page_cache has just stored it; redirects, 4xx, 5xx
  and BigPipe responses are never written.
- Writes: `tempnam` + `chmod 0644` + `rename` in the target directory; the
  `.gz` is written first so the gzip route never serves a `.gz` older than
  its `.html`.
- Tag → file mapping: recorded *before* the file exists, deleted with the
  file, truncated on purge, expired rows removed on cron; invalidations
  run at once and again on `kernel.terminate` (priority −50, before the
  writer's −100), and the writer re-checks the page_cache item after the
  write.
- Route matching (checked against `src/nxt_http_route.c:1962-2118`): a
  `headers` rule must match *every* line of that header and never matches an
  absent header, so bypasses must be positive rules placed first; the
  `cookies` rule parses all Cookie lines. `query: ""` matches no query and a
  bare `?`. `method: [GET, HEAD]` keeps every unsafe method in PHP.

### Round 1 fixes

- B1: `static_cache.headers` (name → exact value) replaces
  `reproduced_headers`/`dropped_headers`; `StaticCacheWriter` requires
  byte-exact values, keeps a hard-coded drop list (`DROPPED`), and refuses
  any `Cache-Control` that is not `public` or that has `private`,
  `no-cache` or `no-store`. `RouteConfigGenerator` emits the same map (plus
  `X-Drupal-Cache: HIT-FREEUNIT`, and `Accept-Encoding` appended to `Vary`
  when gzip is on). The example configuration and the check script were
  updated (the check now also asserts the gzip variant's `Content-Type` and
  `Vary`).
- M1: `hook_runtime_requirements` with `RequirementSeverity` in
  `src/Hook/FreeUnitHooks.php`; `freeunit.install` keeps `hook_schema` only;
  `core_version_requirement: ^11.2 || ^12`.
- M2: `freeunit_uninstall()` purges the files.
- M3: `purge()` deletes the tree's contents and keeps the root.
- M4: a `HIT` is written when its file is missing.
- M5: `/index.php` and `/index.php/*` pass to the application before the
  docroot share (example configuration); the check script asserts it.
- M6: `CacheTagsPurgeInterface` dropped; `hook_cache_flush` purges.
- m1, m2, m4, m5, m6, m7, m8, m9, m10, m12 fixed as proposed. m3 fixed more
  thoroughly than proposed: `static_cache.scheme` keys the file tree on both
  sides (`StaticCachePathMapper::hostDirectory()` no longer takes the
  request's scheme), so a TLS-terminating proxy cannot split writer and
  route. m11 stays documented.
- Check script additions: `Authorization` header, `/index.php`,
  `/index.php/node/1`, an encoded dot-segment traversal (400) and a plain
  one (normalised, falls through to PHP).

## Round 2 (against the round-1 commit)

Counts: blocking 0, major 0, minor 5.

- **m13.** `StaticCacheIndex::record()` still adds the pseudo-tag
  `freeunit:all` to every file although nothing queries it any more (purge
  truncates): one wasted row per page. Fixed: removed.
- **m14.** `Content-Length` on a Drupal response (some modules set it) is
  neither reproduced nor dropped, so such pages are never written, although
  the router computes its own. Fixed: added to `StaticCacheWriter::DROPPED`.
- **m15.** `RouteConfigGenerator::build()` with empty `static_cache.hosts`
  produced a route with an empty `cookies` rule and no shares. Fixed:
  throws `LogicException`.
- **m16.** With the tree keyed on the configured scheme, a page rendered for
  the other scheme (direct HTTP hit on an HTTPS site) is written into the
  same tree. Documented in the README (serve cached hosts on one scheme).
- **m17.** Session cookie names for the bypass rule are computed with
  `Request::create("scheme://host/")`, so a site installed under a base
  path gets the wrong exact name; the `*SESS*` rule still catches it unless
  the cookie sits on its own second Cookie line. Not fixed (prototype;
  `hasSession()` in the writer means no authenticated page is ever
  written, so the failure mode is a PHP round trip, not a leak).

Also re-checked and unchanged: the writer's `Cache-Control` gate handles
page_cache's own `setPrivate()` on a HIT for a request with a session
cookie, and 304 responses (status != 200); the invalidator's ordering
(−50 before −100) holds; `hook_uninstall` runs while the module's
services are still in the container.

Unverifiable here (no Drupal core readable in this environment): the
exact names `RequirementSeverity::{OK,Warning,Error,Info}`, the `cron` and
`state` interface aliases that `AutowireTrait` needs in `CronController`,
and that `hook_runtime_requirements` is the 11.2 name. Each is one line to
correct on first install.

### Round 2 fixes

m13, m14, m15, m16 as above.

## Round 3 (against the round-2 commit)

Counts: blocking 0, major 0, minor 1.

- **m18.** Removing the pseudo-tag (m13) left a page without cache tags
  with *no* index row, so a future `Expires` on such a page would never be
  honoured by `expiredFiles()`. Fixed: one row per file is guaranteed again
  (`freeunit:file`), with the reason stated in the code.

No blocking or major findings remain; the cycle stops here.

## Verification

- `php -l`: 13 files (`src/**/*.php`, `freeunit.install`), PHP 8.4, all
  clean; every `.yml` parses; the example JSON parses.
- PHPStan 2.2 (phar from GitHub releases), level 6, on `src/` and
  `freeunit.install`: 44 reports, every one either an unknown
  Drupal/Drush/Symfony symbol (drupal.org and packagist dists are blocked
  here, and reading a GitHub mirror clone of core was refused by the
  session's policy) or a missing iterable value type in a docblock; no
  logic finding. A run with Drupal stubs was therefore not possible.
- `examples/check-static-cache.py` against a `unitd` built from this tree
  (`./configure && ./configure php && make -j2 unitd php`, run under
  `unshare -n` with `lo` up, control socket in a short `/tmp` path, port
  18931+): 19/19 checks pass, including the new `Authorization`,
  `/index.php`, traversal and gzip-header checks.
- Stub harness (no Drupal runtime: stub `ConfigFactoryInterface`,
  `SessionConfigurationInterface`, `Request`, `LoggerInterface`) against
  the same `unitd`: 35/35 checks pass. `StaticCachePathMapper` refuses
  `..`, `.`, `//`, `%xx`, backslash, non-ASCII, other hosts, query strings
  and >512-byte paths; `StaticCacheFiles` writes 0644 through a temporary
  file and leaves none behind, refuses paths outside the root, deletes
  both variants, and `purge()` empties the tree but keeps the root;
  `RouteConfigGenerator::build()` equals `routes.drupal_page` of the
  example byte for byte (keys sorted); `ControlApiClient` PUTs it, reads
  it back, creates a missing parent, maps 404 to `NULL` and errors to
  `ControlApiException`; the applied route serves the files the classes
  wrote (identity and gzip, fixed headers), and session cookie and
  `Authorization` requests bypass it; a deleted file falls back to PHP.

## Round 4 (CI)

The module was installed and tested for the first time, on Drupal 11.4.7
and 12.0.x-dev with PHP 8.5, under a FreeUnit built from this branch
(workflow `drupal-freeunit-module.yml`). Tests added: unit (path mapper;
header rules; generated route equals the example), kernel (writer, tag
index rows, invalidation, cron expiry, purge on cache flush, uninstall,
runtime requirements, cron controller: 404 unless keyed and loopback,
`hash_equals`, key never logged) and functional (anonymous page to cache
file with the exact route headers, served by FreeUnit).

What CI caught (all fixed):

- **C1 (blocking).** Drupal sends `Content-Type: text/html; charset=utf-8`
  (lower case, `HtmlRenderer`); the default `static_cache.headers` and the
  example route said `UTF-8`, so the byte-exact comparison refused every
  page: the static cache never wrote anything.
- **C2 (major).** The writer only checked that each response header was in
  the fixed set; a page *missing* one the route adds (`Expires`, `Vary`)
  was written, and the router would add a header Drupal did not send. The
  header set must now match exactly.
- **C3 (major).** Uninstall failed with "no such table": the module's table
  is dropped while the invalidator is still in the container, and saving
  `core.extension` invalidates tags. `hook_uninstall` now calls
  `StaticCacheTagsInvalidator::uninstall()`, which purges and then ignores
  invalidations for the rest of the request.
- **C4 (minor).** `StaticCacheIndex::record()` committed by letting the
  transaction go out of scope, deprecated in 11.5 and reported on 12; now
  `commitOrRelease()`.
- **C5 (minor).** phpstan level 6: `invalidateTags()` lacked `: void`; a
  `@var` narrowing replaced by an `instanceof` check where it is used.
- **C6 (tooling).** `check-static-cache.py` set an application `user`,
  which an unprivileged `unitd` refuses even for its own user.

Settled by CI (the items Round 2 could not verify):
`RequirementSeverity::{OK,Warning,Error,Info}` exist; `hook_runtime_requirements`
is the 11.2+ name and the OOP `#[Hook]` form runs; `AutowireTrait` resolves
`CronInterface` and `StateInterface` through core's aliases;
`core_version_requirement: ^11.2 || ^12` holds for 11.4 and 12.0.x-dev.
The live site confirmed `fastcgi_finish_request()` under FreeUnit (status
report), that the FreeUnit schedule runs cron through `/freeunit/cron`
(`system.cron_last` advances) while clients get 404, and that the gzip
variant, the session-cookie bypass, the query-string bypass and deletion
on a node save behave as designed.
