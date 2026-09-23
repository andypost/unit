
/*
 * Copyright (C) NGINX, Inc.
 *
 * Microbenchmark for the IPC queue primitives (O1, freeunit optimisation
 * stream).  Covers single-thread enqueue/dequeue throughput for
 * nxt_nncq, nxt_app_nncq, nxt_port_queue and nxt_app_queue, plus a
 * multi-thread producer/consumer benchmark for nxt_port_queue (1P1C and
 * NPxNC).
 *
 * This is measurement tooling only: it does not touch the SHM layout,
 * atomics or memory ordering of the queues it benchmarks.
 */

#include <nxt_main.h>
#include <nxt_nncq.h>
#include <nxt_app_nncq.h>
#include <nxt_port_queue.h>
#include <nxt_app_queue.h>

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <inttypes.h>


#define NXT_QBENCH_DEFAULT_NOPS  2000000


#if (__x86_64__ || __i386__)
#define NXT_QBENCH_HAVE_RDTSC  1

static inline uint64_t
nxt_qbench_rdtsc(void)
{
    uint32_t  eax, edx;

    __asm__ volatile ("rdtsc" : "=a" (eax), "=d" (edx));

    return ((uint64_t) edx << 32) | eax;
}

#else
#define NXT_QBENCH_HAVE_RDTSC  0
#endif


static int  use_rdtsc = 0;


