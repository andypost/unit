
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * "schedules": the router issues a periodic GET to an application, with no
 * client connection.  The design, and the reasons behind each choice here,
 * are in docs/adr/0004-schedules.md; the section numbers below refer to it.
 */

#include <nxt_router.h>
#include <nxt_conf.h>
#include <nxt_http.h>
#include <nxt_checked.h>
#include <nxt_http_devnull.h>
#include <nxt_router_schedule.h>
#if (NXT_HAVE_OTEL)
#include <nxt_otel.h>
#endif


/* The longest single wait, in milliseconds; see nxt_router_schedule.h. */
#define NXT_SCHEDULE_MSEC_MAX     ((nxt_msec_t) NXT_SCHEDULE_SECONDS_MAX * 1000)

/* "run_on_start": the first run, about a second after the apply. */
#define NXT_SCHEDULE_START_DELAY  1000

/* A timed-out request a worker claimed but has not acknowledged yet. */
#define NXT_SCHEDULE_CLAIM_RETRY  1000


/*
 * Router-global, survives reconfiguration (section 4).  malloc()ed, and
 * touched only on the main engine, so it needs no lock.
 */
typedef struct {
    nxt_queue_link_t         link;        /* nxt_router_schedule_states */

    nxt_router_schedule_t    *conf;       /* current, NULL once removed */
    nxt_router_schedules_t   *schedules;  /* the set "conf" belongs to */

    nxt_timer_t              timer;       /* on the main engine */
    nxt_msec_t               base;        /* timers.now when last armed */

    uint8_t                  pending;     /* "queue": one run waiting */
    uint8_t                  seen;        /* in the configuration applied */

    /* The name, stored after the struct, the counters and "running". */
    nxt_status_schedule_t    stat;
} nxt_router_schedule_state_t;


/*
 * One run.  malloc()ed on the main engine, used on the worker engine from
 * the post until the result is posted back, then freed on the main engine.
 */
typedef struct {
    nxt_http_devnull_t           devnull;    /* r->proto.any */

    nxt_router_schedule_state_t  *state;     /* main engine only */
    nxt_router_schedule_t        *sched;     /* valid while the joint is */
    nxt_router_schedules_t       *schedules;
    nxt_event_engine_t           *main;

    nxt_timer_t                  timer;      /* worker engine */
    nxt_work_t                   work;

    nxt_nsec_t                   started;
    nxt_msec_t                   duration;
    uint32_t                     seq;
    uint8_t                      timed_out;  /* 1 bit */
    uint8_t                      uri_cut;    /* 1 bit */

    size_t                       uri_length;
    u_char                       uri[128];
} nxt_router_schedule_run_t;


typedef struct {
    nxt_str_t                pass;
    nxt_str_t                uri;
    int64_t                  interval;
    int64_t                  jitter;
    int64_t                  timeout;
    nxt_str_t                overlap;
    uint8_t                  run_on_start;
    nxt_conf_value_t         *headers;
} nxt_router_schedule_conf_t;


static nxt_int_t nxt_router_schedules_create(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf, nxt_conf_value_t *root);
static nxt_int_t nxt_router_schedule_create(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf, nxt_str_t *name, nxt_conf_value_t *value,
    nxt_router_schedule_t *sched);
static nxt_event_engine_t *nxt_router_schedule_engine(nxt_router_t *router);
static void nxt_router_schedule_post(nxt_event_engine_t *engine,
    nxt_work_t *work, nxt_work_handler_t handler, void *obj);
static void nxt_router_schedules_insert_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedules_release(nxt_task_t *task,
    nxt_router_schedules_t *sc);
static void nxt_router_schedules_release_handler(nxt_task_t *task, void *obj,
    void *data);
static nxt_router_schedule_state_t *nxt_router_schedule_state_find(
    nxt_str_t *name);
static void nxt_router_schedule_update(nxt_task_t *task,
    nxt_router_schedules_t *sc, nxt_router_schedule_t *sched);
static void nxt_router_schedule_remove(nxt_task_t *task,
    nxt_router_schedule_state_t *state);
static void nxt_router_schedule_state_free(nxt_task_t *task,
    nxt_router_schedule_state_t *state);
static void nxt_router_schedule_state_free_handler(nxt_task_t *task,
    void *obj, void *data);
static void nxt_router_schedule_arm(nxt_event_engine_t *engine,
    nxt_router_schedule_state_t *state, nxt_msec_t delay);
static void nxt_router_schedule_timer_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedule_start(nxt_task_t *task,
    nxt_router_schedule_state_t *state);
static void nxt_router_schedule_run(nxt_task_t *task, void *obj, void *data);
static nxt_int_t nxt_router_schedule_request_init(nxt_http_request_t *r,
    nxt_router_schedule_t *sched, nxt_socket_conf_t *skcf);
