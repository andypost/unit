# Plan: malloc-failure injection test harness

## Context

Both PR #6 (TLS OCSP stapling) and PR #8 (cert/script IPC leak fixes) closed
leaks that are reachable only when `malloc()` (or `nxt_port_msg_alloc()`,
which is a `malloc` wrapper) returns `NULL`. Today there's no way to drive
that path in CI, so the fixes are review-verified but not regression-fenced.
The next time someone refactors the cert/script/OCSP IPC the leak can come
back silently.

This PR adds a small allocator fault-injection facility specifically for
pytest, with the OCSP and cert-store reply paths as the first consumers.

## Goal

A pytest fixture that lets a test say:

```python
@pytest.mark.malloc_inject
def test_cert_get_handler_send_failure(malloc_inject):
    """If nxt_port_msg_alloc fails inside chk_insert during the cert reply,
    the cert fd in the main process must not leak."""

    fd_before = count_main_process_fds()

    with malloc_inject(fail_when='nxt_port_msg_alloc',
                       call_count=2,
                       then='succeed'):
        # Trigger a cert-store-get that drives the receiver into the
        # failing send path.  Use the existing /certificates upload +
        # listener add flow.
        upload_cert_bundle(name='leak_test')
        apply_listener({'tls': {'certificate': 'leak_test'}})

    fd_after = count_main_process_fds()
    assert fd_after == fd_before, "main-process fd leaked on send failure"
```

Out of scope for this PR: actually building any fault-injection backend
beyond what's needed for the cert/script/OCSP tests. Keep it minimal,
extensible later.

## Design choices

### Backend: `LD_PRELOAD` shim, not `malloc_hook`, not allocator patches

Three options were considered:

| Option | Pros | Cons |
|---|---|---|
| `LD_PRELOAD=malloc_inject.so` | Zero source changes; works with system Unit binary; isolated per-test | Linux-only; needs careful pthread/glibc interaction |
| glibc `__malloc_hook` (deprecated) | Simple API | Removed from glibc 2.34+ |
| Wrap `nxt_malloc` in `src/nxt_malloc.{c,h}` with a build-time toggle | Cross-platform; deterministic | Touches the codebase; tests would need a special build |

