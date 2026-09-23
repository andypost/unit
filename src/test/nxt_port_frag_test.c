
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Fragment reassembly limits at a receiving port (#394, src/nxt_port.h,
 * nxt_port_read_msg_process() in src/nxt_port_socket.c).
 *
 * The receiver holds every fragment of a message until the last one
 * arrives, and the sender decides how many streams it opens and how long
 * each one runs.  Nothing bounded either, so a peer could grow the receiver
 * without limit by never finishing a stream, or by opening a new one per
 * message.  Now:
 *
 *   - at most NXT_PORT_FRAG_STREAMS_MAX streams are open on a port;
 *   - a stream holds at most NXT_PORT_FRAG_SIZE_MAX bytes;
 *   - the open streams of a port hold at most NXT_PORT_FRAG_TOTAL_MAX.
 *
 * A stream past a limit is dropped whole, and its last fragment then finds
 * nothing to complete: the handler never sees it.  What each case asserts is
 * whether the handler ran for the stream, and with what size.
 *
 * The fragments are sized, not filled: their buffers point into a
 * PROT_NONE reservation that nothing reads, so a 128 MB stream costs no
 * memory.  The port has no socket; the messages go straight to
 * nxt_port_read_msg_process() through its NXT_TESTS wrapper.
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


static void
nxt_frag_test_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_frag_test_calls++;
    nxt_frag_test_last_stream = msg->port_msg.stream;
    nxt_frag_test_last_size = msg->size;
}


