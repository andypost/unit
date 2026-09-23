
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_port_mmap_read() (src/nxt_port_memory.c) turns the payload of an mmap
 * port message -- an array of nxt_port_mmap_msg_t written by the sending
 * process, which for the router is an untrusted application -- into buffers
 * over the sender's shared memory segment.
 *
 * It used to walk that array as
 *
 *     for (mmap_msg = pos; mmap_msg < free; mmap_msg++)
 *
 * so a payload that is not a whole number of 12-byte records reads its last
 * "record" past mem.free: the peer controls where the message ends, and with
 * it how far the read overruns.  The fixture puts the records at the very
 * end of a page followed by a PROT_NONE page, so an overrun faults in every
 * build rather than only under ASan.  Each case runs in a child process, and
 * a child killed by a signal is reported as a failure.
 *
 * The other rows pin the per-field checks the records go through: an
 * mmap_id the sender has no segment for, a chunk_id past the data area, and
 * a size that runs off the end of it are all refused.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_port_memory_int.h>
#include <nxt_runtime.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"

#include <sys/mman.h>
#include <sys/wait.h>


typedef struct {
    const char           *name;
    nxt_uint_t           nrecords;
    nxt_port_mmap_msg_t  records[2];
    size_t               tail;
    nxt_uint_t           expect_bufs;
    size_t               expect_consumed;
} nxt_port_mmap_read_test_case_t;


