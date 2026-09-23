
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * The NXT_HTTP_PROTO_DEVNULL slot of nxt_http_proto[] (src/nxt_h1proto.c),
 * reserved since 2019 and filled for "schedules".  See nxt_http_devnull.h.
 *
 * Every call site of nxt_http_proto[] is guarded by r->proto.any != NULL
 * or runs only for a request that has a protocol, so filling the slot is
 * what lets a connection-less request go through the ordinary request code
 * unchanged: the start, the application handler, the response, the error
 * paths, the access log and the close.
 */

#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_http_devnull.h>


/* There is no body: the request is ready at once, as h1 does. */

void
nxt_http_devnull_body_read(nxt_task_t *task, nxt_http_request_t *r)
{
    r->state->ready_handler(task, r, NULL);
}


/* The owner sets r->local before the request starts. */

void
nxt_http_devnull_local_addr(nxt_task_t *task, nxt_http_request_t *r)
{
}


/*
 * The header goes nowhere.  The body handler is queued rather than called,
 * as h1 does, so that it never runs inside the caller's frame.
 */

void
nxt_http_devnull_header_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_work_handler_t body_handler, void *data)
{
    nxt_buf_t           *last;
    nxt_http_devnull_t  *dn;

    nxt_debug(task, "devnull request header send: %d", (int) r->status);

    dn = r->proto.any;
    dn->status = r->status;

    r->header_sent = 1;

    if (body_handler != NULL) {
        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           body_handler, task, r, data);
        return;
    }

    last = nxt_http_buf_last(r);

    if (last != NULL) {
        nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue, last);
    }
}


/*
 * Count the body, keep its first bytes, and complete the whole chain at
 * once.  Completing the sync "last" buffer runs its handler, which is how
 * the request ends.
 */

void
nxt_http_devnull_send(nxt_task_t *task, nxt_http_request_t *r, nxt_buf_t *out)
{
    size_t              size, n;
    nxt_buf_t           *b;
    nxt_http_devnull_t  *dn;

    dn = r->proto.any;

    for (b = out; b != NULL; b = b->next) {
        if (nxt_buf_is_sync(b)) {
            continue;
        }

        size = nxt_buf_used_size(b);
        dn->body_bytes += size;

        if (nxt_buf_is_mem(b) && dn->head_length < NXT_HTTP_DEVNULL_HEAD) {
            n = nxt_min(size, NXT_HTTP_DEVNULL_HEAD - dn->head_length);

            nxt_memcpy(dn->head + dn->head_length, b->mem.pos, n);
            dn->head_length += n;
        }
    }

    nxt_debug(task, "devnull request send: %O body bytes", dn->body_bytes);

    nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue, out);
}


nxt_off_t
nxt_http_devnull_body_bytes_sent(nxt_task_t *task, nxt_http_proto_t proto)
{
    nxt_http_devnull_t  *dn;

    dn = proto.any;

    return dn->body_bytes;
}


/* An error ends the request: nothing is queued, so only "last" is left. */

void
nxt_http_devnull_discard(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *last)
{
    nxt_http_devnull_t  *dn;

    nxt_debug(task, "devnull request discard");

    dn = r->proto.any;
    dn->discarded = 1;

    if (last != NULL) {
        nxt_sendbuf_drain(task, &task->thread->engine->fast_work_queue, last);
    }
}


/*
 * Called from nxt_http_request_close_handler(), which releases the request
 * pool as soon as this returns: the owner must not touch dn->request from
 * its close callback onwards.
 */

void
nxt_http_devnull_close(nxt_task_t *task, nxt_http_proto_t proto,
    nxt_socket_conf_joint_t *joint)
{
    nxt_http_devnull_t  *dn;

    nxt_debug(task, "devnull request close");

    dn = proto.any;

    if (dn->request != NULL) {
        dn->status = dn->request->status;
        dn->request = NULL;
    }

    dn->close(task, dn, joint);
}
