# A Drupal module for FreeUnit (`freeunit`): exploration and design

- Status: exploration, with an unverified prototype skeleton
- Date: 2026-09-23 (Day 1 of the DrupalCon sprint week)
- Branch: `stream/drupal-module-explore`, from the integration tip `b3bec257`
- Code base: every `file:line` below refers to `b3bec257`.
- Prototype: not kept in the tree; see commit eb1da176
  (`pkg/docker/drupal/modules/freeunit/`).

## 0. Summary

A Drupal module can make a Drupal site on FreeUnit faster in two ways.
First, it can move work out of PHP and into the C router. Second, it can
use FreeUnit's own scheduling and control plane instead of workarounds.
Ranked by *(gain × confidence) / effort*:

| # | feature | score | expected gain |
|---|---|---|---|
| 7a | Separate application pools for admin/batch/AI and front-end routes (configuration only) | 12.0 | Front-end tail latency no longer depends on slow admin or AI requests |
| 2 | Cron through FreeUnit `schedules` | 10.0 | No cron work in visitor requests, and no sidecar crontab. Verified working |
| 4a | Restart through the control API after deploy, so OPcache can run with `validate_timestamps=0` | 8.0 | No `stat()` per included file; no stale bytecode after deploy |
| 7b | `processes` / `limits` tuning guidance | 6.0 | Fewer 503s and less swapping under load |
| **1** | **Static page cache served by the router** | 5.0 | **Largest absolute gain:** anonymous hits never reach PHP. Estimated 20–100× the requests per second of a Drupal `page_cache` hit (to be measured) |
| 3 | Post-response work | 5.0 | Drupal already has it on FreeUnit. The work left is detection and reporting |
| 5a | FreeUnit `/status` in Drush and the status report | 4.0 | Operability |
| 7e | Queue processing through schedules | 3.0 | Queue work (including AI jobs) happens away from visitor requests |
| 6 | Router offload of private file downloads | 2.25 | Frees a PHP worker per download. **Needs a FreeUnit feature (F2)** |
| 4b | OPcache preload script generated from the container | 1.3 | Probably 5–15 % on the MISS path (assumption) |
| 5b | OTel bridge (Drupal spans as children of FreeUnit's span) | 1.3 | Traces that join up. **Needs FreeUnit fix F1** |
| 7c | 103 Early Hints | 0.75 | Earlier asset fetches. **Needs FreeUnit feature F7** |

Feature 1 has the largest effect, but it has the most correctness pitfalls,
so its confidence is lower and its effort higher. Features 2 and 7a are cheap
and certain, so they come first in the plan (section 12).

Findings from the FreeUnit code that change how the brief framed things:

- **`php_sapi_name()` is `"cli-server"`, not `"unit"`.** `"unit"` is only
  the SAPI's pretty name and the name of its internal extension
  (`src/nxt_php_sapi.c:298-299`, `:184`). This was observed on a local build:
  the `X-Sapi: cli-server` header in section 3.1. Code that wants to detect
  FreeUnit should test `SERVER_SOFTWARE`, which starts with `Unit/`
  (`src/nxt_php_sapi.c:1549`, `src/nxt_main.h:14-15`). A side effect: Drupal's
  `/.ht.router.php`, the router script for PHP's built-in server, passes its
  own `PHP_SAPI !== 'cli-server'` guard on FreeUnit. The dev kit's `*/.ht*`
  → 403 rule blocks it. Every other FreeUnit configuration for Drupal must
  block it too.
- **Drupal already does post-response work on FreeUnit.** Drupal's `index.php`
  (11.x and main) returns a closure to `autoload_runtime.php`, so Symfony
  Runtime's `HttpKernelRunner` handles the request. `HttpKernelRunner`
  calls `fastcgi_finish_request()` when that function exists, and then
  `$kernel->terminate()`. FreeUnit's SAPI defines `fastcgi_finish_request()`
  (`src/nxt_php_sapi.c:228-290`) and reports the rest of the request to the
  router as detached work (`:279`). `/status` shows it as
  `processes.detached` (`src/nxt_status.c:46`, `:228`).
- **`options.admin` is applied too late for `opcache.preload`.** The
  `admin` and `user` PHP options are set *after* `php_module_startup()`
  (`src/nxt_php_sapi.c:442` vs `:448-452`). Preloading and the OPcache
  shared memory size are decided during startup. So they belong in
  `options.file`, which is read before startup (`:433`). The existing
  test suite does it that way (`test/test_php_application.py:96`).
  Observed: the dev kit's own `"opcache.enable": "1"` in `options.admin`
  (`pkg/docker/drupal/unit-drupal.json`) fails on every prototype start
  with `setting PHP option "opcache.enable: 1" failed` and the PHP warning
  "Zend OPcache can't be temporary enabled". It has no effect, because
  OPcache is on by default, but the line should move to a php.ini or be
  dropped. `validate_timestamps` and `revalidate_freq` are runtime-settable
  and do work from `options.admin`.
- **FreeUnit has no X-Accel-Redirect or X-Sendfile.** The router handles
  only `Status`, `Server`, `Date`, `Connection`, `Content-Type`,
  `Content-Length`, `Upgrade` and `Sec-WebSocket-Accept` from an
  application (`src/nxt_http_response.c:23-35`). `sendfile()` exists only
  for the router's own static files (`src/nxt_conn_write.c:194-230`). See F2.
- **In an inherited trace, the application sees the caller's span as its
  parent, not FreeUnit's span.** When a request arrives with a valid
  `traceparent`, FreeUnit passes it on unchanged (`src/nxt_otel.c:189-196`).
  It adds its own span's `traceparent` only when it starts a new trace
  (`:197-216`). So Drupal spans would be siblings of FreeUnit's span. See F1.

## 1. Sources and what was verified

| fact | how established |
|---|---|
| Drupal `main` is `12.0-dev` and requires `php >=8.5.0`, `symfony/http-kernel ^8.1`, `symfony/runtime ^8.1` | **Verified**: `core/composer.json` and `core/lib/Drupal.php` from `raw.githubusercontent.com/drupal/drupal/main`, 2026-09-23 |
| Drupal 11.x (`11.4-dev`) requires PHP ≥ 8.3 and Symfony ^7.4 | **Verified**, same way, branch `11.x` |
| Drupal 12.0.0 is planned for the week of 7 December 2026 | Web search summary only; **not verified** |
| `index.php` uses Symfony Runtime; `HttpKernelRunner::run()` calls `fastcgi_finish_request()` and then `terminate()` | **Verified**: Drupal `index.php` (main, 11.x); `symfony/runtime` 8.1, `Runner/Symfony/HttpKernelRunner.php:32-52` |
| `automated_cron` subscribes to `kernel.terminate` with priority 100 | **Verified**: `AutomatedCron.php` (main) |
| `page_cache` keys on `scheme+host+request URI` and the request format; stores only `CacheableResponseInterface` responses; ignores max-age but honours `Expires`; sets `X-Drupal-Cache: MISS` only after a successful store | **Verified**: `PageCache.php` (main) |
| The session cookie name is `SESS`/`SSESS` followed by the first 32 hex digits of `sha256(host + base path + suffix)` | **Verified**: `SessionConfiguration.php:72-109` (main) |
| BigPipe sends each chunk with `print` and then `flush()` | **Verified**: `BigPipe::sendChunk()` (main) |
| Cache tag invalidators are collected through the `cache_tags_invalidator` service tag; there is also `CacheTagsPurgeInterface` | **Verified**: `core.services.yml:247-253`, `CacheTagsInvalidator.php` (main) |
| `drupal_flush_all_caches()` invokes `hook_cache_flush` and then calls `deleteAll()` on each bin | **Verified**: `core/includes/common.inc:261-281` (main) |
| Gander (`PerformanceTestTrait`) builds its OTel spans in the test process (`OTEL_COLLECTOR`), from the browser performance log and data collected on the server; it sends no `traceparent` to the site | **Verified**: `core/tests/Drupal/Tests/PerformanceTestTrait.php:117, 363-376, 471-576` (main) |
| `open-telemetry/sdk` and `exporter-otlp` are dev dependencies of core | **Verified**: root `composer.json` (main) |
| Symfony 8.1 `SymfonyRuntime` has a FrankenPHP worker-mode runner | **Verified**: `SymfonyRuntime.php:47-55, 169-191` |
| Symfony `Response::sendHeaders(103)` needs a SAPI function `headers_send()` | **Verified**: `Response.php:336-374` (http-foundation 8.1) |
| The only place Drupal core names `cli-server` is `.ht.router.php` (and its scaffold copy) | **Verified**: GitHub code search on `drupal/drupal` |
| DrupalCon Rotterdam runs 28 September to 1 October 2026 | Web search summary; **not verified** on drupal.org, which was blocked |
| Contrib: boost (static files, gzip, served through a RequestEvent in 8.x+), purge / varnish_purge (tag-based external invalidation), redis (cache backend), http_cache_control (Cache-Control tuning), ultimate_cron (per-job schedules), page_cache_boost (10/11) | **Not verified.** drupal.org and git.drupalcode.org were blocked by the egress proxy. This is from memory plus search snippets |
| Drupal AI initiative (the AI module, agents, long LLM calls, streaming) | **Not verified.** From general knowledge |

Every FreeUnit behaviour below that says "observed" was run against a
`unitd` built from `b3bec257` (`./configure && make -j2 unitd`, plus a PHP
8.3 embed module, because this host's apt mirror has no 8.5 embed SAPI).
That covers the routes, the schedule, `/status`, the restart and the
control API semantics.

## 2. Feature evaluation

Scoring: gain (1–5) × confidence (1–5) / effort (1–5). Gains are per
affected request unless noted. "Measure" names the tool and the metric.

| # | mechanism | FreeUnit capability | gain / how to measure | risks, correctness | effort | score |
|---|---|---|---|---|---|---|
| **1** Router static page cache | On a page_cache MISS, write the HTML (and `.gz`) to `<dir>/<scheme>/<host>${uri}_.html` on `kernel.terminate`. A route serves it with `share` and falls back to PHP. Tag invalidation deletes files | `share` with a variable path, `chroot`, `fallback`, `response_headers`, `match` on `method`, `query`, `host`, `scheme`, `cookies`, `headers` (`src/nxt_http_static.c`, `src/nxt_http_route.c`) | **5.** Hits never reach PHP. Observed: router file hit 103k req/s, 0.31 ms mean, vs 19k req/s for a *no-op* PHP script (`ab -k -c32`, 4 cores). A Drupal page_cache hit costs far more than a no-op script. Measure with `wrk`/`ab` on anonymous URLs, and CPU per request | Session bypass (multiple Cookie lines!), query strings, headers that differ per page, multilingual `Content-Language`, the invalidation race inside transactions, disk growth, multi-node. See §3 | 3 | 5×3/3 = **5.0** |
| **2** Cron via `schedules` | `drush freeunit:schedule --apply` PUTs `/config/schedules/drupal-cron`: `GET /freeunit/cron` with an `X-FreeUnit-Cron-Key` header, `overlap: skip`. Listener routes 404 the path | `schedules` (ADR 0004; `src/nxt_router_schedule.c`), validated at `src/nxt_conf_validation.c:1413` | **2.** Removes cron from visitor requests (automated_cron runs in `terminate`, holding a worker as detached) and the sidecar crontab. Measure p99 of visitor latency during cron with `wrk --latency`, and `/status` `detached` | Key handling; a Host that `trusted_host_patterns` rejects (400); a run holds one process (R5); a schedule timeout does not stop PHP. **Observed working**: runs every interval, `REMOTE_ADDR` 127.0.0.1, `User-Agent: FreeUnit-Schedule/drupal-cron`, key redacted in the info log | 1 | 2×5/1 = **10.0** |
| **3** Post-response work | Nothing new to build: `HttpKernelRunner` already calls `fastcgi_finish_request()`, then `terminate` (automated_cron, `DestructableInterface` services, cache collector writes). The module reports it and moves its own writes there | `fastcgi_finish_request()` with detached accounting (`src/nxt_php_sapi.c:228-290`, `src/nxt_unit.c:3612`) | **1** (already there). Measure TTFB with and without terminate-heavy work: `curl -w %{time_starttransfer}` | Detached work keeps its process busy, so `processes.max` has to cover it. With `APP_DEBUG` on, Symfony Runtime skips `fastcgi_finish_request()` | 1 | **5.0** |
| **4a** Deploy restart | After `drush deploy`: `GET /control/applications/drupal/restart`. New prototype, graceful worker replacement. Makes `opcache.validate_timestamps=0` safe | Restart (`src/nxt_controller.c:2470-2540`, `src/nxt_router.c:1602-1690`): the generation is bumped, the old prototype is sent QUIT, old workers finish and quit (`:5917`, `:6026`, `:6181-6218`) | **2.** No per-include `stat()`. Measure req/s on the MISS and authenticated paths with `validate_timestamps` 0 vs 1 | Needs the root-only control socket, so Drush must run as its owner. The first requests after a restart are cold (see 4b) | 1 | 2×4/1 = **8.0** |
| **4b** Preload | Generate `preload.php` from the compiled container's service classes and the Composer classmap, filtered to classes whose parents and interfaces resolve. Set in `options.file` | PHP runs in the prototype before fork (`src/nxt_application.c:604-605`), so preloaded classes are shared by every worker. The preload hooks are safe during startup (`src/nxt_php_sapi.c:1380`, `:1411`) | **2** (assumption: 5–15 % on uncached paths). Measure with Gander `PerformanceData` wall time and `wrk` on authenticated pages | Preloaded code cannot change without a restart, so it needs 4a. Classes that fail to link produce warnings. `opcache.preload_user` must be set when running as root | 3 | 2×2/3 = **1.3** |
| **5a** Status | `drush freeunit:status`; status report rows for FreeUnit, `fastcgi_finish_request`, the cache directory, automated_cron | `/status/applications/<name>` (`src/nxt_status.c`); per-schedule counters are only designed (N4, `docs/observability/status-extensions.md`) | **1**, operability | Web workers cannot read the root-only socket (F3) | 1 | 1×4/1 = **4.0** |
| **5b** OTel bridge | Read `HTTP_TRACEPARENT` and start Drupal's server span as its child with `open-telemetry/sdk`. Patch Gander to take the main document's `traceparent` *response* header as the parent of its `main` span | FreeUnit forwards `traceparent` to PHP as `HTTP_TRACEPARENT` (router prefix, `src/nxt_router.c:424-431`), and echoes it in the response (`src/nxt_otel.c:219-229`) | **2**, one trace from the edge to SQL. Measure: spans in Tempo | Siblings rather than children in the inherited case (F1). The SDK costs something per request, so sample | 3 | 2×2/3 = **1.3** |
| **6** Download offload | Private files: Drupal sets `X-Accel-Redirect: /_private/<path>`, and the router serves the file (Range, ETag, `sendfile`) | **Missing** (F2). Today a download streams through shared memory and holds a PHP worker until a slow client has read it all | **3** for sites with large or slow downloads. Measure concurrent 100 MB downloads over throttled clients: workers busy in `/status`, req/s of other pages | The access check must stay in PHP; the internal location must not be reachable by clients | 4 (C work) | 3×3/4 = **2.25** |
| **7a** Pools | Two applications with the same root: `drupal` (front) and `drupal_admin` (`/admin/*`, `/batch`, `/user/*`, `/node/*/edit`, AI endpoints) with their own `processes` and `limits.timeout` | Per-application processes and limits (`src/nxt_conf_validation.c:1392`, `:1457`). Targets share one application's pool, so this needs two applications | **3** on front tail latency under admin or batch load. Measure `wrk` p99 on the front while a batch or AI request runs | OPcache is per prototype, so memory is counted twice. Sessions are shared (same code and DB), so this is fine | 1 | 3×4/1 = **12.0** |
| **7b** Tuning | Guidance, plus `drush freeunit:schedule` checks: `processes.max ≥ 2` with a schedule; schedule timeout ≤ `limits.timeout`; `limits.requests` to recycle leaky workers; `spare` ≥ 1 to avoid spawn latency | `processes`, `limits.requests`, `limits.timeout` | **2** | — | 1 | 2×3/1 = **6.0** |
| **7c** Early Hints | Symfony can already send 103 when the SAPI has `headers_send()` | **Missing** (F7) | **1–2** on first paint | Proxies that mishandle 1xx | 4 | 1.5×2/4 = **0.75** |
| **7e** Queue schedules | One schedule per heavy queue (`/freeunit/queue/{name}`), run with `overlap: skip` on the admin pool | `schedules` | **2**: AI and batch queue work away from visitors | Same as feature 2 | 2 | 2×3/2 = **3.0** |

## 3. Design: static page cache served by the router (feature 1)

### 3.1 What was observed on a local unitd

`examples/check-static-cache.py` PUTs `examples/unit-drupal-freeunit.json`
(with local paths) to a `unitd` built from this tree. It drives it with a
stand-in PHP script that behaves like the writer. All checks passed:

```
ok   first anonymous GET rendered by PHP
ok   second anonymous GET served by the router
ok   gzip variant served precompressed
ok   HEAD served by the router
ok   exact session cookie reaches PHP
ok   secure session cookie reaches PHP
ok   exact session cookie on a 2nd Cookie line reaches PHP
ok   other host session (*SESS*) reaches PHP
ok   query string reaches PHP
ok   POST reaches PHP
ok   /freeunit/cron is 404 for clients
ok   schedule reached /freeunit/cron with the key
```

Router behaviour that shaped the design, all observed:

1. **A negated match never matches an absent header or cookie.**
   `"headers": {"Cookie": "!*SESS*"}` and `"cookies": {"x": "!*"}` fail on a
   request with no `Cookie` at all. That is the common anonymous case.
   `nxt_http_route_header()` starts from "no match" and returns it when no
   field is found (`src/nxt_http_route.c:1962-1990`); the cookie test does
   the same (`:2087-2118`). **Consequence:** "has a session" must be a
   *positive* bypass rule placed *before* the cache rules.
2. **A header rule must match every line.** With `Cookie: a=1` and
   `Cookie: SESSx=…` on two lines, `"headers": {"Cookie": "*SESS*"}` does
   **not** match, because the first non-matching line returns
   (`src/nxt_http_route.c:1984-1986`). `"cookies"` parses all Cookie lines
   (`:2073`) and matches. So the primary bypass uses the site's exact session
   cookie names (`SESS…`/`SSESS…`, computed by Drupal's
   `SessionConfiguration::getOptions()`). The `*SESS*` header rule is only
   an extra safety net. HTTP/1.1 user agents send one Cookie line (RFC 6265
   §5.4), but a proxy that converts HTTP/2 requests may not.
3. **`"query": ""` matches "no query" and a bare `?`**, and nothing else.
4. **`${uri}_.html` works as a share path.** `$uri` is decoded and has its
   dot segments removed. A `%2e%2e` traversal was answered 400 before
   routing.
5. **FreeUnit's MIME table has no `.gz`** (`src/nxt_http_static.c:1919-1976`
   has no entry), so the gzip rule cannot use `"types"`. It sets
   `Content-Type` and `Content-Encoding` through `response_headers`, which
   replace the static handler's values.
6. **`chroot` + `follow_symlinks: false`** on the cache directory works, and
   it confines the share even if a writer bug created a symlink.
7. Static responses carry an `ETag` of mtime and size, and `Last-Modified`.
   They answer conditional and Range requests
   (`src/nxt_http_static.c:707-890`, `:1129-1500`). The identity and gzip
   variants get different ETags because their sizes differ. In the rare
   case of equal mtime *and* size, the ETags would collide.

### 3.2 Routes

Full example: `examples/unit-drupal-freeunit.json` in commit eb1da176.
The listener's `drupal` route keeps the dev kit's security rules. It adds
`/freeunit/cron` → 404 and ends with the docroot share, which falls back
to `routes/drupal_page`:

```
drupal_page:
  1. cookies  [{SESS<h>: *}, {SSESS<h>: *}]           -> applications/drupal/index
  2. headers  {Cookie: *SESS*}                         -> applications/drupal/index
  3. GET|HEAD, host, scheme, query "", Accept-Encoding *gzip*
         share <dir>/http/<host>${uri}_.html.gz, chroot, response_headers(+Content-Encoding)
         fallback -> applications/drupal/index
  4. GET|HEAD, host, scheme, query ""
         share <dir>/http/<host>${uri}_.html, types [text/html], chroot, response_headers
         fallback -> applications/drupal/index
  5. -> applications/drupal/index
```

Static assets are served by the docroot share first, so they cost no extra
`open()`. A page request costs one failed `open()` in the docroot and then
one `open()` in the cache. A miss adds a second failed `open()` before PHP.

`response_headers` reproduce what Drupal sends on every cacheable
anonymous page: `Cache-Control: max-age=<page max age>, public`,
`Vary: Cookie, Accept-Encoding`, the 1978 `Expires` that goes with
`Vary`, `Content-Language`, `X-Content-Type-Options`, `X-Frame-Options`,
and `X-Drupal-Cache: HIT-FREEUNIT` for observability. Drupal sets these in
`FinishResponseSubscriber` (lines 113-122, 245-273 on main).
`drush freeunit:routes` takes the values from a live anonymous response.

### 3.3 Writer (`StaticCacheWriter`, `kernel.terminate`, priority −100)

The rule is: **write exactly what page_cache has just stored, and only if
the router can reproduce it byte for byte, headers included.** In order:

1. The module is enabled. The method is `GET`. The user is anonymous. The
   request has no session (`SessionConfiguration::hasSession()`). The
   status is 200. The response is a `CacheableResponseInterface`, not a
   `BigPipeResponse` (its content holds placeholders), with
   `X-Drupal-Cache: MISS` (set only after `PageCache::storeResponse()`
   succeeds) and no `Set-Cookie`. The content type is `text/html*` and the
   body is at most `max_bytes`.
2. Every response header is either one the route reproduces or one on the
   drop list (`Date`, `ETag`, `Last-Modified`, `Link`, `X-Drupal-Cache-*`,
   `X-Generator`, …). **Anything else, and the page is not written**, so a
   module that adds a per-page header silently falls back to page_cache.
   A value that differs from the route's value (for example
   `Content-Language` on a multilingual site) is a v1 limitation; see
   §3.6.
