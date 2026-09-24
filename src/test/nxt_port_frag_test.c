
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Fragment reassembly limits at a receiving port (#394): streams per port,
 * bytes per stream and bytes per port.  A stream past a limit is dropped
 * whole, so its handler never runs.  Fragment buffers point into a
 * PROT_NONE reservation that nothing reads, so a 128 MB stream is free.
 * A fragment counts for the buffer it came in, so empty fragments are
 * bounded too.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"

#include <sys/mman.h>


#define NXT_FRAG_TEST_MB      (1024 * 1024)
#define NXT_FRAG_TEST_STREAM  0x46524700    /* distinct from other tests */


static u_char      *nxt_frag_test_space;
static nxt_uint_t  nxt_frag_test_calls;
static uint32_t    nxt_frag_test_last_stream;
static size_t      nxt_frag_test_last_size;
static nxt_bool_t  nxt_frag_test_oom;


static void
nxt_frag_test_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_frag_test_calls++;
    nxt_frag_test_last_stream = msg->port_msg.stream;
    nxt_frag_test_last_size = msg->size;
}


static void
nxt_frag_test_send(nxt_task_t *task, nxt_port_t *port, uint32_t stream,
    size_t size, nxt_bool_t first, nxt_bool_t last)
{
    nxt_buf_t            *b;
    nxt_port_recv_msg_t  msg;

    b = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_buf_t));
    if (nxt_slow_path(b == NULL)) {
        nxt_frag_test_oom = 1;
        return;
    }

    b->mem.start = nxt_frag_test_space;
    b->mem.pos = nxt_frag_test_space;
    b->mem.free = nxt_frag_test_space;
    b->mem.end = nxt_frag_test_space + size;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.buf = b;
    msg.size = sizeof(nxt_port_msg_t) + size;
    msg.fd[0] = -1;
    msg.fd[1] = -1;
    msg.port_msg.stream = stream;
    msg.port_msg.pid = nxt_pid;
    msg.port_msg.type = _NXT_PORT_MSG_STATUS;
    msg.port_msg.nf = !first;
    msg.port_msg.mf = !last;

    nxt_port_test_run_read_msg_process(task, port, &msg);
}


/* A stream of "n" fragments of "mb" MB: delivered whole, or not at all. */
static nxt_int_t
nxt_frag_test_expect(nxt_task_t *task, nxt_port_t *port, const char *name,
    uint32_t stream, nxt_uint_t n, size_t mb, nxt_bool_t expect)
{
    nxt_uint_t  i, calls;

    calls = nxt_frag_test_calls;

    for (i = 0; i < n; i++) {
        nxt_frag_test_send(task, port, stream, mb * NXT_FRAG_TEST_MB, i == 0,
                           i == n - 1);
    }

    NXT_TEST_CHECK(task->log, (nxt_frag_test_calls != calls) == expect,
                   "port frag test: %s: %s", name,
                   expect ? "dropped" : "delivered");
    NXT_TEST_CHECK(task->log, !expect
                   || (nxt_frag_test_last_stream == stream
                       && nxt_frag_test_last_size == n * mb * NXT_FRAG_TEST_MB),
                   "port frag test: %s: %uz bytes", name,
                   nxt_frag_test_last_size);
    return NXT_OK;
}


