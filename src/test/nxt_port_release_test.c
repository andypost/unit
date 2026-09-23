
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_port_release() and a port linked into a process's port list without
 * the process reference that goes with it (#425, src/nxt_port.c).
 *
 * The release decides from port->link.next alone whether the port belongs
 * to a process, then drops port->process's reference.  The only place that
 * links a port, nxt_process_port_add(), also sets port->process and takes
 * the reference, so the two agree in production; but nothing stops a port
 * being linked by hand (src/test/nxt_port_ready_test.c does it for its
 * fixtures), and the only guard was an nxt_assert() that release builds
 * compile out.  A debug build aborted and a release build dereferenced
 * NULL in nxt_process_use().
 *
 * The release now unlinks such a port -- leaving it on the list would leave
 * the list pointing into the pool the release is about to free -- reports
 * the broken pairing, and drops no reference it does not hold.  The case
 * runs in a child so that the pre-fix crash (either build) is reported as
 * a failure rather than taking the test binary down.
 */

#include <nxt_main.h>
#include <nxt_port.h>
#include <nxt_runtime.h>
#include "nxt_tests.h"

#include <sys/wait.h>


static int
nxt_port_release_test_child(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_port_t     *port, *paired;
    nxt_process_t  *process;

    process = nxt_mp_zalloc(rt->mem_pool, sizeof(nxt_process_t));
    if (process == NULL) {
        return 2;
    }

    process->pid = nxt_pid + 53;
    process->use_count = 1;
    nxt_queue_init(&process->ports);

    nxt_runtime_process_add(task, process);

    /* A correctly paired port first: its release drops its reference. */

    paired = nxt_port_new(task, 1, process->pid, NXT_PROCESS_APP);
    if (paired == NULL) {
        return 2;
    }

    paired->pair[0] = -1;
    paired->pair[1] = -1;
    paired->socket.fd = -1;

    nxt_process_port_add(task, process, paired);

    if (process->use_count != 2) {
        return 3;
    }

    nxt_port_use(task, paired, -1);

    if (process->use_count != 1 || !nxt_queue_is_empty(&process->ports)) {
        return 4;
    }

    /* Linked by hand: no port->process, no reference. */

    port = nxt_port_new(task, 2, process->pid, NXT_PROCESS_APP);
    if (port == NULL) {
        return 2;
    }

    port->pair[0] = -1;
    port->pair[1] = -1;
    port->socket.fd = -1;

    nxt_queue_insert_tail(&process->ports, &port->link);

    nxt_port_use(task, port, -1);

    if (!nxt_queue_is_empty(&process->ports)) {
        /* The list still points at the freed port. */
        return 5;
    }

    if (process->use_count != 1) {
        return 6;
    }

    return 0;
}


nxt_int_t
nxt_port_release_test(nxt_thread_t *thr)
{
    int            status;
    pid_t          child;
    nxt_mp_t       *mp;
    nxt_task_t     *task;
    nxt_runtime_t  *rt;

    task = thr->task;
    task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zalloc(mp, sizeof(nxt_runtime_t));
    if (nxt_slow_path(rt == NULL)) {
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

    child = fork();

    if (child == -1) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    if (child == 0) {
        thr->runtime = rt;
        thr->engine = NULL;

        _exit(nxt_port_release_test_child(task, rt));
    }

    nxt_mp_destroy(mp);

    if (waitpid(child, &status, 0) != child) {
        return NXT_ERROR;
    }

    if (WIFSIGNALED(status)) {
        nxt_log_alert(thr->log, "port release test: a hand-linked port "
                      "killed the release with signal %d", WTERMSIG(status));
        return NXT_ERROR;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        nxt_log_alert(thr->log, "port release test failed at step %d",
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "port release test passed");

    return NXT_OK;
}