3. The path maps to a file (`StaticCachePathMapper`). There is no query
   string. The host is in `static_cache.hosts`: a file written for an
   arbitrary `Host` header would never be served and would let clients fill
   the disk. The raw path uses only unreserved and sub-delim characters,
   with no `%`, no `//` and no dot segment, so FreeUnit's decoded `$uri`
   is byte-identical to it. Appending `_.html` makes the mapping injective
   and avoids file/directory clashes (`/node` → `node_.html`,
   `/node/` → `node/_.html`).
4. The `cache.page` item is still valid. PageCache's cid is protected, so
   the writer tries `"<scheme+host><uri>:"` and then
   `"…:<format>"`. If neither is valid, it does not write.
5. **Index before file.** The rows `(file, tag…, expire)` go into
   `freeunit_static_cache`, then `.gz` and `.html` are written, each as a
   temporary file `rename()`d into place (0644, directories 0755: the
   router reads as its own user).
6. **Re-check.** If the `cache.page` item is no longer valid, the tags were
   invalidated during the render or the write, so the file is deleted.

### 3.4 Invalidation (`StaticCacheTagsInvalidator`)

- It is a `cache_tags_invalidator` service, so it is called for every
  `Cache::invalidateTags()`. It looks up the files by tag, unlinks the
  `.html` and the `.gz`, and forgets the rows.
