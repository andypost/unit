/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_PHP_STATUS_H_INCLUDED_
#define _NXT_PHP_STATUS_H_INCLUDED_

#include <nxt_main.h>


/*
 * PHP Status API structure
 * Aligned for ARMv7 compatibility (all uint64_t on 8-byte boundaries)
 */
typedef struct nxt_php_status_s {
    /* Opcache stats */
    uint64_t  opcache_hits;
    uint64_t  opcache_misses;
    uint64_t  opcache_cached_scripts;
    uint64_t  opcache_memory_used;
    uint64_t  opcache_memory_free;
    uint64_t  opcache_interned_strings_used;
    uint64_t  opcache_interned_strings_free;

    /* JIT stats */
    uint64_t  jit_buffer_size;
    uint64_t  jit_memory_used;

    /* Request counters */
    uint64_t  requests_total;
    uint64_t  requests_active;
    uint64_t  requests_rejected;

    /* GC stats */
    uint64_t  gc_runs;
    uint64_t  gc_last_run_time;

    /* Memory stats */
    uint64_t  memory_peak;
    uint64_t  memory_current;

    /* Flags */
    uint8_t   jit_enabled;
    uint8_t   opcache_enabled;
    uint8_t   _reserved[6];  /* Explicit padding for 8-byte alignment */
} nxt_php_status_t;


/*
 * Collect PHP runtime statistics
 * Inline implementation to avoid cross-module linking issues
 */
static inline void
nxt_php_collect_status(nxt_php_status_t *stats)
{
    nxt_memzero(stats, sizeof(nxt_php_status_t));

    /* Memory stats from Zend allocator - requires PHP headers */
#if defined(php_h)
    stats->memory_peak = (uint64_t) zend_memory_peak_usage(0);
    stats->memory_current = (uint64_t) zend_memory_usage(0);
#else
    stats->memory_peak = 0;
    stats->memory_current = 0;
#endif

    /* Opcache stats (if available) */
#if NXT_PHP_HAVE_ACCELERATOR
    #include "ZendAccelerator.h"

    stats->opcache_enabled = 1;
    stats->opcache_hits = (uint64_t) ZCSG(hits);
    stats->opcache_misses = (uint64_t) ZCSG(misses);

    if (ZCSG(hash).data != NULL) {
        stats->opcache_cached_scripts = (uint64_t) ZCSG(hash).num_entries;
    }

    #if PHP_VERSION_ID >= 70000
    if (ZCSG(interned_strings).str != NULL) {
        stats->opcache_interned_strings_used = (uint64_t)
            (ZCSG(interned_strings).top - ZCSG(interned_strings).start);
    }
    #endif
    stats->opcache_interned_strings_free = 0;
#else
    stats->opcache_enabled = 0;
#endif

    /* JIT stats (placeholder) */
    stats->jit_enabled = 0;
    stats->jit_buffer_size = 0;
    stats->jit_memory_used = 0;

    /* Request counters (router tracks these) */
    stats->requests_total = 0;
    stats->requests_active = 0;
    stats->requests_rejected = 0;

    /* GC stats (not in public API) */
    stats->gc_runs = 0;
    stats->gc_last_run_time = 0;
}


#endif /* _NXT_PHP_STATUS_H_INCLUDED_ */
