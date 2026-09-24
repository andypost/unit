/*
 * Copyright (C) F5, Inc.
 */

/*
 * Regression test for the retry budget of the shared-memory queues
 * (src/nxt_nncq.h, src/nxt_app_nncq.h).
 *
 * Both queues live in memory the application process maps writable, and
 * their enqueue and dequeue retry until the entry under head or tail carries
 * a cycle they can act on.  A peer that writes any other cycle there keeps
 * the router spinning on it for as long as it likes: every retry re-reads
 * the same poisoned word.  Each case below poisons one queue that way and
 * runs the router-side operation in a child under alarm(): the operation
 * must return, and report the queue as empty or the send as failed.
 */

#include <nxt_main.h>
#include <nxt_port_queue.h>
#include <nxt_app_queue.h>
#include "nxt_tests.h"

#include <sys/mman.h>
#include <sys/wait.h>


typedef int (*nxt_nncq_bound_test_op_t)(void *q);


static const uint8_t  nxt_nncq_bound_test_msg[1] = { 0x5A };


static int
nxt_nncq_bound_test_port_recv(void *mem)
{
    uint8_t           buf[NXT_PORT_QUEUE_MSG_SIZE];
    nxt_port_queue_t  *q;

    q = mem;

    /* A cycle neither equal to head's nor one behind it. */
    q->queue.entries[0] = 7 * NXT_NNCQ_SIZE;

    return nxt_port_queue_recv(q, buf) == -1;
}


static int
nxt_nncq_bound_test_port_send_full(void *mem)
{
    int               notify;
    nxt_port_queue_t  *q;

    q = mem;
    q->free_items.entries[0] = 7 * NXT_NNCQ_SIZE;

    return nxt_port_queue_send(q, nxt_nncq_bound_test_msg, 1, &notify) == NXT_AGAIN;
}


static int
nxt_nncq_bound_test_port_send_slot(void *mem)
{
    int               notify;
    nxt_port_queue_t  *q;

    q = mem;
    q->queue.entries[0] = 7 * NXT_NNCQ_SIZE;

    return nxt_port_queue_send(q, nxt_nncq_bound_test_msg, 1, &notify) == NXT_ERROR;
}


static int
nxt_nncq_bound_test_app_send_full(void *mem)
{
    int              notify;
    uint32_t         cookie;
    nxt_app_queue_t  *q;

    q = mem;
    q->free_items.entries[0] = 7 * NXT_APP_NNCQ_SIZE;

    return nxt_app_queue_send(q, nxt_nncq_bound_test_msg, 1, 1, &notify, &cookie) == NXT_AGAIN;
}


static int
nxt_nncq_bound_test_app_send_slot(void *mem)
{
    int              notify;
    uint32_t         cookie;
    nxt_app_queue_t  *q;

    q = mem;
    q->queue.entries[0] = 7 * NXT_APP_NNCQ_SIZE;

    return nxt_app_queue_send(q, nxt_nncq_bound_test_msg, 1, 1, &notify, &cookie) == NXT_ERROR;
}


static const struct {
    const char                *name;
    nxt_bool_t                app;
    nxt_nncq_bound_test_op_t  op;
} nxt_nncq_bound_test_cases[] = {
    { "port recv", 0, nxt_nncq_bound_test_port_recv },
    { "port send, poisoned free list", 0, nxt_nncq_bound_test_port_send_full },
    { "port send, poisoned slot", 0, nxt_nncq_bound_test_port_send_slot },
    { "app send, poisoned free list", 1, nxt_nncq_bound_test_app_send_full },
    { "app send, poisoned slot", 1, nxt_nncq_bound_test_app_send_slot },
};


static nxt_int_t
nxt_nncq_bound_test_case(nxt_thread_t *thr, nxt_uint_t i)
{
    int     status;
    pid_t   pid;
    void    *mem;
    size_t  size;

    size = nxt_nncq_bound_test_cases[i].app ? sizeof(nxt_app_queue_t)
                                            : sizeof(nxt_port_queue_t);

    mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
               -1, 0);
    if (mem == MAP_FAILED) {
        nxt_log_alert(thr->log, "nncq bound test mmap failed %E", nxt_errno);
        return NXT_ERROR;
    }

    if (nxt_nncq_bound_test_cases[i].app) {
        nxt_app_queue_init(mem);

    } else {
        nxt_port_queue_init(mem);
    }

    pid = fork();

    if (pid == 0) {
        alarm(5);
        _exit(nxt_nncq_bound_test_cases[i].op(mem) ? 0 : 1);
    }

    munmap(mem, size);

    if (pid == -1 || waitpid(pid, &status, 0) == -1) {
        nxt_log_alert(thr->log, "nncq bound test fork failed %E", nxt_errno);
        return NXT_ERROR;
    }

    if (!WIFEXITED(status)) {
        nxt_log_alert(thr->log, "nncq bound test: %s hangs, child killed "
                      "by signal %d", nxt_nncq_bound_test_cases[i].name,
                      WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        return NXT_ERROR;
    }

    if (WEXITSTATUS(status) != 0) {
        nxt_log_alert(thr->log, "nncq bound test: %s returned, but not "
                      "the failure", nxt_nncq_bound_test_cases[i].name);
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_int_t
nxt_nncq_bound_test(nxt_thread_t *thr)
{
    nxt_uint_t  i;

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nncq bound test started");

    for (i = 0; i < nxt_nitems(nxt_nncq_bound_test_cases); i++) {
        if (nxt_nncq_bound_test_case(thr, i) != NXT_OK) {
            return NXT_ERROR;
        }
    }

    nxt_thread_time_update(thr);
    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nncq bound test passed");

    return NXT_OK;
}