- **The second pass after commit.** Entity saves invalidate inside a
  transaction. A concurrent anonymous request can still read the
  pre-commit data and write its file after our delete. The invalidator
  records the tags and deletes again on `kernel.terminate` (−50, before
  the writer), which runs after the commit. Together with step 6 of the
  writer, the remaining window is a render that *started* before the
  commit, then passed its re-check after the commit, and yet read
  pre-commit data. That window is exactly as large as page_cache's own,
  because page_cache sets its item with the checksum as of `set()` time.
- `hook_cache_flush` (`drush cr`) → `purge()`: `rename()` the whole cache
  root aside (atomic), recreate it, then delete the old tree.
  `drupal_flush_all_caches()` calls `deleteAll()` on `cache.page`, which no
  tag invalidation reports, so this hook is required.
- `Expires`: page_cache honours a future `Expires`, and so does the index
  (`expire`). `hook_cron` deletes expired files, and the cron runs through
  the FreeUnit schedule.

### 3.5 Comparison with boost (from memory, not verified)

| | boost (8.x+) | freeunit |
|---|---|---|
| Who serves a hit | PHP: a `RequestEvent` subscriber reads the file (early, but the kernel still boots). Apache/nginx rewrite recipes can serve the files directly | The FreeUnit router, in C. No PHP process is involved |
| Invalidation | Expiry, cron, and the `expire` module | Drupal cache tags, synchronously, plus the after-commit second pass |
| Session bypass | In PHP | In the route: exact cookie names, plus the `*SESS*` net |
| gzip | Writes `.gz` | Writes `.gz` and serves it with the right headers (verified). Brotli needs F4 or another rule |
| Headers | Fixed | Reproduced by the route. A page with a header that cannot be reproduced is not written |