static void nxt_router_schedule_run_timeout(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedule_run_close(nxt_task_t *task,
    nxt_http_devnull_t *dn, nxt_socket_conf_joint_t *joint);
static void nxt_router_schedule_run_finish(nxt_task_t *task, void *obj,
    void *data);
static void nxt_router_schedule_done(nxt_task_t *task, void *obj, void *data);


static nxt_conf_map_t  nxt_router_schedule_conf[] = {
    {
        nxt_string("pass"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_router_schedule_conf_t, pass),
    },

    {
        nxt_string("uri"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_router_schedule_conf_t, uri),
    },

    {
        nxt_string("interval"),
        NXT_CONF_MAP_INT64,
        offsetof(nxt_router_schedule_conf_t, interval),
    },

    {
        nxt_string("jitter"),
        NXT_CONF_MAP_INT64,
        offsetof(nxt_router_schedule_conf_t, jitter),
    },

    {
        nxt_string("timeout"),
        NXT_CONF_MAP_INT64,
        offsetof(nxt_router_schedule_conf_t, timeout),
    },

    {
        nxt_string("overlap"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_router_schedule_conf_t, overlap),
    },

    {
        nxt_string("run_on_start"),
        NXT_CONF_MAP_INT8,
        offsetof(nxt_router_schedule_conf_t, run_on_start),
    },

    {
        nxt_string("headers"),
        NXT_CONF_MAP_PTR,
        offsetof(nxt_router_schedule_conf_t, headers),
    },
};


/*
 * The request fields a run's header may carry, with the protocol-neutral
 * handlers of the h1 table (src/nxt_h1proto.c).  Connection, Upgrade and
 * Transfer-Encoding are left out: their handlers write into the h1
 * connection state, and the validator refuses them anyway.  Built on the
 * main thread, before any run can use it.
 */
static nxt_lvlhsh_t             nxt_router_schedule_fields_hash;
static nxt_bool_t               nxt_router_schedule_fields_ready;

static nxt_http_field_proc_t    nxt_router_schedule_fields[] = {
    { nxt_string("Host"),              &nxt_http_request_host, 0 },
    { nxt_string("Cookie"),            &nxt_http_request_field,
        offsetof(nxt_http_request_t, cookie) },
    { nxt_string("Referer"),           &nxt_http_request_field,
        offsetof(nxt_http_request_t, referer) },
    { nxt_string("User-Agent"),        &nxt_http_request_field,
        offsetof(nxt_http_request_t, user_agent) },
    { nxt_string("Content-Type"),      &nxt_http_request_field,
        offsetof(nxt_http_request_t, content_type) },
    { nxt_string("Authorization"),     &nxt_http_request_field,
        offsetof(nxt_http_request_t, authorization) },
#if (NXT_HAVE_OTEL)
    { nxt_string("Traceparent"),       &nxt_otel_parse_traceparent, 0 },
    { nxt_string("Tracestate"),        &nxt_otel_parse_tracestate,  0 },
#endif
};


/*
 * The set of the configuration applied last, or NULL.  Its joint's own
 * reference (count 1 at creation) keeps that configuration alive for the
 * schedules; it is dropped when the next configuration replaces it.
 */
static nxt_router_schedules_t   *nxt_router_schedules_current;

/* Of nxt_router_schedule_state_t, by name. */
static nxt_queue_t              nxt_router_schedule_states = {
    { &nxt_router_schedule_states.head, &nxt_router_schedule_states.head }
};


/*
 * The single hook in nxt_router_conf_create() (section 3).  It stands where
 * nxt_http_routes_resolve() was called, so that the function, already very
 * branchy, gains no new decision.  Resolving the routes first keeps the old
 * order; a schedule's "pass" names an application, which
 * nxt_http_action_create() resolves on the spot against rtcf->apps_hash, so
 * the schedules only have to come after the applications.
 *
 * Nothing done here may outlive a failed configuration: everything comes from
 * rtcf->mem_pool, which nxt_router_conf_error() destroys unconditionally.
 */

nxt_int_t
nxt_router_conf_resolve(nxt_task_t *task, nxt_router_temp_conf_t *tmcf,
    nxt_conf_value_t *root)
{
    nxt_int_t  ret;

    ret = nxt_http_routes_resolve(task, tmcf);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    return nxt_router_schedules_create(task, tmcf, root);
}


static nxt_int_t
nxt_router_schedules_create(nxt_task_t *task, nxt_router_temp_conf_t *tmcf,
    nxt_conf_value_t *root)
{
    size_t                  size;
    uint32_t                next;
    nxt_mp_t                *mp;
    nxt_int_t               ret;
    nxt_str_t               name;
    nxt_uint_t              i, n;
    nxt_conf_value_t        *schedules, *value;
    nxt_router_conf_t       *rtcf;
    nxt_router_schedules_t  *sc;

    static const nxt_str_t  schedules_path = nxt_string("/schedules");
    static const nxt_str_t  http_path = nxt_string("/settings/http");

    schedules = nxt_conf_get_path(root, &schedules_path);
    if (schedules == NULL) {
        return NXT_OK;
    }

    n = nxt_conf_object_members_count(schedules);
    if (n == 0) {
        return NXT_OK;
    }

    if (!nxt_router_schedule_fields_ready) {
        ret = nxt_http_fields_hash(&nxt_router_schedule_fields_hash,
                                   nxt_router_schedule_fields,
                                   nxt_nitems(nxt_router_schedule_fields));
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }

        nxt_router_schedule_fields_ready = 1;
    }

    rtcf = tmcf->router_conf;
    mp = rtcf->mem_pool;

    sc = nxt_mp_zget(mp, sizeof(nxt_router_schedules_t));
    if (nxt_slow_path(sc == NULL)) {
        return NXT_ERROR;
    }

    if (nxt_slow_path(nxt_size_mul(n, sizeof(nxt_router_schedule_t), &size)
                      || nxt_u32_from_size(n, &sc->nschedules)))
    {
        return NXT_ERROR;
    }

    sc->schedule = nxt_mp_zget(mp, size);
    if (nxt_slow_path(sc->schedule == NULL)) {
        return NXT_ERROR;
    }

    next = 0;

    for (i = 0; i < n; i++) {
        value = nxt_conf_next_object_member(schedules, &name, &next);
        if (nxt_slow_path(value == NULL)) {
            return NXT_ERROR;
        }

        ret = nxt_router_schedule_create(task, tmcf, &name, value,
                                         &sc->schedule[i]);
        if (nxt_slow_path(ret != NXT_OK)) {
            return ret;
        }
    }

    ret = nxt_router_schedules_joint_init(task, rtcf, sc,
                                          nxt_conf_get_path(root, &http_path));
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    rtcf->schedules = sc;

    nxt_debug(task, "router conf %p: %uD schedules", rtcf, sc->nschedules);

    return NXT_OK;
}