static uint64_t
nxt_qbench_now(void)
{
    struct timespec  ts;

    if (use_rdtsc) {
#if NXT_QBENCH_HAVE_RDTSC
        return nxt_qbench_rdtsc();
#endif
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}


static void
nxt_qbench_report(const char *name, uint64_t ops, uint64_t start,
    uint64_t end)
{
    double  elapsed, ops_per_sec;

    if (use_rdtsc) {
        printf("%-24s  ops=%12" PRIu64 "  cycles=%14" PRIu64
               "  cycles/op=%8.2f\n",
               name, ops, end - start, (double) (end - start) / ops);
        return;
    }

    elapsed = (double) (end - start) / 1e9;
    ops_per_sec = (double) ops / elapsed;

    printf("%-24s  ops=%12" PRIu64 "  time=%9.3f ms  "
           "ops/sec=%14.0f  ns/op=%8.2f\n",
           name, ops, elapsed * 1000.0, ops_per_sec,
           elapsed * 1e9 / (double) ops);
}


/* -------------------------------------------------------------------- */
/* Single-thread nxt_nncq.                                              */

static void
bench_nncq(uint64_t nops)
{
    uint64_t          i, start, end;
    nxt_nncq_t        *q;
    nxt_nncq_atomic_t  v;

    q = nxt_zalloc(sizeof(nxt_nncq_t));
    nxt_nncq_init(q);

    for (i = 0; i < NXT_NNCQ_SIZE; i++) {
        nxt_nncq_enqueue(q, i);
    }

    start = nxt_qbench_now();

    for (i = 0; i < nops; i++) {
        v = nxt_nncq_dequeue(q);
        nxt_nncq_enqueue(q, v);
    }

    end = nxt_qbench_now();

    nxt_qbench_report("nncq (1P1C, self)", nops * 2, start, end);

    nxt_free(q);
}


/* -------------------------------------------------------------------- */
/* Single-thread nxt_app_nncq.                                          */

static void
bench_app_nncq(uint64_t nops)
{
    uint64_t              i, start, end;
    nxt_app_nncq_t        *q;
    nxt_app_nncq_atomic_t  v;

    q = nxt_zalloc(sizeof(nxt_app_nncq_t));
    nxt_app_nncq_init(q);

    for (i = 0; i < NXT_APP_NNCQ_SIZE; i++) {
        nxt_app_nncq_enqueue(q, i);
    }

    start = nxt_qbench_now();

    for (i = 0; i < nops; i++) {
        v = nxt_app_nncq_dequeue(q);
        nxt_app_nncq_enqueue(q, v);
    }

    end = nxt_qbench_now();

    nxt_qbench_report("app_nncq (1P1C, self)", nops * 2, start, end);

    nxt_free(q);
}


/* -------------------------------------------------------------------- */
/* Single-thread nxt_port_queue send/recv.                              */

static void
bench_port_queue(uint64_t nops, uint8_t size)
{
    int                 notify;
    uint64_t            i, start, end;
    nxt_int_t           rc;
    ssize_t             r;
    nxt_port_queue_t    *q;
    uint8_t             buf[NXT_PORT_QUEUE_MSG_SIZE];

    q = nxt_zalloc(sizeof(nxt_port_queue_t));
    nxt_port_queue_init(q);
    nxt_memzero(buf, sizeof(buf));

    start = nxt_qbench_now();

    for (i = 0; i < nops; i++) {
        rc = nxt_port_queue_send(q, buf, size, &notify);
        if (nxt_slow_path(rc != NXT_OK)) {
            fprintf(stderr, "port_queue_send failed unexpectedly\n");
            exit(1);
        }

        r = nxt_port_queue_recv(q, buf);
        if (nxt_slow_path(r < 0)) {
            fprintf(stderr, "port_queue_recv failed unexpectedly\n");
            exit(1);
        }
    }

    end = nxt_qbench_now();

    nxt_qbench_report("port_queue (1P1C, self)", nops * 2, start, end);

    nxt_free(q);
}


/* -------------------------------------------------------------------- */
/* Single-thread nxt_app_queue send/recv.                               */

static void
bench_app_queue(uint64_t nops, uint8_t size)
{
    int                    notify;
    uint32_t               cookie;
    uint64_t               i, start, end;
    nxt_int_t              rc;
    ssize_t                r;
    nxt_app_queue_t        *q;
    uint8_t                buf[NXT_APP_QUEUE_MSG_SIZE];

    q = nxt_zalloc(sizeof(nxt_app_queue_t));
    nxt_app_queue_init(q);
    nxt_memzero(buf, sizeof(buf));

    start = nxt_qbench_now();

    for (i = 0; i < nops; i++) {
        rc = nxt_app_queue_send(q, buf, size, 1, &notify, &cookie);
        if (nxt_slow_path(rc != NXT_OK)) {
            fprintf(stderr, "app_queue_send failed unexpectedly\n");
            exit(1);
        }

        r = nxt_app_queue_recv(q, buf, &cookie);
        if (nxt_slow_path(r < 0)) {
            fprintf(stderr, "app_queue_recv failed unexpectedly\n");
            exit(1);
        }
    }

    end = nxt_qbench_now();

    nxt_qbench_report("app_queue (1P1C, self)", nops * 2, start, end);

    nxt_free(q);
}


/* -------------------------------------------------------------------- */
/* Multi-thread producer/consumer for nxt_port_queue.                   */

typedef struct {
    nxt_port_queue_t   *q;
    uint64_t            nops;      /* messages this producer sends */
    uint8_t             size;
} nxt_qbench_producer_arg_t;


typedef struct {
    nxt_port_queue_t     *q;
    uint64_t              total;    /* total messages to receive, shared */
    _Atomic uint64_t      *received;
} nxt_qbench_consumer_arg_t;


static void *
nxt_qbench_producer(void *p)
{
    int                         notify;
    uint64_t                    i;
    nxt_int_t                   rc;
    uint8_t                     buf[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_qbench_producer_arg_t   *arg;

    arg = p;
    nxt_memzero(buf, sizeof(buf));

    for (i = 0; i < arg->nops; i++) {
        for ( ;; ) {
            rc = nxt_port_queue_send(arg->q, buf, arg->size, &notify);

            if (rc == NXT_OK) {
                break;
            }

            /* NXT_AGAIN: queue full, spin until the consumer drains it. */
            sched_yield();
        }
    }

    return NULL;
}


static void *
nxt_qbench_consumer(void *p)
{
    ssize_t                      r;
    uint8_t                      buf[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_qbench_consumer_arg_t    *arg;

    arg = p;

    for ( ;; ) {
        if (atomic_load_explicit(arg->received, memory_order_relaxed)
            >= arg->total)
        {
            break;
        }

        r = nxt_port_queue_recv(arg->q, buf);

        if (r >= 0) {
            atomic_fetch_add_explicit(arg->received, 1,
                                       memory_order_relaxed);
        } else {
            sched_yield();
        }
    }

    return NULL;
}


static void
bench_port_queue_mt(uint64_t nops_total, uint8_t size, int nprod, int ncons)
{
    int                          i;
    char                         name[64];
    uint64_t                     start, end;
    _Atomic uint64_t             received;
    nxt_port_queue_t             *q;
    pthread_t                    *prod_th, *cons_th;
    nxt_qbench_producer_arg_t    *prod_arg;
    nxt_qbench_consumer_arg_t    cons_arg;

    q = nxt_zalloc(sizeof(nxt_port_queue_t));
    nxt_port_queue_init(q);

    prod_th  = nxt_zalloc(nprod * sizeof(pthread_t));
    cons_th  = nxt_zalloc(ncons * sizeof(pthread_t));
    prod_arg = nxt_zalloc(nprod * sizeof(nxt_qbench_producer_arg_t));

    atomic_init(&received, 0);

    cons_arg.q        = q;
    cons_arg.total    = nops_total;
    cons_arg.received = &received;

    for (i = 0; i < nprod; i++) {
        prod_arg[i].q    = q;
        prod_arg[i].size = size;
        prod_arg[i].nops = nops_total / nprod;

        if (i == nprod - 1) {
            /* give the remainder to the last producer */
            prod_arg[i].nops += nops_total - (prod_arg[i].nops * nprod);
        }
    }

    start = nxt_qbench_now();

    for (i = 0; i < ncons; i++) {
        pthread_create(&cons_th[i], NULL, nxt_qbench_consumer, &cons_arg);
    }

    for (i = 0; i < nprod; i++) {
        pthread_create(&prod_th[i], NULL, nxt_qbench_producer, &prod_arg[i]);
    }

    for (i = 0; i < nprod; i++) {
        pthread_join(prod_th[i], NULL);
    }

    for (i = 0; i < ncons; i++) {
        pthread_join(cons_th[i], NULL);
    }

    end = nxt_qbench_now();

    snprintf(name, sizeof(name), "port_queue (%dP%dC)", nprod, ncons);
    nxt_qbench_report(name, nops_total * 2, start, end);

    nxt_free(q);
    nxt_free(prod_th);
    nxt_free(cons_th);
    nxt_free(prod_arg);
}


/* -------------------------------------------------------------------- */

static void
usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <mode> [-n ops] [--size N] [--producers N] "
        "[--consumers N] [--rdtsc]\n"
        "  modes: nncq, app_nncq, port_queue, app_queue, port_queue_mt\n",
        prog);
}


int nxt_cdecl
main(int argc, char **argv)
{
    int          i, producers, consumers;
    uint8_t      size;
    uint64_t     nops;
    const char   *mode;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    mode      = argv[1];
    nops      = NXT_QBENCH_DEFAULT_NOPS;
    size      = 24;
    producers = 1;
    consumers = 1;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            nops = strtoull(argv[++i], NULL, 10);
            continue;
        }

        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size = (uint8_t) atoi(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--producers") == 0 && i + 1 < argc) {
            producers = atoi(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--consumers") == 0 && i + 1 < argc) {
            consumers = atoi(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--rdtsc") == 0) {
            use_rdtsc = 1;
            continue;
        }

        fprintf(stderr, "unknown option: %s\n", argv[i]);
        usage(argv[0]);
        return 1;
    }

#if !NXT_QBENCH_HAVE_RDTSC
    if (use_rdtsc) {
        fprintf(stderr, "--rdtsc is not supported on this architecture, "
                         "falling back to clock_gettime\n");
        use_rdtsc = 0;
    }
#endif

    if (strcmp(mode, "nncq") == 0) {
        bench_nncq(nops);

    } else if (strcmp(mode, "app_nncq") == 0) {
        bench_app_nncq(nops);

    } else if (strcmp(mode, "port_queue") == 0) {
        bench_port_queue(nops, size);

    } else if (strcmp(mode, "app_queue") == 0) {
        bench_app_queue(nops, size);

    } else if (strcmp(mode, "port_queue_mt") == 0) {
        bench_port_queue_mt(nops, size, producers, consumers);

    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        usage(argv[0]);
        return 1;
    }

    return 0;
}