### 3.6 Known limits of v1

- **Multilingual sites.** `Content-Language` varies per page. v2 emits one
  rule per language path prefix (`uri: "/fr/*"` → `Content-Language: fr`).
  Negotiation by `Accept-Language` is unsafe for page_cache itself, and
  unsafe here too.
- **Query strings** are never cached (pagers, views exposed filters). A v2
  could key on the arguments FreeUnit exposes (`$arg_page`), but not on a
  raw query string: there is no `$args`/`$query_string` variable
  (`src/nxt_http_variables.c:59-125`), and `$request_uri` is not normalised,
  so it must not be used in a file path.
- **Multiple web nodes.** The files are local. Use purge/varnish or a CDN,
  or put the cache directory on shared storage and accept the invalidation
  fan-out.
- **Disk.** There is no cap in the prototype. v2: an LRU sweep on cron by
  `atime`, or a maximum file count in the index.
- **Accept-Encoding.** The rule matches `*gzip*`, so `gzip;q=0` also gets
  gzip. That is rare, and harmless for real browsers.

## 4. Design: cron through FreeUnit schedules (feature 2)

- The schedule (built by `ScheduleConfigGenerator`, applied by
  `drush freeunit:schedule --apply`):

  ```json
  "drupal-cron": {
      "pass": "applications/drupal/index",
      "uri": "/freeunit/cron",
      "interval": 300, "jitter": 30, "timeout": 240, "overlap": "skip",
      "headers": { "Host": "example.org", "X-FreeUnit-Cron-Key": "<state system.cron_key>" }
  }
  ```