/*
 * The configuration has been validated, so a value out of range here is an
 * internal error, not a user one; it is still checked rather than trusted,
 * because an out-of-range wait would wrap the timer silently.
 */

static nxt_int_t
nxt_router_schedule_create(nxt_task_t *task, nxt_router_temp_conf_t *tmcf,
    nxt_str_t *name, nxt_conf_value_t *value, nxt_router_schedule_t *sched)
{
    nxt_mp_t                    *mp;
    nxt_int_t                   ret;
    nxt_router_schedule_conf_t  scf;

    mp = tmcf->router_conf->mem_pool;

    nxt_memzero(&scf, sizeof(scf));
    scf.timeout = -1;

    ret = nxt_conf_map_object(tmcf->mem_pool, value, nxt_router_schedule_conf,
                              nxt_nitems(nxt_router_schedule_conf), &scf);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_alert(task, "schedule \"%V\" map error", name);
        return NXT_ERROR;
    }

    /* The configuration tree lives in tmcf->mem_pool; keep copies. */

    if (nxt_slow_path(nxt_str_dup(mp, &sched->name, name) == NULL
                      || nxt_str_dup(mp, &sched->uri, &scf.uri) == NULL))
    {
        return NXT_ERROR;
    }

    if (scf.timeout == -1) {
        scf.timeout = scf.interval;
    }

    if (nxt_slow_path(nxt_router_schedule_msec(scf.interval, &sched->interval)
                      != NXT_OK
                      || nxt_router_schedule_msec(scf.jitter, &sched->jitter)
                         != NXT_OK
                      || nxt_router_schedule_msec(scf.timeout, &sched->timeout)
                         != NXT_OK
                      || sched->interval == 0
                      || sched->timeout == 0
                      || sched->jitter > NXT_SCHEDULE_MSEC_MAX
                                         - sched->interval))
    {
        nxt_alert(task, "schedule \"%V\": invalid timing", name);
        return NXT_ERROR;
    }

    sched->overlap = nxt_str_eq(&scf.overlap, "queue", 5) ? NXT_SCHEDULE_QUEUE
                                                          : NXT_SCHEDULE_SKIP;
    sched->run_on_start = scf.run_on_start;

    sched->action = nxt_http_action_create(task, tmcf, &scf.pass);
    if (nxt_slow_path(sched->action == NULL)) {
        nxt_alert(task, "schedule \"%V\": invalid \"pass\"", name);
        return NXT_ERROR;
    }

    /* The validator admits applications only (section 2); make sure. */

    if (nxt_slow_path(sched->action->handler != nxt_http_application_handler))
    {
        nxt_alert(task, "schedule \"%V\": \"pass\" is not an application",
                  name);
        return NXT_ERROR;
    }

    return nxt_router_schedule_request_build(mp, sched, scf.headers);
}


/*
 * The internal socket configuration and joint (section 6.3): a run's
 * request needs a real r->conf, with what a listener would have.
 */

nxt_int_t
nxt_router_schedules_joint_init(nxt_task_t *task, nxt_router_conf_t *rtcf,
    nxt_router_schedules_t *sc, nxt_conf_value_t *http)
{
    nxt_socket_conf_t        *skcf;
    nxt_socket_conf_joint_t  *joint;

    static nxt_str_t  remote = nxt_string("127.0.0.1");
    static nxt_str_t  local = nxt_string("127.0.0.1:80");

    skcf = &sc->skcf;

    if (nxt_slow_path(nxt_router_socket_conf_http(rtcf->mem_pool, skcf, http)
                      != NXT_OK))
    {
        return NXT_ERROR;
    }

    /*
     * nxt_router_conf_release() unlinks both nodes with nxt_queue_remove(),
     * which writes through the neighbours' pointers: a self-linked node
     * survives that, a zeroed one would not.  Neither is on any list:
     * router->sockets holds listeners only.
     */
    nxt_queue_self(&skcf->link);

    skcf->router_conf = rtcf;

    sc->remote = nxt_sockaddr_parse_optport(rtcf->mem_pool, &remote);
    sc->local = nxt_sockaddr_parse(rtcf->mem_pool, &local);

    if (nxt_slow_path(sc->remote == NULL || sc->local == NULL)) {
        return NXT_ERROR;
    }

    joint = &sc->joint;

    nxt_queue_self(&joint->link);
    joint->socket_conf = skcf;

    /*
     * The references, as a listener's are counted: the joint's own one, the
     * skcf's on behalf of the joint, and the rtcf's on behalf of the skcf.
     * If this configuration fails to apply, the pool that holds all three
     * counters is destroyed, so nothing has to be undone.
     */
    joint->count = 1;
    skcf->count = 1;
    rtcf->count++;

    return NXT_OK;
}