**Pick `LD_PRELOAD`.** Linux-only is acceptable for an internal test
harness (Unit's CI is Linux already). It keeps the Unit binary unchanged
and matches what most production-grade fault-injection libraries
(`libfiu`, `libfaketime`, `libfailmalloc`) do.

### Counter semantics: per-symbol, per-test

The shim exports a control interface via two environment variables and a
named-pipe / unix-socket, so a pytest harness can reconfigure injection
between test phases:

```
MALLOC_INJECT_TARGETS    "malloc:5,nxt_port_msg_alloc:2"
MALLOC_INJECT_CONTROL    "/tmp/malloc-inject-<pid>.sock"
```

Format: `<symbol>:<call-number-to-fail>` — fail the Nth call to that symbol
for this process. Subsequent calls succeed normally. Tests run one targeted
failure per assertion.

If `MALLOC_INJECT_TARGETS` is unset, the shim is a no-op.

### Symbol coverage

For these PRs only two need wrappers; design extensibility but ship
narrowly:

- `malloc` — covers `nxt_port_msg_alloc` (which calls `nxt_malloc` → `malloc`)
  and most of `nxt_mp_alloc`'s lazy-region allocations.
- `mmap` — covers `nxt_port_mmap_*` paths if they get added later.
- `posix_memalign` — covers aligned allocations in `nxt_buf_mem_alloc` etc.

For PR scope: ship only `malloc` + `posix_memalign`. The rest can come as
needed.

### Per-symbol vs per-call-site

Failing "the second call to malloc anywhere in the process" is too coarse
— Unit calls malloc thousands of times per request. The shim should
support targeting via a stack-walk filter:

```
MALLOC_INJECT_TARGETS  "malloc@nxt_port_msg_alloc:1"
```

Implementation: when targeted-mode is active, walk one frame up the
stack with `__builtin_return_address(1)` and resolve via `dladdr()`; if
the symbol matches, count and possibly fail. This costs ~1 µs per call
in injection mode, zero when disabled (the shim early-returns when
`MALLOC_INJECT_TARGETS` is empty).

## Layout

```
tools/
└── malloc_inject/
    ├── README.md
    ├── Makefile
    ├── malloc_inject.c          # the LD_PRELOAD shim
    └── malloc_inject_test.c     # standalone unit test for the shim itself

test/
├── conftest.py                  # add the malloc_inject fixture
└── test_cert_store_inject.py    # first consumer; tests the PR #8 + PR #6 leaks
```

The shim builds with the regular Unit toolchain:

```
$(CC) -shared -fPIC -ldl -o build/malloc_inject.so \
    tools/malloc_inject/malloc_inject.c
```

`Makefile` integration: add a `make malloc-inject` target that's not part
of the default build. CI invokes it explicitly before running the
`test_*_inject.py` tests.

## Pytest integration

`test/conftest.py` gets:

```python
@pytest.fixture
def malloc_inject(unit, tmp_path):
    """Yield a context manager that activates allocator fault injection
    against the running Unit instance.  Requires the Unit binary to have
    been started under LD_PRELOAD=build/malloc_inject.so (handled by the
    `unit` fixture when the test is decorated with @pytest.mark.malloc_inject).
    """
    sock = tmp_path / 'malloc-inject.sock'

    @contextmanager
    def _activate(fail_when, call_count, then='succeed'):
        spec = f'{fail_when}:{call_count}'
        _send_to_shim(unit.pid, spec)
        try:
            yield
        finally:
            _send_to_shim(unit.pid, 'reset')

    yield _activate
```

The `unit` fixture's `start()` path checks for the `malloc_inject` marker
on the requesting test and adds `LD_PRELOAD=$BUILD/malloc_inject.so` to
the spawned daemon's env if present. Tests without the marker run
unchanged.

## First test consumers

Three concrete tests to ship in this PR:

### 1. `test_cert_send_failure_no_fd_leak`

Force `malloc` to fail at the second call inside `nxt_port_msg_alloc`,
then upload a cert and apply a listener that references it. Assert that
`/proc/$main_pid/fd` does not grow.

### 2. `test_script_send_failure_no_mp_leak`

Same shape but for `nxt_script_store_get`'s sender side. The leak is mp-
pool retain rather than fd, so the assertion is "router process VSZ does
not grow by more than the new app's actual config size after the failed
apply."

### 3. `test_ocsp_send_failure_no_fd_leak`

OCSP twin of test 1 — drive `nxt_cert_store_get_ocsp` past the
`malloc`-fail point in `nxt_port_msg_alloc`, assert main-process fd
count is stable after multiple retries.

These three tests together fence both PR #6 and PR #8.

## CI integration

```yaml
# .github/workflows/malloc-inject.yml  (new)
name: malloc-inject

on: [pull_request, push]

jobs:
  inject:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: ./configure --openssl
      - run: ./configure python --config=python3-config
      - run: make -j$(nproc)
      - run: make malloc-inject
      - run: pytest test/test_cert_store_inject.py -v
```

Doesn't replace the regular `test` matrix — runs separately so a shim
regression is visible as its own red check.

## Effort estimate

- Shim implementation (`malloc_inject.c`, control protocol, build): 2 days.
- Unit test for the shim itself (no Unit involved, just calls `malloc`
  in a contrived program and asserts the right call fails): 0.5 day.
- Pytest fixture + `unit` fixture wiring: 1 day.
- Three first-consumer tests: 1 day.
- CI workflow: 0.5 day.
- Documentation in `tools/malloc_inject/README.md`: 0.5 day.

Total: **~5–6 days** (one focused week).

## Risks / things to think about up front

- **`LD_PRELOAD` and setuid:** Unit's main process drops privs, but doesn't
  setuid in the test environment. Shim should still gracefully no-op if it
  detects setuid (defensive).
- **Thread safety:** the call counter and stack-walk filter both need to be
  thread-safe. Use `__thread` for per-thread call counts where it makes
  sense, atomic compare-exchange for global counters.
- **Symbol collision:** shim must `dlsym(RTLD_NEXT, ...)` to forward
  non-failing calls. The first call to the wrapper has to bootstrap the
  dlsym lookup without itself calling malloc — use a static buffer for the
  bootstrap path (this is the standard `LD_PRELOAD` malloc-shim trick).
- **glibc tcmalloc/jemalloc swap:** if Unit ever links to a non-glibc
  malloc, the `LD_PRELOAD` approach still works but the symbol filter
  changes. Document this in the shim's README.

## Forwarding

This harness is a pure test-infrastructure addition; it doesn't touch any
production code path. Worth forwarding to `freeunitorg/freeunit` because
the leaks it tests are upstream leaks. Same posture as the prior PRs.

---

## Quick-reference command bag

```bash
# 1. Set up branch.
git fetch origin master
git checkout -b claude/malloc-inject-harness origin/master

# 2. Skeleton.
mkdir -p tools/malloc_inject
$EDITOR tools/malloc_inject/malloc_inject.c

# 3. Build standalone:
make -C tools/malloc_inject
# or after Makefile integration:
make malloc-inject

# 4. Smoke the shim against any process:
LD_PRELOAD=./build/malloc_inject.so \
MALLOC_INJECT_TARGETS='malloc:1' \
ls /

# 5. Run the new test set:
pytest test/test_cert_store_inject.py -v
```

## Suggested follow-on uses (out of scope for this PR)

Once the harness exists, the same pattern fences:

- The cert-rotation flow under heavy reconfigure churn.
- `nxt_router_access_log_reopen` retry on `nxt_port_socket_write` failure.
- `nxt_main_port_modules_handler` (modules discovery) reply path.
- Any future code that does the `nxt_port_socket_write(..., b)` pattern.

These should be added incrementally as features touching those paths
land — not as one big rollout.