- **Key handling.** The key is Drupal's own `system.cron_key`, so there is
  one key to rotate: rotate it in the UI, then run
  `drush freeunit:schedule --apply`. It travels in a header, so it is in
  neither FreeUnit's info log (which already redacts the URI after the
  last `/`, as observed: `GET /cron/... -> 204`) nor any access log. It
  lives in FreeUnit's configuration, which only root can read.
  `CronController` compares it with `hash_equals()`, requires a loopback
  `REMOTE_ADDR` (a run comes from 127.0.0.1, as observed), and answers 404
  otherwise.
- **The route is unreachable from outside.** A schedule's `pass` must be
  `applications/…` (`src/nxt_conf_validation.c`, `nxt_conf_vldt_schedule_pass`;
  observed: `routes/…` is rejected with a clear error). So the run never
  goes through the listener's routes, and the listener can answer 404 for
  `/freeunit/cron` to every client. Observed.
- **Synchronous cron.** The controller runs cron before it responds,
  unlike automated_cron. `overlap: skip` then really does skip overlapping
  runs (ADR 0004 R3). The run's status and duration in the FreeUnit log are
  cron's own.
- **Checks** (`ScheduleConfigGenerator::check()`): the live schedule equals
  the generated one (the key is never printed); `processes.max ≥ 2`
  (R5); the schedule timeout is at most the application's `limits.timeout`.
- **Control API facts, observed:** there is no PATCH (405). A PUT to
  `/config/schedules/x` answers 404 while `/config/schedules` does not
  exist, so `ControlApiClient::putPath()` creates the parent. A DELETE of
  `/config/schedules` removes every schedule.

## 5. Design: deployment hooks (feature 4)

- `drush deploy` → a post-command hook → `freeunit:restart` →
  `GET /control/applications/drupal/restart` (GET only,
  `src/nxt_controller.c:2485`). The router bumps the application
  generation, swaps in a new shared port, and QUITs the old prototype
  (`src/nxt_router.c:1646-1680`). New workers come from a new prototype,
  whose PHP startup gives a fresh OPcache and re-runs `opcache.preload`.
  Old workers finish their current request and are sent QUIT when they
  return (`:6026`, `:6218`). Observed: `{"success": "Ok"}`, and an unknown
  application answers 404.