/*
 * Activation (section 4).  Called from nxt_router_conf_apply() on the main
 * thread, once the new engines are posted and the configuration can no
 * longer fail.
 *
 *   - The new set's joint goes to its worker engine (section 5), where a
 *     job puts it on engine->joints, so that engine cannot exit under a run.
 *   - Each schedule finds its state by name.  "running" and "pending" carry
 *     over, so a run in flight across a reconfiguration still counts for
 *     "overlap".  A state whose name is gone is dropped now, or when its run
 *     ends.
 *   - The previous set's joint gets its own reference dropped, by a job on
 *     its engine: runs still in flight keep that configuration alive, the
 *     way an in-flight listener request does.
 */

void
nxt_router_schedules_apply(nxt_task_t *task, nxt_router_temp_conf_t *tmcf)
{
    nxt_uint_t                   i;
    nxt_queue_link_t             *lnk, *next;
    nxt_event_engine_t           *engine;
    nxt_router_schedules_t       *sc, *old;
    nxt_router_schedule_state_t  *state;

    old = nxt_router_schedules_current;
    sc = tmcf->router_conf->schedules;

    if (sc != NULL) {
        engine = nxt_router_schedule_engine(tmcf->router_conf->router);

        if (engine != NULL) {
            sc->joint.engine = engine;

            nxt_router_schedule_post(engine, &sc->insert_work,
                                     nxt_router_schedules_insert_handler,
                                     &sc->joint);

        } else {
            nxt_alert(task, "schedules: no worker engine, no run will start");
        }
    }

    nxt_router_schedules_current = sc;

    nxt_queue_each(state, &nxt_router_schedule_states,
                   nxt_router_schedule_state_t, link)
    {
        state->seen = 0;

    } nxt_queue_loop;

    for (i = 0; sc != NULL && i < sc->nschedules; i++) {
        nxt_router_schedule_update(task, sc, &sc->schedule[i]);
    }

    for (lnk = nxt_queue_first(&nxt_router_schedule_states);
         lnk != nxt_queue_tail(&nxt_router_schedule_states);
         lnk = next)
    {
        next = nxt_queue_next(lnk);

        state = nxt_queue_link_data(lnk, nxt_router_schedule_state_t, link);

        if (!state->seen && state->conf != NULL) {
            nxt_router_schedule_remove(task, state);
        }
    }

    if (old != NULL) {
        nxt_router_schedules_release(task, old);
    }
}


/*
 * The run executes on the first worker engine.  It must not run on the main
 * engine: the main router port hands every ".data" message to the
 * configuration handler, so an application's reply would be read as a new
 * configuration.  router->engines lists worker engines only.
 */

static nxt_event_engine_t *
nxt_router_schedule_engine(nxt_router_t *router)
{
    nxt_queue_link_t  *lnk;

    lnk = nxt_queue_first(&router->engines);

    if (lnk == nxt_queue_tail(&router->engines)) {
        return NULL;
    }

    return nxt_queue_link_data(lnk, nxt_event_engine_t, link0);
}


static void
nxt_router_schedule_post(nxt_event_engine_t *engine, nxt_work_t *work,
    nxt_work_handler_t handler, void *obj)
{
    work->next = NULL;
    work->handler = handler;
    work->task = &engine->task;
    work->obj = obj;
    work->data = NULL;

    nxt_event_engine_post(engine, work);
}


static void
nxt_router_schedules_insert_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_socket_conf_joint_t  *joint;

    joint = obj;

    nxt_debug(task, "schedules joint %p inserted", joint);

    nxt_queue_insert_tail(&task->thread->engine->joints, &joint->link);
}


static void
nxt_router_schedules_release(nxt_task_t *task, nxt_router_schedules_t *sc)
{
    nxt_event_engine_t  *engine;

    engine = sc->joint.engine;

    if (engine == NULL) {
        /* Never posted anywhere: the joint is still self-linked. */
        nxt_router_conf_release(task, &sc->joint);
        return;
    }

    /* Posted after the insert job, so it runs after it on that engine. */

    nxt_router_schedule_post(engine, &sc->release_work,
                             nxt_router_schedules_release_handler,
                             &sc->joint);
}


static void
nxt_router_schedules_release_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_debug(task, "schedules joint %p released", obj);

    nxt_router_joint_release(task, obj);
}


static nxt_router_schedule_state_t *
nxt_router_schedule_state_find(nxt_str_t *name)
{
    nxt_router_schedule_state_t  *state;

    nxt_queue_each(state, &nxt_router_schedule_states,
                   nxt_router_schedule_state_t, link)
    {
        if (nxt_strstr_eq(&state->stat.name, name)) {
            return state;
        }

    } nxt_queue_loop;

    return NULL;
}


