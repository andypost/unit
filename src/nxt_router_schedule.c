
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
#include <nxt_router_schedule.h>


/* The longest single wait, in milliseconds; see nxt_router_schedule.h. */
#define NXT_SCHEDULE_MSEC_MAX  ((nxt_msec_t) NXT_SCHEDULE_SECONDS_MAX * 1000)


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


typedef struct {
    uint8_t                  discard_unsafe_fields;
    uint8_t                  log_route;
    uint8_t                  server_version;
} nxt_router_schedule_http_conf_t;


static nxt_int_t nxt_router_schedules_create(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf, nxt_conf_value_t *root);
static nxt_int_t nxt_router_schedule_create(nxt_task_t *task,
    nxt_router_temp_conf_t *tmcf, nxt_str_t *name, nxt_conf_value_t *value,
    nxt_router_schedule_t *sched);


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


static nxt_conf_map_t  nxt_router_schedule_http_conf[] = {
    {
        nxt_string("discard_unsafe_fields"),
        NXT_CONF_MAP_INT8,
        offsetof(nxt_router_schedule_http_conf_t, discard_unsafe_fields),
    },

    {
        nxt_string("log_route"),
        NXT_CONF_MAP_INT8,
        offsetof(nxt_router_schedule_http_conf_t, log_route),
    },

    {
        nxt_string("server_version"),
        NXT_CONF_MAP_INT8,
        offsetof(nxt_router_schedule_http_conf_t, server_version),
    },
};


/*
 * The joint of the configuration applied last, or NULL.  Its own reference
 * (count 1 at creation) is what keeps the configuration alive for the
 * schedules; it is dropped when the next configuration replaces it.  Only
 * the router's main thread reads or writes this.
 */
static nxt_socket_conf_joint_t  *nxt_router_schedule_joint;


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
 * The internal socket configuration and joint (section 6.3).  A run's
 * request needs a real r->conf: nxt_http_request_header_send(), the close
 * handler, the access log and compression all dereference it without a
 * check.  The listener defaults are copied so that anything reading them
 * sees the value a listener would; only the "settings.http" members the
 * run can observe are mapped, because the rest concern a connection.
 */

nxt_int_t
nxt_router_schedules_joint_init(nxt_task_t *task, nxt_router_conf_t *rtcf,
    nxt_router_schedules_t *sc, nxt_conf_value_t *http)
{
    nxt_int_t                        ret;
    nxt_socket_conf_t                *skcf;
    nxt_socket_conf_joint_t          *joint;
    nxt_router_schedule_http_conf_t  hcf;

    static nxt_str_t  remote = nxt_string("127.0.0.1");
    static nxt_str_t  local = nxt_string("127.0.0.1:80");

    hcf.discard_unsafe_fields = 1;
    hcf.log_route = 0;
    hcf.server_version = 1;

    if (http != NULL) {
        ret = nxt_conf_map_object(rtcf->mem_pool, http,
                                  nxt_router_schedule_http_conf,
                                  nxt_nitems(nxt_router_schedule_http_conf),
                                  &hcf);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }
    }

    skcf = &sc->skcf;

    /* The defaults of a listener in nxt_router_conf_create(). */
    skcf->header_buffer_size = 2048;
    skcf->large_header_buffer_size = 8192;
    skcf->large_header_buffers = 4;
    skcf->body_buffer_size = 16 * 1024;
    skcf->max_body_size = 8 * 1024 * 1024;
    skcf->proxy_header_buffer_size = 64 * 1024;
    skcf->proxy_buffer_size = 4096;
    skcf->proxy_buffers = 256;
    skcf->idle_timeout = 30 * 1000;
    skcf->header_read_timeout = 30 * 1000;
    skcf->body_read_timeout = 30 * 1000;
    skcf->send_timeout = 30 * 1000;
    skcf->proxy_timeout = 60 * 1000;
    skcf->proxy_send_timeout = 30 * 1000;
    skcf->proxy_read_timeout = 30 * 1000;
    skcf->websocket_conf.max_frame_size = 1024 * 1024;
    skcf->websocket_conf.read_timeout = 60 * 1000;
    skcf->websocket_conf.keepalive_interval = 30 * 1000;

    skcf->discard_unsafe_fields = hcf.discard_unsafe_fields;
    skcf->log_route = hcf.log_route;
    skcf->server_version = hcf.server_version;

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
 * Called from nxt_router_conf_apply() once the new configuration can no
 * longer fail.  The new configuration's joint becomes current, and the
 * previous one's own reference is dropped; the previous configuration is
 * freed then, unless something else still holds it.
 */

void
nxt_router_schedules_apply(nxt_task_t *task, nxt_router_temp_conf_t *tmcf)
{
    nxt_router_schedules_t   *sc;
    nxt_socket_conf_joint_t  *old;

    old = nxt_router_schedule_joint;
    sc = tmcf->router_conf->schedules;

    nxt_router_schedule_joint = (sc != NULL) ? &sc->joint : NULL;

    if (sc != NULL) {
        nxt_log(task, NXT_LOG_INFO, "%uD schedule(s) configured",
                sc->nschedules);
    }

    if (old != NULL) {
        nxt_router_conf_release(task, old);
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

    nxt_assert(p == end);

    sched->request.length = size;

    return NXT_OK;
}