static nxt_int_t
nxt_port_mmap_read_test_child(nxt_task_t *task, nxt_port_t *port,
    nxt_pid_t pid, const nxt_port_mmap_read_test_case_t *tc)
{
    u_char               *page, *start, *end;
    size_t               psize, len;
    nxt_uint_t           i, nbufs;
    nxt_buf_t            *b, *in;
    nxt_port_recv_msg_t  msg;

    psize = getpagesize();

    page = mmap(NULL, 2 * psize, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        return NXT_ERROR;
    }

    if (mprotect(page + psize, psize, PROT_NONE) != 0) {
        return NXT_ERROR;
    }

    len = tc->nrecords * sizeof(nxt_port_mmap_msg_t) + tc->tail;
    end = page + psize;
    start = end - len;

    nxt_memcpy(start, tc->records,
               tc->nrecords * sizeof(nxt_port_mmap_msg_t));

    /*
     * A zero tail reads as mmap_id 0, the one segment the sender has, so
     * the overrun is not cut short by a failed segment lookup.
     */
    nxt_memzero(start + tc->nrecords * sizeof(nxt_port_mmap_msg_t), tc->tail);

    in = nxt_buf_mem_alloc(port->mem_pool, 0, 0);
    if (in == NULL) {
        return NXT_ERROR;
    }

    in->mem.start = start;
    in->mem.pos = start;
    in->mem.free = end;
    in->mem.end = end;

    nxt_memzero(&msg, sizeof(nxt_port_recv_msg_t));

    msg.port = port;
    msg.buf = in;
    msg.port_msg.pid = pid;
    msg.port_msg.mmap = 1;

    nxt_port_mmap_read(task, &msg);

    nbufs = 0;

    for (b = msg.buf; b != NULL && b != in; b = b->next) {
        nbufs++;
    }

    if (nbufs != tc->expect_bufs) {
        nxt_log_alert(task->log, "port mmap read test \"%s\": %ui buffers, "
                      "expected %ui", tc->name, nbufs, tc->expect_bufs);
        return NXT_ERROR;
    }

    if ((size_t) (in->mem.pos - start) != tc->expect_consumed) {
        nxt_log_alert(task->log, "port mmap read test \"%s\": consumed %uz "
                      "bytes, expected %uz", tc->name,
                      (size_t) (in->mem.pos - start), tc->expect_consumed);
        return NXT_ERROR;
    }

    len = 0;

    for (i = 0; i < tc->expect_bufs; i++) {
        len += tc->records[i].size;
    }

    if (msg.size != len) {
        nxt_log_alert(task->log, "port mmap read test \"%s\": size %uz, "
                      "expected %uz", tc->name, msg.size, len);
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_port_mmap_read_test_case(nxt_task_t *task, nxt_port_t *port,
    nxt_pid_t pid, const nxt_port_mmap_read_test_case_t *tc)
{
    int    status;
    pid_t  child;

    child = fork();

    if (child == -1) {
        nxt_log_alert(task->log, "port mmap read test: fork() failed %E",
                      nxt_errno);
        return NXT_ERROR;
    }

    if (child == 0) {
        _exit(nxt_port_mmap_read_test_child(task, port, pid, tc) == NXT_OK
              ? 0 : 1);
    }

    if (waitpid(child, &status, 0) != child) {
        return NXT_ERROR;
    }

    if (WIFSIGNALED(status)) {
        nxt_log_alert(task->log, "port mmap read test \"%s\": killed by "
                      "signal %d", tc->name, WTERMSIG(status));
        return NXT_ERROR;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        nxt_log_alert(task->log, "port mmap read test \"%s\" failed",
                      tc->name);
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_int_t
nxt_port_mmap_read_test(nxt_thread_t *thr)
{
    size_t                          rec;
    nxt_mp_t                        *mp;
    nxt_int_t                       ret;
    nxt_buf_t                       *seg;
    nxt_uint_t                      i;
    nxt_task_t                      *task;
    nxt_port_t                      *port;
    nxt_process_t                   *process;
    nxt_runtime_t                   *rt, *saved_rt;
    nxt_chunk_id_t                  c;
    nxt_event_engine_t              engine, *saved_engine;
    nxt_port_mmap_handler_t         *mmap_handler;
    nxt_port_mmap_read_test_case_t  tc;

    nxt_thread_time_update(thr);

    task = thr->task;
    task->thread = thr;

    rec = sizeof(nxt_port_mmap_msg_t);

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    process = nxt_mp_zalloc(mp, sizeof(nxt_process_t));

    if (nxt_slow_path(rt == NULL || process == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    rt->mem_pool = mp;

    if (nxt_slow_path(nxt_thread_mutex_create(&rt->processes_mutex)
                      != NXT_OK))
    {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_memzero(&engine, sizeof(engine));
    engine.mem_pool = mp;
    engine.task.thread = thr;
    engine.task.log = thr->log;

    saved_rt = thr->runtime;
    saved_engine = thr->engine;
    thr->runtime = rt;
    thr->engine = &engine;

    ret = NXT_ERROR;
    port = NULL;

    process->pid = nxt_pid + 41;
    process->use_count = 1;
    nxt_queue_init(&process->ports);

    if (nxt_slow_path(nxt_thread_mutex_create(&process->incoming.mutex)
                      != NXT_OK))
    {
        goto done;
    }

    nxt_runtime_process_add(task, process);

    /*
     * The sender's one segment, id 0.  Allocating it through the outgoing
     * path of the same array is what gives it a real mapping and header.
     */
    seg = nxt_port_mmap_get_buf(task, &process->incoming,
                                2 * PORT_MMAP_CHUNK_SIZE);
    if (nxt_slow_path(seg == NULL)) {
        goto done;
    }

    mmap_handler = seg->parent;
    c = nxt_port_mmap_chunk_id(mmap_handler->hdr, seg->mem.pos);

    port = nxt_port_new(task, 1, nxt_pid, NXT_PROCESS_ROUTER);
    if (nxt_slow_path(port == NULL)) {
        goto done;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    /* Whole records. */

    nxt_memzero(&tc, sizeof(tc));

    tc.name = "one record";
    tc.nrecords = 1;
    tc.records[0].mmap_id = 0;
    tc.records[0].chunk_id = c;
    tc.records[0].size = 100;
    tc.expect_bufs = 1;
    tc.expect_consumed = rec;

    if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
        != NXT_OK)
    {
        goto done;
    }

    tc.name = "two records";
    tc.nrecords = 2;
    tc.records[1].mmap_id = 0;
    tc.records[1].chunk_id = c + 1;
    tc.records[1].size = PORT_MMAP_CHUNK_SIZE;
    tc.expect_bufs = 2;
    tc.expect_consumed = 2 * rec;

    if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
        != NXT_OK)
    {
        goto done;
    }

    /* A whole record followed by every possible partial tail. */

    tc.name = "record + partial tail";
    tc.nrecords = 1;
    tc.expect_bufs = 1;
    tc.expect_consumed = rec;

    for (i = 1; i < rec; i++) {
        tc.tail = i;

        if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
            != NXT_OK)
        {
            nxt_log_alert(thr->log, "port mmap read test: tail of %ui bytes",
                          i);
            goto done;
        }
    }

    tc.name = "partial tail only";
    tc.nrecords = 0;
    tc.tail = 5;
    tc.expect_bufs = 0;
    tc.expect_consumed = 0;

    if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
        != NXT_OK)
    {
        goto done;
    }

    /* Per-field bounds. */

    tc.tail = 0;
    tc.nrecords = 1;

    tc.name = "unknown mmap_id";
    tc.records[0].mmap_id = 1;
    tc.records[0].chunk_id = c;
    tc.records[0].size = 100;

    if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
        != NXT_OK)
    {
        goto done;
    }

    tc.name = "huge mmap_id";
    tc.records[0].mmap_id = 0xFFFFFFFF;

    if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
        != NXT_OK)
    {
        goto done;
    }

    tc.name = "chunk_id past the data area";
    tc.records[0].mmap_id = 0;
    tc.records[0].chunk_id = PORT_MMAP_CHUNK_COUNT;

    if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
        != NXT_OK)
    {
        goto done;
    }

    tc.name = "size past the data area";
    tc.records[0].chunk_id = PORT_MMAP_CHUNK_COUNT - 1;
    tc.records[0].size = PORT_MMAP_CHUNK_SIZE + 1;

    if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
        != NXT_OK)
    {
        goto done;
    }

    tc.name = "size near UINT32_MAX";
    tc.records[0].chunk_id = 0;
    tc.records[0].size = 0xFFFFFFFF;

    if (nxt_port_mmap_read_test_case(task, port, process->pid, &tc)
        != NXT_OK)
    {
        goto done;
    }

    tc.name = "unknown sender";

    tc.records[0].size = 100;

    if (nxt_port_mmap_read_test_case(task, port, process->pid + 1, &tc)
        != NXT_OK)
    {
        goto done;
    }

    ret = NXT_OK;

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port mmap read test passed");

done:

    /*
     * The fixture is not torn down: the segment and the port stay mapped
     * for the life of the test binary, the same as the other port tests'
     * process fixtures.  Only the thread's runtime and engine are restored.
     */

    thr->runtime = saved_rt;
    thr->engine = saved_engine;

    return ret;
}
