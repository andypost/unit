
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_STATUS_H_INCLUDED_
#define _NXT_STATUS_H_INCLUDED_


typedef struct {
    nxt_str_t         name;
    uint32_t          active_requests;
    uint32_t          pending_processes;
    uint32_t          processes;
    uint32_t          unaccounted_processes;
    uint32_t          idle_processes;
    uint32_t          detached_processes;
} nxt_status_app_t;


/*
 * One schedule's counters (src/nxt_router_schedule.c's
 * nxt_router_schedule_state_t), copied out by
 * nxt_router_schedules_status() -- see docs/observability/status-extensions.md.
 * "running" is 0/1: whether a run is in flight right now.  "last_start" is
 * zero until the first run is dispatched, in whole seconds since the Epoch;
 * "last_duration" is in milliseconds and, like "last_status", reflects only
 * the most recently *finished* run, so it lags "running" while one is still
 * in flight.
 */
typedef struct {
    nxt_str_t         name;
    uint32_t          runs;
    uint32_t          skipped;
    uint32_t          failed;
    uint32_t          timed_out;
    uint32_t          running;
    uint32_t          last_status;
    uint32_t          last_duration;
    int64_t           last_start;
} nxt_status_schedule_t;


typedef struct {
    uint64_t          accepted_conns;
    uint64_t          idle_conns;
    uint64_t          closed_conns;
    uint64_t          requests;

    /*
     * OpenTelemetry span export health, filled in by the router.  The two
     * counters are only meaningful when otel_configured is set: a build
     * without OTel support, or one where "settings/telemetry" is absent,
     * leaves them zero and reports no "telemetry" object in /status at all.
     *
     * Counted in spans since the live tracer provider was installed, i.e.
     * reset by a telemetry reconfiguration.
     */
    uint64_t          otel_spans_exported;
    uint64_t          otel_spans_failed;
    uint8_t           otel_configured;

    size_t            apps_count;
    /*
     * schedules_count entries of nxt_status_schedule_t follow the last
     * nxt_status_app_t in "apps" (not a member here, since C allows only one
     * flexible array member per struct): the whole report is one contiguous
     * buffer copied across the router/controller port, so both sides must
     * compute this second array's address the same way --
     * (nxt_status_schedule_t *) (report->apps + report->apps_count), exactly
     * as done in nxt_router.c and nxt_status.c.
     */
    size_t            schedules_count;
    nxt_status_app_t  apps[];
} nxt_status_report_t;


nxt_conf_value_t *nxt_status_get(nxt_status_report_t *report, nxt_mp_t *mp);


/* See nxt_status_report_t.schedules_count above. */

nxt_inline nxt_status_schedule_t *
nxt_status_report_schedules(nxt_status_report_t *report)
{
    return (nxt_status_schedule_t *) (report->apps + report->apps_count);
}


#endif /* _NXT_STATUS_H_INCLUDED_ */
