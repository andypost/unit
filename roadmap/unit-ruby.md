# FreeUnit Ruby — State & Roadmap

Ruby moves on an annual Christmas-day release cadence (3.3 in 2023, 3.4 in 2024, 3.5 in 2025). Each recent release has shipped major runtime features (Ractors, Fiber scheduler, YJIT, Prism parser) that an app server can exploit — and that Unit's module currently ignores.

## Current state

| File | LoC | Responsibility |
|---|---|---|
| `src/ruby/nxt_ruby.c` | 1508 | SAPI: init, Rack bridge, env hash, response dispatch, lifecycle |
| `src/ruby/nxt_ruby_stream_io.c` | 287 | `rack.input` / `rack.errors` IO shim |

`auto/modules/ruby` probes via the `ruby` executable's `RbConfig` rather than pkg-config: reads `rubyhdrdir`, `rubyarchhdrdir`, `RUBY_SO_NAME`, etc. Requires libruby to be linkable (`-lruby-X.Y`).

### Execution model

- **Rack-only.** The module speaks Rack 1.x/2.x/3.x. Startup loads a `.ru` file via `Rack::Builder.parse_file`. No non-Rack entry points.
- **One request per process at a time.** No thread pool, no Ractors, no Fiber scheduler — plain blocking dispatch. Scaling is horizontal across processes via Unit's prefork (`processes: { … }`).
- **Rackup discovery** (`nxt_ruby.c:331`): rackup is built once at startup, stored in `nxt_ruby_rackup`, reused for every request.
- **Isolation:** inherits Unit's full block (namespaces, rootfs, cgroups, user).
- **Hooks:** `hooks` config lets users run Ruby code at specific lifecycle points (`test/test_ruby_hooks.py`).
- **Version support:** CI exercises modern 3.x; Docker ships 3.3 and 3.4 variants.

### Configuration probes

`auto/modules/ruby` computes include/lib paths from the target `ruby` binary's `RbConfig`. There is an **acknowledged bug** baked into the script as a TODO (visible in the module): on Debian/Ubuntu with multiarch layout, `RbConfig["libdir"]` reports `/usr/lib` but the actual `libruby-X.Y.so` lives in `/usr/lib/<triplet>/`. The two-pass check misses it. Fix: probe `dpkg-architecture -q DEB_HOST_MULTIARCH`.

### Known gaps

1. **No threads.** Unlike Python (`threads: N`), Ruby runs one request at a time per process. MRI has a GVL, but thread-based concurrency still wins for I/O-bound workloads — this is table-stakes that Puma had a decade ago.
2. **No Ractors (3.0+).** Ruby's actual parallelism primitive — independent GVLs per Ractor — is not exploited. This is the Ruby equivalent of Python's subinterpreters and it has been stable longer.
3. **No Fiber scheduler integration (3.1+).** Ruby 3.1's `Fiber::SchedulerInterface` lets I/O auto-yield at the kernel level; libraries like `async` use it. Unit doesn't plug its event engine in as a scheduler, so async frameworks gain nothing running under Unit.
4. **No YJIT awareness.** Users can `RUBY_YJIT_ENABLE=1`, but Unit doesn't tune YJIT defaults per workload, doesn't report YJIT stats, doesn't warn when YJIT is unavailable on this build.
5. **No preload/warmup.** Rails' full app graph loads lazily per worker. Cold-start cliff.
6. **Bundler-unaware.** If `Gemfile` is present next to the `.ru` file, Unit doesn't auto-activate the bundle. Users must set `BUNDLE_GEMFILE` and rely on `bundler/setup` at the top of their rackup.
7. **No `tmp/restart.txt` compatibility.** Rails/Passenger convention; would make deploy-triggered graceful reload a one-line operation for Rails users.
8. **No Sidekiq/rake CLI path.** Same scheduler gap as PHP/Python.
9. **No status surface.** GC stats, ObjectSpace counts, YJIT compile stats, thread counts, Ractor counts — none of it is exposed by Unit.
10. **Multiarch libdir probe bug** (documented in `auto/modules/ruby` TODO).
11. **Rack 3 streaming body** (`rack.response.finished`, `Rack::Response#each` returning `to_proc`-style streams) — need audit against current implementation in `nxt_ruby_rack_result_body_each`.
12. **No rbenv/asdf hint.** The module assumes the build-time ruby is the runtime one. Fine for Docker, awkward for multi-version hosts.

---

## Roadmap

### Near term (1–3 months)

**P1. Thread pool per worker (`threads: N`).**
- Create N Ruby threads at startup, each blocked on a request queue. Use `rb_thread_create` / `rb_thread_call_without_gvl` correctly around the request dispatch path.
- Despite the GVL, I/O-bound workloads (most Rails apps) see large throughput gains because `IO#read`, DB driver calls, etc. release the GVL.
- Rack apps are not guaranteed thread-safe; default `threads: 1` for backwards compat, require explicit opt-in. Document `config.threadsafe!` expectations.
- **Wins:** parity with Puma's default mode; fewer processes needed for the same throughput; smaller memory footprint per request.
- **Effort:** ~2 weeks.

**P2. Multiarch libdir probe fix.**
- Land the fix noted in `auto/modules/ruby`: consult `dpkg-architecture -q DEB_HOST_MULTIARCH` (and `ldconfig -p`) before giving up on `libdir`.
- Add a fallback that runs `ruby -rfiddle -e 'p Fiddle.dlopen(nil)'` to confirm libruby is actually linkable.
- **Effort:** 2 days.

**P3. Preload/warmup hook.**
- `"preload": true` for a Rails app runs `require 'config/environment'` (or user-specified entry) before accepting requests.
- Fork-after-require pattern for memory COW savings on Linux.
- Publish as `preset: "rails"` config sugar that infers preload paths.
- **Effort:** 3 days.

