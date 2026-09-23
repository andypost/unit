
/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_ROUTER_SCHEDULE_H_INCLUDED_
#define _NXT_ROUTER_SCHEDULE_H_INCLUDED_


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


#endif /* _NXT_ROUTER_SCHEDULE_H_INCLUDED_ */
