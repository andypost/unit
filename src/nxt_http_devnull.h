
/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_HTTP_DEVNULL_H_INCLUDED_
#define _NXT_HTTP_DEVNULL_H_INCLUDED_


#include <nxt_http.h>


/*
 * NXT_HTTP_PROTO_DEVNULL: the protocol of a request that has no client
 * connection (docs/adr/0004-schedules.md, section 6.4).  The response is
 * counted, its first bytes are kept for the log, and every buffer is
 * completed at once, which is what ends the request: the router's
 * completion handler on r->last closes it.
 *
 * r->proto.any points at an nxt_http_devnull_t that the owner of the
 * request embeds in its own object.  The owner is told through ->close,
 * called in place of the h1 connection close, and must release "joint",
 * the request's r->conf, from there or later on the same engine.
 */

#define NXT_HTTP_DEVNULL_HEAD  256


typedef struct nxt_http_devnull_s  nxt_http_devnull_t;

struct nxt_http_devnull_s {
    nxt_http_request_t        *request;

    void                      (*close)(nxt_task_t *task,
                                       nxt_http_devnull_t *devnull,
                                       nxt_socket_conf_joint_t *joint);

    nxt_off_t                 body_bytes;
    nxt_http_status_t         status;
    uint8_t                   discarded;  /* 1 bit */

    size_t                    head_length;
    u_char                    head[NXT_HTTP_DEVNULL_HEAD];
};


void nxt_http_devnull_body_read(nxt_task_t *task, nxt_http_request_t *r);
void nxt_http_devnull_local_addr(nxt_task_t *task, nxt_http_request_t *r);
void nxt_http_devnull_header_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_work_handler_t body_handler, void *data);
void nxt_http_devnull_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *out);
nxt_off_t nxt_http_devnull_body_bytes_sent(nxt_task_t *task,
    nxt_http_proto_t proto);
void nxt_http_devnull_discard(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *last);
void nxt_http_devnull_close(nxt_task_t *task, nxt_http_proto_t proto,
    nxt_socket_conf_joint_t *joint);


#endif /* _NXT_HTTP_DEVNULL_H_INCLUDED_ */
