# maintainer.md

## Scope

This file summarizes maintainer-facing findings, priorities, and implementation ideas for **FreeUnit**, based on the public roadmap branch at:

- https://github.com/andypost/unit/tree/roadmap/roadmap

## Important limitation

I could **not directly access or transcribe the Telegram voice messages** from the provided `t.me` links in this environment.

Because of that, this file is a **maintainer-oriented synthesis of the GitHub roadmap and prior technical analysis**, plus a clearly marked section for **Telegram-specific ideas pending transcript verification**.

If voice-message transcripts are provided later, this file should be updated and split into:

- confirmed from roadmap/docs
- confirmed from Telegram messages
- inferred maintainer recommendations

---

## One-sentence maintainer thesis

**FreeUnit should avoid becoming three separate partially-custom app servers and instead ship a small set of core/libunit primitives once, then let PHP, Python, and Ruby plug into them with thin hooks.**

That is the most important architectural idea in the roadmap and the main condition for long-term maintainability.

---

## What the project is trying to become

FreeUnit is not just “archived Unit, kept alive”.

The roadmap positions it as:

- an actively maintained fork of NGINX Unit,
- an embedded multi-language app server/runtime platform,
- a serious home for modern **PHP 8.4/8.5**, **Python 3.13+**, and **Ruby 3.x**,
- with first-class operational features:
  - preload/warmup,
  - status API,
  - graceful reload,
  - scheduler,
  - structured logging,
  - OpenTelemetry,
  - Prometheus metrics,
  - packaging and support policy.

This is a platform strategy, not a patch queue.

---

## The central engineering decision

The repeated capabilities across PHP / Python / Ruby / scheduler should be implemented **once** in core:

- preload / warmup
- status API
- graceful reload
- persistent-worker contract
- scheduler primitive
- per-target env / path overrides
- OpenTelemetry conventions
- metrics endpoint

Maintainer rule:

> If a feature appears in more than one language roadmap, default to designing it in router/controller/libunit first.

Avoid language-specific drift unless the runtime truly requires it.

---

## Recommended maintainer priorities

### Priority 0 — keep the fork credible

These items should land before big language-specific bets:

1. armv7 / armhf alignment and SIGBUS fixes
2. better config validation and error messages
3. core graceful-shutdown correctness
4. support matrix / security policy / release process
5. CI expansion
6. packaging plan

Reason:
- they reduce operational risk,
- unblock later roadmap work,
- make the fork trustworthy to users and contributors.

---

## Critical path as maintainers should see it

### Phase A — foundation and debt reduction

- Fix 32-bit ARM alignment issues
- Improve config validation
- Finish graceful shutdown / lifecycle correctness
- Reduce active support burden from very old language minors
- Decide what to do with weakly maintained TLS backends outside OpenSSL

### Phase B — cross-cutting primitives

- unified preload/warmup contract
- unified status API schema
- per-target env/path overrides
- scheduler primitive phase 1 (`/run`)
- OTel span conventions
- structured logs

### Phase C — prove value in real workloads

Focus first on:
- PHP preload + status + per-target config + ZTS worker mode
- Python preload + status + venv-aware launch + 3.13t support

Ruby should follow the same primitives, but not define the first 6 months.

### Phase D — lifecycle and operations

- graceful reload
- scheduler config + overlap policies + retries + status
- Prometheus metrics
- systemd support
- migration guides
- packages

### Phase E — deeper differentiators

- PHP persistent worker mode
- Python subinterpreters
- Ruby thread pool / Bundler / reload ergonomics
- HTTP/2 once the platform basics are stable

---

## High-confidence findings from the roadmap

### 1. Shared primitives matter more than language-specific features

The roadmap itself already says the same feature keeps reappearing across languages. This is not just a nice design preference; it is a maintenance survival requirement.

Without shared primitives, the fork will accumulate three different:
- reload behaviors,
- preload mechanisms,
- observability models,
- scheduler implementations,
- config schemas.

That would likely become unmaintainable.

### 2. Graceful reload depends on deeper lifecycle work