static void
nxt_router_schedule_update(nxt_task_t *task, nxt_router_schedules_t *sc,
    nxt_router_schedule_t *sched)
{
    nxt_msec_t                   delay, elapsed;
    nxt_event_engine_t           *engine;
    nxt_router_schedule_t        *prev;
    nxt_router_schedule_state_t  *state;

    engine = task->thread->engine;

    state = nxt_router_schedule_state_find(&sched->name);

    if (state == NULL) {
        state = nxt_malloc(sizeof(nxt_router_schedule_state_t)
                           + sched->name.length);
        if (nxt_slow_path(state == NULL)) {
            nxt_alert(task, "schedule \"%V\": no memory, it will not run",
                      &sched->name);
            return;
        }

        nxt_memzero(state, sizeof(nxt_router_schedule_state_t));

        state->stat.name.start = (u_char *) state
                            + sizeof(nxt_router_schedule_state_t);
        state->stat.name.length = sched->name.length;
        nxt_memcpy(state->stat.name.start, sched->name.start, sched->name.length);

        state->timer.bias = NXT_TIMER_DEFAULT_BIAS;
        state->timer.work_queue = &engine->fast_work_queue;
        state->timer.handler = nxt_router_schedule_timer_handler;
        state->timer.task = &engine->task;
        state->timer.log = engine->task.log;

        nxt_queue_insert_tail(&nxt_router_schedule_states, &state->link);

        prev = NULL;

    } else {
        prev = state->conf;
    }

    state->conf = sched;
    state->schedules = sc;
    state->seen = 1;

    if (sched->overlap == NXT_SCHEDULE_SKIP) {
        state->pending = 0;
    }

    if (prev == NULL) {
        /* A new schedule, or one removed while running and now back. */

        delay = nxt_router_schedule_delay(sched->run_on_start
                                          ? NXT_SCHEDULE_START_DELAY
                                          : sched->interval,
                                          sched->jitter,
                                          nxt_random(&task->thread->random));

        nxt_router_schedule_arm(engine, state, delay);

        nxt_log(task, NXT_LOG_INFO, "schedule \"%V\": first run in %M ms",
                &state->stat.name, delay);

        return;
    }

    if (prev->interval == sched->interval && prev->jitter == sched->jitter) {
        /* The clock is kept: a change of "uri" or "timeout" moves nothing. */
        return;
    }

    /*
     * Measure the new wait from the moment the current one started, not
     * from now, so that a change does not postpone the next run by the
     * time already waited.
     */

    delay = nxt_router_schedule_delay(sched->interval, sched->jitter,
                                      nxt_random(&task->thread->random));

    elapsed = nxt_max(nxt_msec_diff(engine->timers.now, state->base), 0);

    nxt_debug(task, "schedule \"%V\": re-armed, %M ms of %M elapsed",
              &state->stat.name, elapsed, delay);

    nxt_timer_add(engine, &state->timer,
                  (delay > elapsed) ? delay - elapsed : 0);
}


static void
nxt_router_schedule_remove(nxt_task_t *task, nxt_router_schedule_state_t *state)
{
    nxt_log(task, NXT_LOG_INFO, "schedule \"%V\": removed%s", &state->stat.name,
            state->stat.running ? ", the run in progress will complete" : "");

    state->conf = NULL;
    state->schedules = NULL;
    state->pending = 0;

    if (state->stat.running) {
        /* Freed when the run is reported, in nxt_router_schedule_done(). */
        (void) nxt_timer_delete(task->thread->engine, &state->timer);
        return;
    }

    nxt_router_schedule_state_free(task, state);
}


/*
 * As nxt_router_free_app() does for the idle timer: a delete that is still
 * queued keeps the timer reachable from the engine, so the memory then goes
 * with a zero-delay expiry instead.
 */

static void
nxt_router_schedule_state_free(nxt_task_t *task,
    nxt_router_schedule_state_t *state)
{
    nxt_event_engine_t  *engine;

    engine = task->thread->engine;

    nxt_queue_remove(&state->link);

    if (nxt_timer_delete(engine, &state->timer)) {
        state->timer.handler = nxt_router_schedule_state_free_handler;
        nxt_timer_add(engine, &state->timer, 0);
        return;
    }

    nxt_free(state);
}


static void
nxt_router_schedule_state_free_handler(nxt_task_t *task, void *obj,
    void *data)
{
    nxt_free(nxt_timer_data(obj, nxt_router_schedule_state_t, timer));
}


static void
nxt_router_schedule_arm(nxt_event_engine_t *engine,
    nxt_router_schedule_state_t *state, nxt_msec_t delay)
{
    state->base = engine->timers.now;

    nxt_timer_add(engine, &state->timer, delay);
}


/* The schedule is due (section 5).  Main engine. */

static void
nxt_router_schedule_timer_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_msec_t                   delay;
    nxt_router_schedule_t        *sched;
    nxt_router_schedule_state_t  *state;

    state = nxt_timer_data(obj, nxt_router_schedule_state_t, timer);

    sched = state->conf;
    if (nxt_slow_path(sched == NULL)) {
        return;
    }

    delay = nxt_router_schedule_delay(sched->interval, sched->jitter,
                                      nxt_random(&task->thread->random));

    nxt_router_schedule_arm(task->thread->engine, state, delay);

    nxt_debug(task, "schedule \"%V\": due, next in %M ms", &state->stat.name,
              delay);

    if (!state->stat.running) {
        nxt_router_schedule_start(task, state);
        return;
    }

    if (sched->overlap == NXT_SCHEDULE_QUEUE) {
        nxt_debug(task, "schedule \"%V\": queued behind the running one",
                  &state->stat.name);

        state->pending = 1;
        return;
    }

    state->stat.skipped++;

    nxt_log(task, NXT_LOG_WARN, "schedule \"%V\": run skipped, the previous "
            "one is still running", &state->stat.name);
}


/* Main engine: hand a run to the worker engine that owns the joint. */

