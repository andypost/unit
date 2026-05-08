
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_port.h>
#include <nxt_main_process.h>
#include <nxt_router.h>


static void nxt_signal_handler(nxt_task_t *task, void *obj, void *data);
static void nxt_signal_sigterm_handler(nxt_task_t *task, void *obj, void *data);
static void nxt_signal_sigquit_handler(nxt_task_t *task, void *obj, void *data);


const nxt_sig_event_t  nxt_process_signals[] = {
    nxt_event_signal(SIGHUP,  nxt_signal_handler),
    nxt_event_signal(SIGINT,  nxt_signal_sigterm_handler),
    nxt_event_signal(SIGQUIT, nxt_signal_sigquit_handler),
    nxt_event_signal(SIGTERM, nxt_signal_sigterm_handler),
    nxt_event_signal(SIGCHLD, nxt_signal_handler),
    nxt_event_signal(SIGUSR1, nxt_signal_handler),
    nxt_event_signal(SIGUSR2, nxt_signal_handler),
    nxt_event_signal_end,
};


static void
nxt_signal_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_trace(task, "signal signo:%d (%s) received, ignored",
              (int) (uintptr_t) obj, data);
}


void
nxt_signal_quit_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_buf_t      *b;
    uint8_t        quit_param;
    nxt_runtime_t  *rt;

    /*
     * P5: read the optional wire-format byte set by P1's
     * nxt_runtime_quit_buf() and propagate to rt->quit_mode so the
     * router/app process's nxt_runtime_quit() (called via
     * nxt_process_quit) can take the GRACEFUL path.  Same parser
     * shape as libunit's nxt_unit.c:1062-1068 and as
     * nxt_port_quit_handler() above.
     */
    quit_param = NXT_PORT_QUIT_NORMAL;

    b = msg->buf;
    if (b != NULL && nxt_buf_mem_used_size(&b->mem) >= 1) {
        quit_param = b->mem.pos[0];
    }

    rt = task->thread->runtime;
    rt->quit_mode = quit_param;

    nxt_process_quit(task, 0);
}


static void
nxt_signal_sigterm_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_debug(task, "sigterm handler signo:%d (%s)",
              (int) (uintptr_t) obj, data);

    /* A fast exit. */

    nxt_runtime_quit(task, 0);
}


static void
nxt_signal_sigquit_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_debug(task, "sigquit handler signo:%d (%s)",
              (int) (uintptr_t) obj, data);

    /* A graceful exit. */

    nxt_process_quit(task, 0);
}