The public TODO/debt inventory suggests graceful reload is not just a control endpoint to add.
It depends on:
- proper graceful shutdown,
- worker draining,
- generation handoff correctness,
- event-engine shutdown behavior.

Maintainers should treat graceful reload as a **lifecycle milestone**, not just a feature ticket.

### 3. HTTP/2 is strategically important but dangerous to start too early

HTTP/2 has very high platform value, but also large scope and delay risk.
It should stay on the roadmap, but it should not consume the project before:
- reload,
- observability,
- scheduler primitive,
- core lifecycle correctness,
- and at least one successful modern-language concurrency story.

### 4. Scheduler is not a side feature

Running scheduled tasks inside Unit is strategically strong because it unifies:
- runtime,
- isolation,
- rootfs,
- user,
- cgroups,
- telemetry,
- logging.

That can replace a lot of fragile “cron + docker exec + manual env replication” setups.

### 5. Python may become the cleanest technical flagship

Python is well positioned because Unit already has threads and ASGI support.
Adding support for:
- free-threaded 3.13t,
- subinterpreters,
- venv-aware startup,
- preload,
can make FreeUnit especially compelling for modern Python workloads.

### 6. PHP is likely the market flagship

For adoption, PHP may matter the most.
The strongest differentiators are:
- ZTS worker-pool mode,
- preload/warmup,
- per-target config,
- graceful reload,
- persistent worker mode,
- scheduler for drush/artisan.

If these land reliably, FreeUnit gets a much stronger PHP story than “keeps working after upstream stopped”.

### 7. Ruby has upside, but should not dominate the early roadmap

Ruby’s long-term story is strong:
- threads,
- Fiber scheduler,
- Ractors,
- YJIT,
- Rails-compatible reload.

But early value likely comes from simpler improvements:
- preload,
- Bundler-aware startup,
- status API,
- thread pool,
- `tmp/restart.txt` reload compatibility.

---

## Maintainer recommendations by subsystem

## Core / daemon

### Must do early
- arm32 alignment audit and fix
- graceful shutdown implementation
- config validation improvements
- structured logs
- JSON Patch / Merge Patch support
- systemd socket activation

### Should do after basics
- control API auth
- fuzzing coverage expansion
- body streaming audit

### Delay until core is stable
- full HTTP/2 implementation

---

## PHP track

### Best early sequence
1. preload/warmup
2. status API
3. per-target ini/env overrides
4. ZTS thread-per-request mode
5. graceful reload
6. scheduler integration
7. persistent worker mode
8. Fiber/event-loop bridge

### Main risks
- extension safety under ZTS
- request-state reset correctness in persistent-worker mode
- test matrix complexity

### Maintainer note
Persistent workers are highly valuable, but riskier than they look.
Do not ship them without strong reset semantics and observability.

---

## Python track

### Best early sequence
1. preload/warmup
2. status API
3. venv-aware launcher
4. free-threaded 3.13t support
5. subinterpreters
6. graceful reload
7. scheduler integration
8. unit-native event loop

### Main risks
- C-extension compatibility for no-GIL and per-interpreter execution
- subtle runtime assumptions around GIL state
- test coverage across 3.13t and pre-release versions

### Maintainer note
Python may be the strongest place to prove that FreeUnit is aligned with where runtimes are going, not where they were.

---

## Ruby track

### Best early sequence
1. multiarch build fix
2. preload/warmup
3. status API
4. Bundler-aware launcher
5. thread pool
6. graceful reload (`tmp/restart.txt`)
7. YJIT awareness
8. Fiber scheduler
9. Ractors

### Main risks
- app thread-safety assumptions
- native extension behavior
- complexity of Fiber scheduler integration with Unit’s event engine
- Ractor compatibility limits

### Maintainer note
Ruby should benefit from the common core work first; deeper runtime innovation can follow once the project has more implementation confidence.

---

## Scheduler / task execution

### Recommended path
1. ship `POST /control/applications/<name>/run`
2. add `schedules` config
3. add overlap policy / retries / status ring buffer / metrics / OTel
4. add language-specific presets:
   - drupal / drush
   - laravel / artisan
   - django / manage.py
   - rails / rake / sidekiq