static void
nxt_router_schedule_start(nxt_task_t *task, nxt_router_schedule_state_t *state)
{
    size_t                     n;
    nxt_realtime_t             now;
    nxt_event_engine_t         *engine;
    nxt_router_schedule_t      *sched;
    nxt_router_schedules_t     *sc;
    nxt_router_schedule_run_t  *run;

    sc = state->schedules;
    sched = state->conf;
    engine = sc->joint.engine;

    if (nxt_slow_path(engine == NULL)) {
        state->stat.failed++;
        return;
    }

    run = nxt_zalloc(sizeof(nxt_router_schedule_run_t));
    if (nxt_slow_path(run == NULL)) {
        state->stat.failed++;

        nxt_alert(task, "schedule \"%V\": no memory to start a run",
                  &state->stat.name);
        return;
    }

    run->state = state;
    run->sched = sched;
    run->schedules = sc;
    run->main = task->thread->engine;
    run->seq = ++state->stat.runs;

    /*
     * What the log may show of the URI, copied now: the configuration the
     * run belongs to can be gone by the time its result is logged.
     */
    n = nxt_router_schedule_uri_public(&sched->uri);
    run->uri_length = nxt_min(n, sizeof(run->uri));
    nxt_memcpy(run->uri, sched->uri.start, run->uri_length);
    run->uri_cut = (run->uri_length < sched->uri.length);

    state->stat.running = 1;

    nxt_realtime(&now);
    state->stat.last_start = now.sec;

    nxt_debug(task, "schedule \"%V\": run %uD posted to engine %p",
              &state->stat.name, run->seq, engine);

    nxt_router_schedule_post(engine, &run->work, nxt_router_schedule_run,
                             run);
}


/*
 * The run, on the worker engine (section 6.5).  A request is created with
 * the internal joint as r->conf and the devnull protocol, its header is
 * parsed by the same parser a client's is, and the "pass" action runs
 * directly: nxt_http_request_start() would read socket_conf->action, one
 * per skcf, while each schedule has its own.
 */

