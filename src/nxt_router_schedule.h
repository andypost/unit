
/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_ROUTER_SCHEDULE_H_INCLUDED_
#define _NXT_ROUTER_SCHEDULE_H_INCLUDED_


#include <nxt_router.h>
#include <nxt_conf.h>


/*
 * "schedules": periodic internal requests to an application.  The design is
 * docs/adr/0004-schedules.md.
 *
 * The limits below are shared by the validator (src/nxt_conf_validation.c)
 * and the router, so that a value the validator lets through is one the
 * router can represent.
 */

/*
 * Timers are nxt_msec_t and the timer tree compares them as a signed 32-bit
 * difference (nxt_msec_diff()), so a single wait must stay below 2^31 ms.
 * The limit applies to "interval" + "jitter", the longest wait there is.
 */
#define NXT_SCHEDULE_SECONDS_MAX   2147483

#define NXT_SCHEDULE_NAME_MAX      128
#define NXT_SCHEDULE_URI_MAX       4096
#define NXT_SCHEDULE_HEADERS_MAX   8192


typedef enum {
    NXT_SCHEDULE_SKIP = 0,
    NXT_SCHEDULE_QUEUE,
} nxt_router_schedule_overlap_t;


/*
 * One per schedule per configuration.  Allocated from rtcf->mem_pool, so it
 * lives exactly as long as the configuration that defined it.
 */
typedef struct {
    nxt_str_t                name;
    nxt_http_action_t        *action;     /* resolved "pass" */
    nxt_str_t                uri;

    /*
     * The whole request header, "GET <uri> HTTP/1.1\r\n...\r\n\r\n", built
     * once here and parsed by the real request parser on every run.
     */
    nxt_str_t                request;

    nxt_msec_t               interval;
    nxt_msec_t               jitter;
    nxt_msec_t               timeout;
    uint8_t                  overlap;     /* nxt_router_schedule_overlap_t */
    uint8_t                  run_on_start;
} nxt_router_schedule_t;


/*
 * The schedules of one configuration, and the internal socket configuration
 * and joint that give their requests a real r->conf (ADR 0004, section 6.3).
 *
 * Ownership: the skcf holds one reference on the rtcf, taken while the
 * configuration is created, exactly as a listener's skcf does.  The joint
 * holds the skcf, and each run holds the joint.  The joint's own reference is
 * dropped when the next configuration is applied, so the rtcf survives as
 * long as the newest of: the configuration, and its last run.
 */
struct nxt_router_schedules_s {
    nxt_router_schedule_t    *schedule;
    uint32_t                 nschedules;

    nxt_socket_conf_t        skcf;
    nxt_socket_conf_joint_t  joint;

    nxt_sockaddr_t           *remote;
    nxt_sockaddr_t           *local;
};


nxt_int_t nxt_router_conf_resolve(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf, nxt_conf_value_t *root);
void nxt_router_schedules_apply(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf);

/* Exported for src/test/nxt_router_schedule_test.c. */
nxt_int_t nxt_router_schedules_joint_init(nxt_task_t *task,
    nxt_router_conf_t *rtcf, nxt_router_schedules_t *sc,
    nxt_conf_value_t *http);
nxt_int_t nxt_router_schedule_msec(int64_t seconds, nxt_msec_t *out);
nxt_msec_t nxt_router_schedule_delay(nxt_msec_t interval, nxt_msec_t jitter,
    uint32_t rnd);
size_t nxt_router_schedule_uri_public(const nxt_str_t *uri);
nxt_int_t nxt_router_schedule_request_build(nxt_mp_t *mp,
    nxt_router_schedule_t *sched, nxt_conf_value_t *headers);


#endif /* _NXT_ROUTER_SCHEDULE_H_INCLUDED_ */