- With restarts in place, production can use `opcache.validate_timestamps=0`
  in `options.file`.
- **Preload generator** (not in the skeleton): `drush freeunit:preload`
  writes `preload.php` with `opcache_compile_file()` over (a) the classes
  of the compiled container's service definitions and (b)
  `vendor/composer/autoload_classmap.php` limited to `Drupal\Core`,
  `Drupal\Component`, `Symfony\Component\{HttpKernel,HttpFoundation,
  DependencyInjection,EventDispatcher,Routing}` and `Twig`. Each class is
  kept only if its parents, interfaces and traits are in the set. Its
  `options.file` php.ini must set `opcache.preload` and
  `opcache.preload_user`, **not `options.admin`** (§0).
- **Access.** Only Drush uses the control socket, run as its owner (root
  in the dev kit). Making the socket group-writable for `www-data`
  (`--control-mode 0660 --control-group www-data`, `src/nxt_runtime.c:1072`)
  would let any PHP code rewrite FreeUnit's configuration, including
  application `user`, `isolation` and `processes`. The module never asks
  for that.

## 6. Observability (feature 5)

- **The Drupal side (v2).** A request subscriber at priority 1000 reads
  `HTTP_TRACEPARENT`. When `open-telemetry/sdk` is installed and
  `OTEL_EXPORTER_OTLP_ENDPOINT` is set, it starts a `SERVER` span with that
  remote parent, and ends it on `kernel.terminate`, so detached work is
  inside the span. Sampling follows the flags in `traceparent`. Without
  the SDK the subscriber does nothing.
- **Gander.** Gander makes its spans in the PHPUnit process after the page
  load. A core patch can take the main document's `traceparent` response
  header, which FreeUnit adds (`src/nxt_otel.c:219-229`), and use it as the
  remote parent of the `main` span. FreeUnit's router span and Gander's
  query and cache spans then share one trace. This needs F1 for the
  parent–child relation to be right when the test client sends its own
  `traceparent`.
- **Status.** `drush freeunit:status` covers running, idle and detached
  processes and active requests. The status report shows what it can see
  without the socket (§7, F3).

## 7. Other features (7a, 7b, 7e)

- **Pools (7a).** The `drupal` application gets `processes.max` sized for
  the front end and `limits.timeout: 30`. The `drupal_admin` application
  gets a small `max`, `limits.timeout: 600` and more memory. The listener
  sends `/admin/*`, `/batch*`, `/user/*`, `*/edit`, `/ai/*`,
  `/system/ajax*` and every non-GET request that carries a session cookie
  to `drupal_admin`. BigPipe and SSE streaming work in both pools. PHP
  writes each chunk to the router at once (`src/nxt_php_sapi.c:1387`,
  `src/nxt_unit.c:3173-3250`). With router compression, each chunk is
  sync-flushed (`src/nxt_zlib.c:63`), so streaming survives compression at
  some cost in ratio. BigPipe only uses `flush()`, which does not empty
  PHP's own output buffer, so `output_buffering` must be 0. That is the
  built-in default, but `php.ini-production` sets 4096.
- **Tuning (7b).** `max` ≈ cores × 1.5–2 for CPU-bound Drupal, capped by
  memory / `memory_limit`. `spare` ≥ 1 (a fork from the prototype is cheap,
  but not free). `limits.requests` of 500–2000 recycles leaky workers.
  Detached work (terminate, automated_cron) counts against `max`. The
  schedule timeout must not exceed `limits.timeout`.
- **Queues (7e).** A `drupal-queue-<name>` schedule calls
  `/freeunit/queue/<name>` on the admin pool with a time budget, for AI
  embedding or indexing queues. The Drupal AI facts here are not
  verified.

## 8. Required FreeUnit changes (separate list)

None of these block v1 of the module. Each one is a separate proposal.