/* One fragment of "size" payload bytes: first, middle or last. */
static nxt_int_t
nxt_frag_test_send(nxt_task_t *task, nxt_port_t *port, uint32_t stream,
    size_t size, nxt_bool_t first, nxt_bool_t last)
{
    nxt_buf_t            *b;
    nxt_port_recv_msg_t  msg;

    b = nxt_mp_zalloc(port->mem_pool, sizeof(nxt_buf_t));
    if (nxt_slow_path(b == NULL)) {
        return NXT_ERROR;
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

    return NXT_OK;
}


/*
 * A stream of "n" fragments of "mb" MB each; returns whether the handler
 * ran for it, and checks the size it saw.
 */
static nxt_int_t
nxt_frag_test_stream(nxt_task_t *task, nxt_port_t *port, uint32_t stream,
    nxt_uint_t n, size_t mb, nxt_bool_t *delivered)
{
    nxt_uint_t  i, calls;

    calls = nxt_frag_test_calls;

    for (i = 0; i < n; i++) {
        if (nxt_frag_test_send(task, port, stream, mb * NXT_FRAG_TEST_MB,
                               i == 0, i == n - 1)
            != NXT_OK)
        {
            return NXT_ERROR;
        }
    }

    *delivered = (nxt_frag_test_calls != calls);

    if (*delivered
        && (nxt_frag_test_last_stream != stream
            || nxt_frag_test_last_size != n * mb * NXT_FRAG_TEST_MB))
    {
        nxt_log_alert(task->log, "port frag test: stream #%uD delivered "
                      "%uz bytes, expected %uz", stream,
                      nxt_frag_test_last_size, n * mb * NXT_FRAG_TEST_MB);
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_frag_test_expect(nxt_task_t *task, nxt_port_t *port, const char *name,
    uint32_t stream, nxt_uint_t n, size_t mb, nxt_bool_t expect)
{
    nxt_bool_t  delivered;

    if (nxt_frag_test_stream(task, port, stream, n, mb, &delivered)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (delivered != expect) {
        nxt_log_alert(task->log, "port frag test: %s: %s", name,
                      delivered ? "delivered" : "dropped");
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_frag_test_run(nxt_task_t *task, nxt_port_t *port)
{
    size_t      mb;
    uint32_t    s;
    nxt_uint_t  i, calls;

    s = NXT_FRAG_TEST_STREAM;
    mb = NXT_PORT_FRAG_SIZE_MAX / NXT_FRAG_TEST_MB;

    /*
     * Per stream: exactly at the limit; past it on the last fragment; and
     * past it on a middle one, which drops the stream while it is open.
     */

    if (nxt_frag_test_expect(task, port, "a stream at the size limit",
                             s++, mb, 1, 1)
        != NXT_OK
        || nxt_frag_test_expect(task, port, "past the size limit, last",
                                s++, mb + 1, 1, 0)
           != NXT_OK
        || nxt_frag_test_expect(task, port, "past the size limit, middle",
                                s++, mb + 2, 1, 0)
           != NXT_OK
        || nxt_frag_test_expect(task, port, "a stream after a dropped one",
                                s++, 3, 1, 1)
           != NXT_OK)
    {
        return NXT_ERROR;
    }

    /* Per port, streams: open the most there may be, then one more. */

    for (i = 0; i < NXT_PORT_FRAG_STREAMS_MAX; i++) {
        if (nxt_frag_test_send(task, port, s + i, 1, 1, 0) != NXT_OK) {
            return NXT_ERROR;
        }
    }

    calls = nxt_frag_test_calls;

    if (nxt_frag_test_send(task, port, s + i, 1, 1, 0) != NXT_OK
        || nxt_frag_test_send(task, port, s + i, 1, 0, 1) != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (nxt_frag_test_calls != calls) {
        nxt_log_alert(task->log, "port frag test: stream %ui of %ui was "
                      "accepted", i + 1, (nxt_uint_t) NXT_PORT_FRAG_STREAMS_MAX);
        return NXT_ERROR;
    }

    /* The open ones still complete, and free their slots as they do. */

    for (i = 0; i < NXT_PORT_FRAG_STREAMS_MAX; i++) {
        if (nxt_frag_test_send(task, port, s + i, 1, 0, 1) != NXT_OK) {
            return NXT_ERROR;
        }
    }

    if (nxt_frag_test_calls != calls + NXT_PORT_FRAG_STREAMS_MAX) {
        nxt_log_alert(task->log, "port frag test: %ui of %ui open streams "
                      "completed", nxt_frag_test_calls - calls,
                      (nxt_uint_t) NXT_PORT_FRAG_STREAMS_MAX);
        return NXT_ERROR;
    }

    s += NXT_PORT_FRAG_STREAMS_MAX + 1;

    /*
     * Per port, bytes: two open streams at half the total each fill it;
     * a third that would add to it is dropped, and the two still complete.
     */

    mb = NXT_PORT_FRAG_TOTAL_MAX / 2 / NXT_FRAG_TEST_MB;

    if (nxt_frag_test_send(task, port, s, mb * NXT_FRAG_TEST_MB, 1, 0)
        != NXT_OK
        || nxt_frag_test_send(task, port, s + 1, mb * NXT_FRAG_TEST_MB, 1, 0)
           != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (nxt_frag_test_expect(task, port, "a stream past the port's total",
                             s + 2, 2, 1, 0)
        != NXT_OK)
    {
        return NXT_ERROR;
    }

    calls = nxt_frag_test_calls;

    if (nxt_frag_test_send(task, port, s, 0, 0, 1) != NXT_OK
        || nxt_frag_test_send(task, port, s + 1, 0, 0, 1) != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (nxt_frag_test_calls != calls + 2) {
        nxt_log_alert(task->log, "port frag test: streams within the total "
                      "did not complete");
        return NXT_ERROR;
    }

    /* Everything is accounted back: a fresh stream at the limit passes. */

    return nxt_frag_test_expect(task, port, "a stream at the limit, again",
                                s + 3, NXT_PORT_FRAG_SIZE_MAX
                                       / NXT_FRAG_TEST_MB, 1, 1);
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

    /* A whole stream past the size limit, as address space only. */
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

    /*
     * The port is left behind like the other port tests' fixtures: its
     * pool still holds the fragments' buffer headers, which point into the
     * reservation, so the reservation stays too.
     */

    if (ret == NXT_OK) {
        nxt_log_error(NXT_LOG_NOTICE, thr->log, "port frag test passed");
    }

    return ret;
}