static nxt_int_t
nxt_frag_test_run(nxt_task_t *task, nxt_port_t *port)
{
    size_t      mb, half;
    uint32_t    s;
    nxt_uint_t  i, n, calls;

    s = NXT_FRAG_TEST_STREAM;
    mb = NXT_PORT_FRAG_SIZE_MAX / NXT_FRAG_TEST_MB;

    /* Per stream: at the limit; past it on the last or a middle fragment. */

    if (nxt_frag_test_expect(task, port, "at the size limit", s++, mb, 1, 1)
        || nxt_frag_test_expect(task, port, "past it, last", s++, mb + 1, 1, 0)
        || nxt_frag_test_expect(task, port, "past it, middle", s++, mb + 2, 1,
                                0)
        || nxt_frag_test_expect(task, port, "after a drop", s++, 3, 1, 1))
    {
        return NXT_ERROR;
    }

    /* Per port, streams: open the most there may be, then one more. */

    for (i = 0; i < NXT_PORT_FRAG_STREAMS_MAX; i++) {
        nxt_frag_test_send(task, port, s + i, 1, 1, 0);
    }

    calls = nxt_frag_test_calls;

    nxt_frag_test_send(task, port, s + i, 1, 1, 0);
    nxt_frag_test_send(task, port, s + i, 1, 0, 1);

    NXT_TEST_CHECK(task->log, nxt_frag_test_calls == calls,
                   "port frag test: a stream past the most was accepted");

    /* The open ones still complete, and free their slots as they do. */

    for (i = 0; i < NXT_PORT_FRAG_STREAMS_MAX; i++) {
        nxt_frag_test_send(task, port, s + i, 1, 0, 1);
    }

    NXT_TEST_CHECK(task->log,
                   nxt_frag_test_calls == calls + NXT_PORT_FRAG_STREAMS_MAX,
                   "port frag test: open streams did not complete");

    s += NXT_PORT_FRAG_STREAMS_MAX + 1;

    /* Per port, bytes: two open halves fill it; a third stream is dropped. */

    half = NXT_PORT_FRAG_TOTAL_MAX / 2 / NXT_FRAG_TEST_MB * NXT_FRAG_TEST_MB;

    nxt_frag_test_send(task, port, s, half, 1, 0);
    nxt_frag_test_send(task, port, s + 1, half, 1, 0);

    if (nxt_frag_test_expect(task, port, "past the port's total", s + 2, 2,
                             1, 0))
    {
        return NXT_ERROR;
    }

    calls = nxt_frag_test_calls;

    nxt_frag_test_send(task, port, s, 0, 0, 1);
    nxt_frag_test_send(task, port, s + 1, 0, 0, 1);

    NXT_TEST_CHECK(task->log, nxt_frag_test_calls == calls + 2,
                   "port frag test: streams within the total did not "
                   "complete");

    /* Everything is accounted back: a fresh stream at the limit passes. */

    if (nxt_frag_test_expect(task, port, "at the limit, again", s + 3, mb, 1,
                             1))
    {
        return NXT_ERROR;
    }

    /*
     * Empty fragments: each one holds a buffer of port->max_size, so the
     * port's total admits NXT_PORT_FRAG_TOTAL_MAX / max_size of them.
     */

    port->max_size = 16 * 1024;
    n = NXT_PORT_FRAG_TOTAL_MAX / port->max_size;

    if (nxt_frag_test_expect(task, port, "empty fragments up to the total",
                             s + 4, n + 1, 0, 1)
        || nxt_frag_test_expect(task, port, "empty fragments past the total",
                                s + 5, n + 2, 0, 0))
    {
        return NXT_ERROR;
    }

    NXT_TEST_CHECK(task->log, !nxt_frag_test_oom,
                   "port frag test: out of memory");
    return NXT_OK;
}


nxt_int_t
nxt_port_frag_test(nxt_thread_t *thr)
{
    size_t              reserve;
    nxt_int_t           ret;
    nxt_task_t          *task;
    nxt_port_t          *port;

    nxt_thread_time_update(thr);

    task = thr->task;
    task->thread = thr;

    reserve = NXT_PORT_FRAG_TOTAL_MAX + 16 * NXT_FRAG_TEST_MB;

    nxt_frag_test_space = mmap(NULL, reserve, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                               -1, 0);
    if (nxt_frag_test_space == MAP_FAILED) {
        nxt_log_alert(thr->log, "port frag test: mmap() failed %E", nxt_errno);
        return NXT_ERROR;
    }

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(port == NULL)) {
        (void) munmap(nxt_frag_test_space, reserve);
        return NXT_ERROR;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;
    port->handler = nxt_frag_test_handler;

    ret = nxt_frag_test_run(task, port);

    /* The port and the reservation it points into are left behind. */

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port frag test passed");
    }

    return ret;
}
