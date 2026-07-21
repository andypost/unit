# FreeUnit Zero-Allocation Performance & Architectural Optimization Plan

## Executive Summary
This plan outlines the architecture, parallel task breakdown, future subsystem expansion, validation procedures, and measurement protocol for achieving **zero-allocation hot paths** across FreeUnit's HTTP/1.1 parser, keep-alive handler, static file server, variable evaluation engine, and connection infrastructure.

---

## 1. Architectural Scope & File Mapping

* [src/nxt_http_parse.h](file:///home/andy/www/unit/src/nxt_http_parse.h) - Parser data structures (`nxt_http_request_parse_t`, `nxt_http_field_t`).
* [src/nxt_http_parse.c](file:///home/andy/www/unit/src/nxt_http_parse.c) - Header FSM state machine and field processing routines.
* [src/nxt_http_request.c](file:///home/andy/www/unit/src/nxt_http_request.c) - Request creation (`nxt_http_request_create`), memory pool initialization.
* [src/nxt_h1proto.c](file:///home/andy/www/unit/src/nxt_h1proto.c) - HTTP/1.1 protocol handler and keep-alive state machine.
* [src/nxt_http_static.c](file:///home/andy/www/unit/src/nxt_http_static.c) - Static file serving module and buffer allocation.
* [src/nxt_var.c](file:///home/andy/www/unit/src/nxt_var.c) - Variable cache and evaluation engine.
* [src/nxt_conn.c](file:///home/andy/www/unit/src/nxt_conn.c) - Connection creation (`nxt_conn_create`) and event engine interface.
* [src/test/nxt_http_parse_test.c](file:///home/andy/www/unit/src/test/nxt_http_parse_test.c) - Parser unit tests.

---

## 2. Parallel Agent Task Breakdown (Phase 1: Header Parser & Keep-Alive)

### Track A: Agent 1 — Embedded Header Array Storage
* **Goal**: Eliminate `nxt_list_create` heap allocation for standard HTTP request headers.
* **Tasks**:
  1. Add an embedded header buffer `nxt_http_field_t inline_fields[16];` inside `nxt_http_request_parse_t` and `nxt_http_request_t`.
  2. Update `nxt_http_parse_request_init()` to initialize `rp->fields` pointing to the embedded storage before falling back to dynamic `nxt_list_t` chunks.
  3. Modify `nxt_http_parse_field_end()` to fill `inline_fields` first, spilling over to dynamic list allocation only when header count > 16.
  4. Ensure all header iterators (`nxt_list_each`) handle both embedded items and linked overflow parts seamlessly.

### Track B: Agent 2 — Keep-Alive Memory Pool Recycling
* **Goal**: Reuse request memory pools across keep-alive pipeline requests to avoid pool creation/destruction churn.
* **Tasks**:
  1. Audit `nxt_h1p_keepalive()` in [src/nxt_h1proto.c](file:///home/andy/www/unit/src/nxt_h1proto.c) to identify cleanup requirements between keep-alive requests.
  2. Implement `nxt_mp_reset(r->mem_pool)` on keep-alive boundary instead of destroying `r->mem_pool`.
  3. Ensure pointer states (`r->target`, `r->path`, `r->args`, parser FSM flags) are completely reset for consecutive requests on the same connection.
  4. Verify no memory leak occurs from unreleased pool blocks across long-lived keep-alive connections.

### Track C: Agent 3 — Lazy Sync Buffer Allocation & Capacity Tuning
* **Goal**: Eliminate upfront allocation of unused descriptors and optimize dynamic list chunks.
* **Tasks**:
  1. Update `nxt_http_request_create()` in [src/nxt_http_request.c](file:///home/andy/www/unit/src/nxt_http_request.c#L245) to remove upfront `nxt_mp_zget(mp, NXT_BUF_SYNC_SIZE)`.
  2. Move `r->last` sync buffer initialization to `nxt_h1p_request_header_send()` when headers are validated and response output is queued.
  3. Increase dynamic fallback list chunk size from 8 to 16 in `nxt_list_create()` calls across HTTP request setup.

### Track D: Agent 4 — Test Harness & Allocation Measurement Suite
* **Goal**: Build unit test validation and automated memory allocation measurement tools.
* **Tasks**:
  1. Update [src/test/nxt_http_parse_test.c](file:///home/andy/www/unit/src/test/nxt_http_parse_test.c) with test cases covering requests with 0 to 32 headers.
  2. Create an allocation tracking wrapper or hook in `nxt_mp_alloc` for debug/test builds to count allocations per request.
  3. Implement benchmark harness verifying:
     * 0 memory pool block allocations for keep-alive requests with <= 16 headers.
     * Correct parsing accuracy for normal, complex, and oversized header sets.

---

## 3. Future Expansion Roadmap (Phase 2: Subsystem Optimization Pointers)

### Track E: Static File Server Zero-Allocation Context ([src/nxt_http_static.c](file:///home/andy/www/unit/src/nxt_http_static.c#L214))
* **Findings**: `nxt_http_static_handler()` executes `ctx = nxt_mp_zget(r->mem_pool, sizeof(nxt_http_static_ctx_t))` and `fb = nxt_mp_zget(r->mem_pool, NXT_BUF_FILE_SIZE)` on every static file hit.
* **Pointers**:
  1. Embed `nxt_http_static_ctx_t` into `nxt_http_request_t` or request memory pool slab.
  2. Add a thread-local freelist for `NXT_BUF_FILE_SIZE` file buffer descriptors on `engine->fast_work_queue`.

### Track F: Variable Cache Scratchpad ([src/nxt_var.c](file:///home/andy/www/unit/src/nxt_var.c#L222))
* **Findings**: `nxt_var_cache_value()` executes `value = nxt_mp_zget(cache->pool, sizeof(nxt_str_t))` when evaluating variables (`$host`, `$request_uri`, etc.).
* **Pointers**:
  1. Add an inline 8-element `nxt_str_t` array directly inside `nxt_var_cache_t` as a primary scratchpad.
  2. Avoid `nxt_mp_zget` for non-cacheable variable evaluation by using zero-copy slice pointers directly referencing request parser buffers.

### Track G: Thread-Local Connection Freelist ([src/nxt_conn.c](file:///home/andy/www/unit/src/nxt_conn.c#L48), [src/nxt_openssl.c](file:///home/andy/www/unit/src/nxt_openssl.c#L1172))
* **Findings**: `nxt_conn_create()` and TLS init allocate `nxt_conn_t` and `nxt_openssl_conn_t` via `nxt_mp_zget()` for every new TCP stream.
* **Pointers**:
  1. Implement a lock-free thread-local freelist of recycled `nxt_conn_t` objects attached to `nxt_thread()->engine`.
  2. Recycle connection structs on socket close (`nxt_conn_close`) to bypass memory pool allocation on accept.

### Track H: Upstream Proxy Response Header Pre-Allocation ([src/nxt_router.c](file:///home/andy/www/unit/src/nxt_router.c#L2473))
* **Findings**: Forwarding backend responses from application workers (PHP, Python, Node.js, Go) allocates response field lists.
* **Pointers**:
  1. Extend the embedded `inline_fields[16]` array pattern to `r->resp.fields`.
  2. Achieve zero-allocation response processing for application worker responses with <= 16 response headers.

---

## 4. Actionable Todo Checklist

### Phase 1: Header Parser & Keep-Alive
- [x] **Agent 1**: Implement `inline_fields[16]` embedded storage in `nxt_http_request_parse_t`.
- [x] **Agent 1**: Update `nxt_http_parse_field_end()` logic for embedded storage fill and overflow fallback.
- [x] **Agent 2**: Implement `nxt_mp_reset()` recycling in `nxt_h1p_keepalive()`.
- [x] **Agent 2**: Add state reset guard verification between keep-alive requests.
- [x] **Agent 3**: Defer `r->last` sync buffer allocation to response header send phase.
- [x] **Agent 3**: Tune dynamic list initial capacity from 8 to 16.
- [x] **Agent 4**: Extend `nxt_http_parse_test.c` with 0, 8, 16, 24, and 32 header test suites.
- [x] **Agent 4**: Run allocation profiling test to verify 0 heap allocations per request on keep-alive.

### Phase 2: Subsystem Optimization Expansion
- [x] **Track E**: Embed `nxt_http_static_ctx_t` in `r` and implement file buffer descriptor freelist.
- [x] **Track F**: Implement inline 8-element scratchpad in `nxt_var_cache_t`.
- [x] **Track G**: Build per-thread `nxt_conn_t` freelist recycler on event engine.
- [x] **Track H**: Apply `inline_fields[16]` array to `r->resp.fields` in proxy response pipeline.

---

## 5. General Validation & Measurement Protocol

### A. Correctness & Regression Testing
Run the FreeUnit unit test suite to verify HTTP parser compliance and structural integrity:
```bash
make -j$(nproc) test
./build/nxt_tests
```

### B. Allocation Measurement Protocol
To measure allocation reduction:
1. Compile unit tests with allocation tracing enabled (`-DNXT_DEBUG_ALLOC=1`).
2. Execute 100,000 HTTP requests over keep-alive:
   ```bash
   wrk -t2 -c50 -d10s http://127.0.0.1:8080/
   ```
3. Target Metric: **0 allocations per request** after initial connection setup for requests with <= 16 headers.

### C. Performance Metrics Target
* **Latency**: 5-10% reduction in p99 HTTP request header processing latency.
* **Throughput**: ~8-15% increase in requests per second (RPS) on keep-alive benchmark runs.