static void
nxt_router_schedule_run(nxt_task_t *task, void *obj, void *data)
{
    nxt_int_t                  ret;
    nxt_event_engine_t         *engine;
    nxt_http_request_t         *r;
    nxt_router_schedule_t      *sched;
    nxt_router_schedules_t     *sc;
    nxt_router_schedule_run_t  *run;

    run = obj;
    sched = run->sched;
    sc = run->schedules;
    engine = task->thread->engine;

    /* The run's reference; nxt_router_schedule_run_finish() drops it. */
    sc->joint.count++;

    run->started = nxt_thread_monotonic_time(task->thread);

    run->timer.bias = NXT_TIMER_DEFAULT_BIAS;
    run->timer.work_queue = &engine->fast_work_queue;
    run->timer.task = &engine->task;
    run->timer.log = engine->task.log;

    run->devnull.close = nxt_router_schedule_run_close;

    r = nxt_http_request_create(task);
    if (nxt_slow_path(r == NULL)) {
        nxt_alert(task, "schedule run: no memory for the request");

        run->timer.handler = nxt_router_schedule_run_finish;
        nxt_timer_add(engine, &run->timer, 0);
        return;
    }

    r->task = *task;
    task = &r->task;

    r->conf = &sc->joint;
    r->protocol = NXT_HTTP_PROTO_DEVNULL;
    r->proto.any = &run->devnull;
    run->devnull.request = r;

    r->remote = sc->remote;
    r->local = sc->local;
    r->log_route = sc->skcf.log_route;

    /* The schedule's own deadline; r->timer carries the application's. */
    run->timer.handler = nxt_router_schedule_run_timeout;
    nxt_timer_add(engine, &run->timer, sched->timeout);

    ret = nxt_router_schedule_request_init(r, sched, &sc->skcf);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_http_request_error(task, r, (ret > 0) ? (nxt_http_status_t) ret
                                        : NXT_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    nxt_http_request_action(task, r, sched->action);
}


/*
 * Parse the configured request header into "r", then copy the results as
 * nxt_h1p_header_process() does.  Returns NXT_OK, NXT_ERROR, or an HTTP
 * status.
 */

static nxt_int_t
nxt_router_schedule_request_init(nxt_http_request_t *r,
    nxt_router_schedule_t *sched, nxt_socket_conf_t *skcf)
{
    u_char                    *p;
    nxt_int_t                 ret;
    nxt_buf_mem_t             mem;
    nxt_http_request_parse_t  *rp;

    /* The parser may rewrite a complex target in place: parse a copy. */

    p = nxt_mp_nget(r->mem_pool, sched->request.length);
    rp = nxt_mp_zget(r->mem_pool, sizeof(nxt_http_request_parse_t));

    if (nxt_slow_path(p == NULL || rp == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(p, sched->request.start, sched->request.length);

    mem.start = p;
    mem.pos = p;
    mem.free = p + sched->request.length;
    mem.end = mem.free;

    ret = nxt_http_parse_request_init(rp, r->mem_pool);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    rp->discard_unsafe_fields = skcf->discard_unsafe_fields;

    ret = nxt_http_parse_request(rp, &mem);
    if (nxt_slow_path(ret != NXT_DONE)) {
        return NXT_HTTP_BAD_REQUEST;
    }

    r->request_line.start = rp->method.start;
    r->request_line.length = rp->request_line_end - rp->method.start;

    r->target.start = rp->target_start;
    r->target.length = rp->target_end - rp->target_start;
    r->quoted_target = rp->quoted_target;

    r->version.start = rp->version.str;
    r->version.length = sizeof(rp->version.str);

    r->method = &rp->method;
    r->path = &rp->path;
    r->args = &rp->args;

    r->num_inline_fields = rp->num_inline_fields;
    nxt_memcpy(r->inline_fields, rp->inline_fields,
               sizeof(nxt_http_field_t) * r->num_inline_fields);
    r->fields = rp->fields;

    return nxt_http_fields_process(r->inline_fields, r->num_inline_fields,
                                   r->fields, &nxt_router_schedule_fields_hash,
                                   r);
}


/*
 * The schedule's "timeout" (section 7.2), on the worker engine.  It does
 * what "limits": {"timeout"} does, through the same function: retract a
 * queued message, leave a claimed one until it is acknowledged, abandon the
 * port of a running one, and answer 503.  It cannot stop the application.
 */

static void
nxt_router_schedule_run_timeout(nxt_task_t *task, void *obj, void *data)
{
    nxt_http_request_t         *r;
    nxt_router_schedule_run_t  *run;

    run = nxt_timer_data(obj, nxt_router_schedule_run_t, timer);

    r = run->devnull.request;

    /* Answered already: the response is on its way to the close. */

    if (r == NULL || r->req_rpc_data == NULL) {
        return;
    }

    if (!nxt_router_request_expire(task, r, r->req_rpc_data)) {
        nxt_timer_add(task->thread->engine, &run->timer,
                      NXT_SCHEDULE_CLAIM_RETRY);
        return;
    }

    run->timed_out = 1;
}


/*
 * The devnull close (section 6.4).  The request pool goes as soon as this
 * returns, so the rest waits for a zero-delay expiry of the run's own timer,
 * which also replaces the deadline if it is still armed.
 */

static void
nxt_router_schedule_run_close(nxt_task_t *task, nxt_http_devnull_t *dn,
    nxt_socket_conf_joint_t *joint)
{
    nxt_nsec_t                 now;
    nxt_event_engine_t         *engine;
    nxt_router_schedule_run_t  *run;

    run = (nxt_router_schedule_run_t *) dn;

    nxt_assert(joint == &run->schedules->joint);

    engine = task->thread->engine;
    now = nxt_thread_monotonic_time(task->thread);

    run->duration = (nxt_msec_t) ((now - run->started) / 1000000);

    run->timer.handler = nxt_router_schedule_run_finish;
    nxt_timer_add(engine, &run->timer, 0);
}


/*
 * Report to the main engine, then drop the run's joint reference.  The
 * order matters: the release may free the configuration, and may end the
 * thread if its engine is quitting; the run is the main engine's from the
 * post on and is not touched here again.
 */

static void
nxt_router_schedule_run_finish(nxt_task_t *task, void *obj, void *data)
{
    nxt_socket_conf_joint_t    *joint;
    nxt_router_schedule_run_t  *run;

    run = nxt_timer_data(obj, nxt_router_schedule_run_t, timer);

    joint = &run->schedules->joint;

    nxt_router_schedule_post(run->main, &run->work, nxt_router_schedule_done,
                             run);

    nxt_router_joint_release(task, joint);
}


/* The result, on the main engine (section 7). */

static void
nxt_router_schedule_done(nxt_task_t *task, void *obj, void *data)
{
    size_t                       i;
    u_char                       *p;
    nxt_http_status_t            status;
    nxt_router_schedule_run_t    *run;
    nxt_router_schedule_state_t  *state;

    run = obj;
    state = run->state;
    status = run->devnull.status;

    state->stat.running = 0;
    state->stat.last_status = status;
    state->stat.last_duration = run->duration;

    if (run->timed_out) {
        state->stat.timed_out++;

        nxt_log(task, NXT_LOG_WARN, "schedule \"%V\" run %uD: GET %*s%s "
                "timed out after %M ms", &state->stat.name, run->seq,
                run->uri_length, run->uri, run->uri_cut ? "..." : "",
                run->duration);

    } else if (status == 0 || status >= NXT_HTTP_BAD_REQUEST
               || run->devnull.discarded)
    {
        state->stat.failed++;

        p = run->devnull.head;

        for (i = 0; i < run->devnull.head_length; i++) {
            if (p[i] < 0x20 || p[i] >= 0x7F) {
                p[i] = '.';
            }
        }

        nxt_log(task, NXT_LOG_WARN, "schedule \"%V\" run %uD: GET %*s%s -> "
                "%d in %M ms: \"%*s\"", &state->stat.name, run->seq,
                run->uri_length, run->uri, run->uri_cut ? "..." : "",
                (int) status, run->duration, run->devnull.head_length, p);

    } else {
        nxt_log(task, NXT_LOG_INFO, "schedule \"%V\" run %uD: GET %*s%s -> "
                "%d in %M ms", &state->stat.name, run->seq, run->uri_length,
                run->uri, run->uri_cut ? "..." : "", (int) status,
                run->duration);
    }

    nxt_free(run);

    if (state->conf == NULL) {
        nxt_router_schedule_state_free(task, state);
        return;
    }

    if (state->pending) {
        state->pending = 0;
        nxt_router_schedule_start(task, state);
    }
}


nxt_int_t
nxt_router_schedule_msec(int64_t seconds, nxt_msec_t *out)
{
    if (seconds < 0 || seconds > NXT_SCHEDULE_SECONDS_MAX) {
        return NXT_ERROR;
    }

    *out = (nxt_msec_t) seconds * 1000;

    return NXT_OK;
}


/*
 * The wait before a run: "interval" plus a uniformly chosen part of
 * "jitter", both in milliseconds.  "rnd" is a uniform 32-bit random number;
 * the modulo bias is below 2^-10 for any jitter the validator admits.  The
 * result never exceeds NXT_SCHEDULE_MSEC_MAX, whatever the inputs.
 */

nxt_msec_t
nxt_router_schedule_delay(nxt_msec_t interval, nxt_msec_t jitter, uint32_t rnd)
{
    size_t  delay;

    if (jitter > NXT_SCHEDULE_MSEC_MAX) {
        jitter = NXT_SCHEDULE_MSEC_MAX;
    }

    if (nxt_size_add(interval, rnd % ((uint64_t) jitter + 1), &delay)
        || delay > NXT_SCHEDULE_MSEC_MAX)
    {
        return NXT_SCHEDULE_MSEC_MAX;
    }

    return (nxt_msec_t) delay;
}


/*
 * How much of "uri" may go into an info-level log line.  A cron URI
 * usually carries its key ("/cron/SECRET_KEY"), in the last path segment or
 * in the query, so only the path up to and including its last "/" is shown.
 */

size_t
nxt_router_schedule_uri_public(const nxt_str_t *uri)
{
    u_char  *query;
    size_t  n;

    query = memchr(uri->start, '?', uri->length);
    n = (query != NULL) ? (size_t) (query - uri->start) : uri->length;

    while (n > 0 && uri->start[n - 1] != '/') {
        n--;
    }

    return n;
}


/*
 * Build "GET <uri> HTTP/1.1\r\n<headers>\r\n" once per configuration.  Each
 * run copies it and hands the copy to nxt_http_parse_request(), so the run
 * gets exactly the target normalisation, path and arguments a client
 * request would.  A "User-Agent" naming the schedule is added unless the
 * configuration sets one.
 */

nxt_int_t
nxt_router_schedule_request_build(nxt_mp_t *mp, nxt_router_schedule_t *sched,
    nxt_conf_value_t *headers)
{
    u_char            *p, *end;
    size_t            size;
    uint32_t          next;
    nxt_str_t         name, value;
    nxt_bool_t        user_agent;
    nxt_conf_value_t  *member;

    static const char  method[] = "GET ";
    static const char  version[] = " HTTP/1.1\r\n";
    static const char  ua[] = "User-Agent: FreeUnit-Schedule/";

    user_agent = 0;

    if (nxt_size_add(nxt_length(method) + nxt_length(version)
                     + nxt_length("\r\n"), sched->uri.length, &size))
    {
        return NXT_ERROR;
    }

    next = 0;

    while (headers != NULL) {
        member = nxt_conf_next_object_member(headers, &name, &next);
        if (member == NULL) {
            break;
        }

        nxt_conf_get_string(member, &value);

        if (nxt_size_add(size, name.length, &size)
            || nxt_size_add(size, value.length, &size)
            || nxt_size_add(size, nxt_length(": \r\n"), &size))
        {
            return NXT_ERROR;
        }

        if (name.length == nxt_length("User-Agent")
            && nxt_strncasecmp(name.start, (u_char *) "User-Agent",
                               name.length) == 0)
        {
            user_agent = 1;
        }
    }

    if (!user_agent
        && (nxt_size_add(size, nxt_length(ua), &size)
            || nxt_size_add(size, sched->name.length, &size)
            || nxt_size_add(size, nxt_length("\r\n"), &size)))
    {
        return NXT_ERROR;
    }

    p = nxt_mp_nget(mp, size);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    sched->request.start = p;
    end = p + size;

    p = nxt_cpymem(p, method, nxt_length(method));
    p = nxt_cpymem(p, sched->uri.start, sched->uri.length);
    p = nxt_cpymem(p, version, nxt_length(version));

    next = 0;

    while (headers != NULL) {
        member = nxt_conf_next_object_member(headers, &name, &next);
        if (member == NULL) {
            break;
        }

        nxt_conf_get_string(member, &value);

        p = nxt_cpymem(p, name.start, name.length);
        *p++ = ':'; *p++ = ' ';
        p = nxt_cpymem(p, value.start, value.length);
        *p++ = '\r'; *p++ = '\n';
    }

    if (!user_agent) {
        p = nxt_cpymem(p, ua, nxt_length(ua));
        p = nxt_cpymem(p, sched->name.start, sched->name.length);
        *p++ = '\r'; *p++ = '\n';
    }

    *p++ = '\r'; *p++ = '\n';

    if (nxt_slow_path(p != end)) {
        return NXT_ERROR;
    }

    sched->request.length = size;

    return NXT_OK;
}


/*
 * /status (docs/observability/status-extensions.md), on the main engine
 * like the states themselves.  A removed schedule whose last run is still
 * in flight is included until that run ends.
 */

size_t
nxt_router_schedules_status_size(nxt_uint_t *n)
{
    size_t                       size;
    nxt_router_schedule_state_t  *state;

    *n = 0;
    size = 0;

    nxt_queue_each(state, &nxt_router_schedule_states,
                   nxt_router_schedule_state_t, link)
    {
        (*n)++;
        size += sizeof(nxt_status_schedule_t) + state->stat.name.length;

    } nxt_queue_loop;

    return size;
}


/* Names are copied down from "p"; their offsets are relative to "base". */

void
nxt_router_schedules_status(nxt_status_schedule_t *stat, u_char *p,
    u_char *base)
{
    nxt_router_schedule_state_t  *state;

    nxt_queue_each(state, &nxt_router_schedule_states,
                   nxt_router_schedule_state_t, link)
    {
        p -= state->stat.name.length;
        nxt_memcpy(p, state->stat.name.start, state->stat.name.length);

        *stat = state->stat;
        stat->name.start = (u_char *) (p - base);
        stat++;

    } nxt_queue_loop;
}