| id | change | why | code areas | size |
|---|---|---|---|---|
| **F1** | When a valid inbound `traceparent` is inherited, forward the application **FreeUnit's own span id** as the parent-id, instead of the inbound value | So that application spans nest under FreeUnit's span. Today they are siblings | `src/nxt_otel.c:171-229` (`nxt_otel_propagate_header`: in the inherited branch, rewrite the request field's value with `nxt_otel_rs_copy_traceparent()`); tests `test/test_otel_traceparent_app*.py` (the inherited case must keep the trace id and get a new parent-id; the no-otel case stays byte-exact) | S |
| **F2** | **Internal redirect for application responses** (`X-Accel-Redirect`-style), opt-in per application: `applications.<app>.internal_redirect: {"header": "X-Accel-Redirect", "pass": "routes/internal"}` | Offload private downloads with Range, ETag and `sendfile` in the router; frees PHP workers | `src/nxt_http_response.c:23-35` (a field handler that records the value and skips the field); `src/nxt_router.c:5368-5590` (`nxt_router_response_ready_handler`: on status 200 with the header, drain the application body, finish the application side as a normal completion, keep `Content-Type`, `Content-Disposition` and `Cache-Control`, set the request's path to the header value, and run the configured internal route through `nxt_http_request_action()`, `src/nxt_http_request.c:612`); `src/nxt_http_route.c` (an `"internal": true` route that listeners cannot `pass` to); `src/nxt_conf_validation.c` (the option; `pass` must be an internal route); `src/nxt_http_static.c` (unchanged: Range, ETag and `sendfile` already work). On the Drupal side, Symfony's `BinaryFileResponse::trustXSendfileTypeHeader()` together with an `X-Sendfile-Type: X-Accel-Redirect` / `X-Accel-Mapping` request header already produce the header, so the module needs only a request subscriber | M |
| **F3** | Read-only status access: a second socket that serves only `GET /status…` (`--status unix:/path`, `--status-mode`, `--status-group`) | So the web status report can show FreeUnit processes and schedules without the configuration being writable | `src/nxt_runtime.c:1030-1140` (options), `src/nxt_controller.c:1236-1280` (request routing: on the status socket, refuse anything but `GET /status`) | S |
| **F4** | Precompressed variants for `share`: `"precompressed": ["br", "gzip"]` | One rule instead of one per encoding, the correct `Content-Type` from the original extension, and `Vary` added automatically. Today static compression re-compresses the file into a `mkstemp()` temporary file on every request (`src/nxt_http_compression.c:317-430`) | `src/nxt_http_static.c:495-560` (open `file.br`/`file.gz` according to `Accept-Encoding`), `:960-990` (skip dynamic compression), `src/nxt_http_compression.c:629-690` (reuse the `Vary` merge), `src/nxt_conf_validation.c:899-935` | S–M |
| **F5** | Route match for absent headers and cookies: a way to say "no header X", for example `"headers": {"Cookie": null}`, or document that a negation never matches an absent field | Today "no session cookie" cannot be expressed. It works only because bypass rules come first | `src/nxt_http_route.c:1962-1990`, `:2073-2118`, the validation of `match` (`src/nxt_conf_validation.c:796-850`) | S (doc) / M (feature) |
| **F6** | Per-schedule counters in `/status` (N4) | Show the cron schedule's health in `drush freeunit:status` and, with F3, in the status report | Designed in `docs/observability/status-extensions.md`; counters already exist in `nxt_router_schedule_state_t` (`src/nxt_router_schedule.c:38-57`) | S |
| **F7** | 103 Early Hints: a SAPI function `headers_send(int $status)`, a libunit "interim response" message, and a router that writes a 1xx and keeps waiting for the final response | Symfony `sendHeaders(103)` works as soon as the SAPI has `headers_send()` | `src/nxt_php_sapi.c:170-176` (the function table), `src/nxt_unit.c` (a new message type), `src/nxt_router.c:5368` (the first header message is currently final), `src/nxt_h1proto.c:1282-1419` (the 1xx status line already exists), `src/nxt_http_request.c:890` (1xx is already bodyless) | L |
| **F8** | (Long term) PHP worker mode, like FrankenPHP: a SAPI loop function that serves many requests per script execution | The biggest possible gain for dynamic pages. Symfony Runtime 8.1 already has a worker runner for FrankenPHP | `src/nxt_php_sapi.c` request lifecycle (`nxt_php_execute`, `:1233-1340`). **Drupal would have to be safe to reset between requests. Not verified, and probably a long way off** | XL |

Not a change request, but it should be written down: FreeUnit's SAPI
`name` is `"cli-server"` (`src/nxt_php_sapi.c:299`). Changing it could
break applications that rely on it. Instead, the FreeUnit documentation
should list the consequences, including Drupal's `.ht.router.php`.

## 9. Module architecture

```
freeunit/
  freeunit.info.yml          core_version_requirement: ^11.1 || ^12; depends on page_cache
  freeunit.services.yml      services below
  freeunit.routing.yml       /freeunit/cron (no_cache)
  freeunit.install           hook_schema (freeunit_static_cache), hook_requirements
  config/install|schema      freeunit.settings
  src/StaticCache/           PathMapper, Files, Index, RouteConfigGenerator
  src/EventSubscriber/       StaticCacheWriter (kernel.terminate −100)
  src/Cache/                 StaticCacheTagsInvalidator (cache_tags_invalidator, kernel.terminate −50)
  src/Control/               ControlApiClient (+ exception)
  src/Schedule/              ScheduleConfigGenerator
  src/Controller/            CronController
  src/Hook/                  FreeUnitHooks: cache_flush, cron
  src/Drush/Commands/        FreeUnitCommands
```

| kind | name | role |
|---|---|---|
| service | `freeunit.static_cache.path_mapper` | request → file; safe-path rules; host allowlist |
| service | `freeunit.static_cache.files` | atomic write, delete, purge |
| service | `freeunit.static_cache.index` | table `freeunit_static_cache (file, tag, expire)` |
| subscriber | `freeunit.static_cache.writer` | §3.3 |
| invalidator + subscriber | `freeunit.static_cache.invalidator` | §3.4 |
| service | `freeunit.control_api` | control API client (Drush only) |
| service | `freeunit.schedule_generator` | §4 |
| service | `freeunit.route_generator` | §3.2 |
| hook | `cache_flush`, `cron` | purge; delete expired files |
| drush | `freeunit:status`, `freeunit:schedule [--apply] [--host]`, `freeunit:routes [--host] [--apply]`, `freeunit:restart`, `freeunit:cache-purge`; a post-`deploy` hook; later `freeunit:preload` | control plane |
| config | `freeunit.settings`: `static_cache.{enabled, directory, hosts, gzip, max_bytes, reproduced_headers, dropped_headers}`, `control.{socket, application, target}`, `cron.{schedule_name, interval, jitter, timeout, host}` | |
| admin UI (minimal, not in the skeleton) | `/admin/config/development/freeunit`: a settings form for `static_cache.enabled`, `hosts`, `directory` and `gzip`. It shows, read-only, the route JSON and schedule JSON to apply and the Drush command that applies them. A "Purge static cache" button | The web UI never writes to the control socket |

## 10. Benchmark plan (dev kit image, `pkg/docker/drupal/`)

Setup: build `unit:…-drupal-core-dev-php8.5` (PHP 8.5 matches Drupal
12's requirement) and a Drupal `main` checkout with the `standard`
profile. Mount the module at `web/modules/custom/freeunit`, and generate
50 nodes and 10 users with `devel_generate`. Replace `unit-drupal.json`
with `examples/unit-drupal-freeunit.json` (host set to the benchmark host).
Pin the container to N CPUs (`--cpus`). Run the load generator on other
cores, or on another host. **SQLite** (the kit's default) is not
representative of authenticated or MISS numbers. Repeat those on MariaDB
10.11+ (Drupal 12's minimum).

| scenario | config | tool | metrics |
|---|---|---|---|
| A. anonymous, page_cache hit (baseline) | module off | `wrk -t4 -c64 -d60s --latency` on 20 node URLs (Lua script) | req/s, p50/p99, container CPU %, `/status` busy processes |
| B. anonymous, router hit | module on, warmed | same | same. Expect PHP CPU near 0 and busy processes at 0 |
| C. anonymous, gzip | B + `-H 'Accept-Encoding: gzip'` | same | bytes/s, req/s |
| D. authenticated | a session cookie per connection (the `wrk` script logs in 10 users) | same | req/s and p99. **Must equal A's authenticated run within noise**: the bypass rules cost microseconds. Check that every response has `X-Drupal-Cache` absent or MISS, never `HIT-FREEUNIT` |
| E. correctness under churn | B + `drush php:eval` saving a random node every 2 s | `wrk` plus a checker that fetches each node anonymously after each save | stale responses must be 0: title after save = title served |
| F. cron | automated_cron (interval 60) vs schedule (60 s) | `wrk --latency` for 10 minutes | p99/p999 of visitor latency; `/status` `detached` |
| G. deploy restart | `drush freeunit:restart` every 30 s during `wrk` | `wrk` | non-2xx must be 0; recovery time of req/s |
| H. validate_timestamps | 1 vs 0 (with G's restart) | `wrk` on D and on MISS URLs (`?nocache=<rand>`) | req/s |
| I. Gander | core `PerformanceTestTrait` tests, with `OTEL_COLLECTOR` pointing at the kit's collector | `drupal-dev.sh test --group OpenTelemetry` (or core's performance tests) | query and cache counts on the MISS path must not change with the module on (the writer runs in terminate; its DB writes are extra and should be reported). Wall time |
| J. pools (7a) | one vs two applications | `wrk` on the front + a slow admin loop (`/batch`) | front p99 |

`ab -k -n 100000 -c 64` is fine for a quick A/B. `h2load` can be used
when TLS is on. Numbers observed so far are on a scratch `unitd` only
(§2, feature 1). They say that the router serves a file about 5× faster
than PHP runs *an empty script*. That is a lower bound for the gain over
Drupal.

## 11. Risks

1. **An authenticated user gets an anonymous page.** This happens only if
   both bypass rules miss: a session cookie with a name the generator did
   not foresee (a changed `cookie_domain` or `name_suffix`, or a base path)
   *and* a split Cookie line. Mitigations: `drush freeunit:routes`
   recomputes the names from the live session configuration; the status
   report compares them. Worst case, the user sees anonymous content: no
   private data can leak, because only anonymous responses are ever
   written.
2. **Stale pages.** The invalidation race (§3.4) is no wider than
   page_cache's own. A module that invalidates caches without tags (direct
   `cache.page->deleteAll()`) is covered only by `hook_cache_flush`, or
   not at all if it calls the bin directly.
3. **Headers that differ per page.** Handled by refusing to write, which
   costs hit ratio, not correctness. Multilingual sites are the main case.
4. **Disk growth and inode use**, and a cache directory filled through
   many distinct valid URLs. Host allowlist, max size per page, and
   planned: an LRU sweep and a count cap.
5. **The control socket.** Only Drush uses it. The documentation forbids
   making it writable to `www-data`.
6. **Schedules are new (ADR 0004).** R2 (engine removal), R3 (detached
   work) and R7 (state lost on restart) apply. The cron controller is
   synchronous to avoid R3.
7. **`cli-server` SAPI name.** `.ht.router.php` and any contrib code that
   treats `cli-server` as the development server. Block `*/.ht*` in every
   configuration.
8. **Symfony Runtime debug.** With `APP_DEBUG`, `HttpKernelRunner` does not
   call `fastcgi_finish_request()`, so terminate work, including the
   writer, delays the response.
9. **Unverified Drupal facts** (§1): the contrib landscape, the Drupal 12
   release date, the AI initiative, Drush 13 attribute details, and the
   exact behaviour of `SessionConfiguration::getOptions()` for base paths.
10. **The prototype was never run in Drupal.** The service wiring, the
    hook discovery and the Drush command discovery are untested.

## 12. Phased plan

DrupalCon Rotterdam: 28 September to 1 October 2026 (per search). The
brief's "3 days from now" gives a freeze on Friday 26 September.

| when | work | exit criterion |
|---|---|---|
| **Day 1 (Wed 23)**, done | This document; the skeleton; the FreeUnit route and schedule validated on a local `unitd` | `check-static-cache.py` passes |
| **Day 2 (Thu 24)** | Install on a real Drupal 11.x site in the dev kit (the network allows packagist there). Fix service wiring. Run scenario E. Implement `freeunit:routes` from a live response | Writer and invalidator pass E with 0 stale |
| **Day 3 (Fri 25)** | Benchmarks A–D, F, G on the dev kit, on 11.x and on `main` (12.0-dev). Short README numbers. Demo script: cron via schedule plus router hits, with `/status` live | Numbers in this document; a demo that runs twice cleanly |
| **Freeze (Fri 26)** | Tag the prototype; slides with the §2 table and the measured numbers | — |
| **DrupalCon (28 Sep – 1 Oct)** | Demo at the FreeUnit/Drupal BoF; contribution day (1 Oct): open the `drupal.org` project as a sandbox; gather feedback on the Gander `traceparent` patch and on `cli-server` | Issues filed |
| **After, weeks 1–2** | FreeUnit F1 (OTel parent) and F3 (status socket); multilingual rules; LRU cap; Gander patch upstream | F1/F3 merged with tests |
| **After, weeks 3–6** | F2 (internal redirect) with the private-file subscriber; F4 (precompressed); preload generator with measurements | F2 benchmark: workers freed during downloads |
| **Later** | F7 (Early Hints); evaluation of F8 (worker mode) against Drupal's resettability | ADRs |