### Why this matters
This can turn FreeUnit into more than a web app server:
it becomes a runtime supervisor for recurring framework-native tasks.

---

## Governance and project hygiene

These are not optional docs chores.
They are part of the product.

### Must publish early
- `SUPPORT.md`
- `SECURITY.md`
- `RELEASE-PROCESS.md`

### Must operationalize early
- public CI matrix
- package distribution plan
- documentation site / architecture docs
- migration guides from major alternatives
- cherry-pick / upstream-patch tracking

### Branding guidance
Be honest:
- keep `nxt_` internally where renaming is too costly,
- rebrand user-facing docs/log strings deliberately,
- do not pretend it is not a Unit fork,
- do make it clear that FreeUnit is the active maintained project.

---

## Technical debt that likely deserves explicit maintainer issues

Create or confirm tracked issues for:

- graceful shutdown / reload prerequisites
- old language-version support burden
- alternative TLS backend policy
- body streaming limitations
- HTTP/2 design scope and staging plan
- scheduler ABI design
- persistent-worker request-state reset requirements
- extension-compatibility matrices:
  - PHP ZTS
  - Python no-GIL / subinterpreters
  - Ruby threads / Ractors

---

## Suggested maintainer rules of thumb

### Rule 1
Do not add a language feature until the core hook that supports it is named and documented.

### Rule 2
Every concurrency feature must ship with:
- status visibility,
- clear warnings for incompatible extensions,
- CI coverage,
- rollback path.

### Rule 3
Every control-plane feature should prefer composable primitives first, sugar second.

### Rule 4
Every roadmap item should identify whether it changes:
- ABI,
- config schema,
- runtime semantics,
- deploy procedure,
- observability output.

### Rule 5
Do not let HTTP/2 starve the rest of the platform roadmap.

---

## Proposed near-term maintainer backlog

### First wave
- D1 armv7 fix
- D5 config validation improvements
- graceful shutdown core work
- X1 preload contract
- X2 status schema
- X6 per-target env/path overrides
- G1 support matrix
- G2 security process

### Second wave
- D8 structured logs
- X5 scheduler phase 1 (`/run`)
- PHP preload + status
- Python preload + status + venv handling
- G4 CI matrix

### Third wave
- X3 graceful reload
- X5 scheduler phase 2
- PHP ZTS worker mode
- Python 3.13t support
- G5 packaging

### Fourth wave
- X4 persistent-worker contract
- PHP persistent worker
- Python subinterpreters
- Ruby thread pool / Bundler / reload
- X8 metrics
- D9 systemd

---

## Telegram voice messages — pending verification

I could not access the Telegram voice messages directly, so the following section is intentionally conservative.

### Items to verify against the Telegram messages once transcripts exist

Check whether the voice messages add or change any of the following:

- stronger emphasis on PHP vs Python vs Ruby priorities
- opinions on whether TrueAsync stays experimental or becomes strategic
- maintainer appetite for HTTP/2 timeline
- packaging priorities by distro
- whether scheduler is intended as a flagship feature or a utility
- how aggressive version support drops should be
- whether the fork intends to reduce scope for some languages
- whether docs/migration guides are meant to land much earlier
- any funding, staffing, or contributor constraints affecting sequencing
- any explicit promises the roadmap docs should avoid making

### Recommended follow-up once transcripts are available

Update this file with three labels on each bullet:
- **confirmed from roadmap**
- **confirmed from Telegram**
- **maintainer inference**

That separation will make future roadmap decisions much easier.

---

## Bottom line for maintainers

FreeUnit can become a credible long-term project if it stays disciplined about sequence:

1. stabilize the core,
2. design shared primitives once,
3. prove value in PHP and Python first,
4. make operations/packaging/docs trustworthy,
5. only then push the deeper concurrency and protocol ambitions.

The project’s biggest risk is not lack of ideas.
It is shipping too many ideas before the common platform underneath them is solid.