**P4. Ruby status API.**
- `/status/applications/<name>/ruby`: `GC.stat`, `GC.latest_gc_info`, `ObjectSpace.count_objects`, YJIT stats (when enabled), thread count, per-thread state.
- OpenTelemetry span per request with GC pause counters.
- **Effort:** ~1 week.

### Mid term (3–6 months)

**P5. Fiber scheduler integration.**
- Register Unit's event engine as a `Fiber.set_scheduler`. I/O inside Rack handlers (with `async` or `falcon`-style apps) auto-yields to Unit's epoll/kqueue loop instead of a Ruby-level scheduler.
- Complements P1: threads for parallelism, fibers for I/O concurrency inside each thread.
- **Wins:** makes Unit the most natural Ruby host for async-aware code; Falcon-grade throughput without Falcon.
- **Effort:** ~4 weeks. Needs careful handling of `rb_thread_call_without_gvl` interaction.

**P6. Ractor-based worker mode.**
- `"ractors": N` spawns N Ractors per process. Each Ractor is isolated (can't share mutable state) — true parallelism without multiple processes.
- Not every Rack app is Ractor-safe (shareable constants restriction). Default off, require opt-in, document compatibility.
- **Wins:** scaling knob for Ractor-ready apps; a Ruby-3-native answer to Python subinterpreters.
- **Effort:** ~3–4 weeks. Lots of extension-compat triage.

**P7. Graceful reload (Rails-compatible).**
- Watch `tmp/restart.txt` mtime (the Passenger/Phusion convention) — on change, spawn fresh workers, drain old.
- Also exposes `POST /control/applications/<name>/reload` like PHP/Python roadmaps.
- **Wins:** zero-config Rails deploy reload; drop-in replacement UX for Passenger users.
- **Effort:** ~2 weeks.

**P8. Bundler-aware launcher.**
- If `Gemfile` exists next to the rackup, `require 'bundler/setup'` in the right order and activate the correct bundle. Export `BUNDLE_GEMFILE`, respect `BUNDLE_PATH`.
- Compatible with `rbenv`/`asdf` layout when configured binary matches.
- **Effort:** ~1 week.

**P9. YJIT-aware tuning.**
- Probe YJIT availability in `auto/modules/ruby`; expose `yjit: {enable: true, call_threshold: N}` in config.
- At runtime, call `RubyVM::YJIT.enable` on worker start; surface `YJIT.stats` via P4.
- **Effort:** ~3 days.

**P10. Scheduler integration.**
- Uses the primitive from `unit-cron.md`. Ruby presets: `rake:*`, `sidekiq`, `rails:*` (e.g. `rails db:migrate`, `rails runner`).
- Sidekiq specifically: let users run the Sidekiq worker as a Unit-managed scheduled process so one Unit replaces web + worker supervisor.
- **Effort:** see `unit-cron.md`.

### Long term (6–12 months)

**P11. Rack 3.x compliance audit and Rack 4 prep.**
- Systematic test of Rack 3 streaming semantics, `rack.response.finished`, `rack.hijack`, early hints (103).
- Track Rack 4 proposals so FreeUnit is the first server to support them.
- **Effort:** ~2 weeks.

**P12. Ruby-native event loop (like Python P10, PHP P8).**
- Skip the Ruby-level scheduler layer, drive Fibers directly from Unit's engine. Much deeper than P5.
- Probably unnecessary once P5 lands well. Keep as an option if measurements justify it.
- **Effort:** ~2 months.

**P13. mruby or `ruby-wasm` target spike.**
- Unit's WASI support could host `ruby-wasm` components for per-request isolation. Exploratory.
- **Effort:** 3 weeks spike.

**P14. CI matrix.**
- `{3.2, 3.3, 3.4, 3.5, ruby-head} × {YJIT on, YJIT off} × {threads 1, threads 8} × {Rack 2, Rack 3}`.
- Test against Rails `main` weekly.
- **Effort:** ~1 week + ongoing.

---

## Short roadmap table

| # | Item | Effort | Ship window |
|---|------|--------|-------------|
| P1 | Thread pool per worker | 2w | Near |
| P2 | Multiarch libdir probe fix | 2d | Near |
| P3 | Preload / warmup | 3d | Near |
| P4 | Ruby status API (GC/YJIT/threads) | 1w | Near |
| P5 | Fiber scheduler integration | 4w | Mid |
| P6 | Ractor worker mode | 3–4w | Mid |
| P7 | Graceful reload (tmp/restart.txt) | 2w | Mid |
| P8 | Bundler-aware launcher | 1w | Mid |
| P9 | YJIT awareness & tuning | 3d | Mid |
| P10 | Scheduler (rake/Sidekiq/rails) | see unit-cron | Mid |
| P11 | Rack 3.x audit + Rack 4 prep | 2w | Long |
| P12 | Unit-native Fiber loop | ~2m | Long |
| P13 | ruby-wasm target spike | 3w | Long |
| P14 | CI matrix expansion | 1w + ongoing | Long |

**Headline bets:** P1 (threads), P5 (Fiber scheduler), and P6 (Ractors) together turn Unit's Ruby story from "prefork like 2015" into "Ruby 3.x-native app server." That's the positioning that justifies this fork for Ruby users.

---

## Cross-cutting (Python + Ruby + PHP)

The scheduler (`unit-cron.md`), graceful reload endpoint, preload/warmup pattern, status-API layout, OpenTelemetry span conventions, and persistent-worker mode are **all the same feature four times**. Design them generically in the router/libunit layer and have each SAPI implement thin hooks. Otherwise you'll ship three slightly-different reload endpoints and regret it.
