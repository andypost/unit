
/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * Unit tests for "schedules" (docs/adr/0004-schedules.md):
 *
 *   - the seconds-to-milliseconds conversion and its range;
 *   - the wait before a run: interval plus a uniform part of jitter, never
 *     past the signed 32-bit timer range, whatever the inputs;
 *   - how much of a URI may be logged at info level;
 *   - the request header built once per configuration, which must parse to
 *     the same target, path and arguments as the same line from a client;
 *   - the reference chain run -> joint -> skcf -> rtcf: the configuration
 *     outlives both the joint's own reference and a run's, in either order,
 *     and the self-linked skcf never touches router->sockets;
 *   - the devnull protocol slot: the response is counted and its head kept,
 *     every buffer, "last" included, is completed exactly once and never
 *     inline, and the close reaches the owner once.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_conf.h>
#include <nxt_http.h>
#include <nxt_router_schedule.h>
#include <nxt_http_devnull.h>
#include <nxt_event_engine.h>
#include "nxt_tests.h"


#define NXT_SCHEDULE_TEST_MSEC_MAX  ((nxt_msec_t) NXT_SCHEDULE_SECONDS_MAX * 1000)


static nxt_int_t
nxt_router_schedule_msec_test(nxt_thread_t *thr)
{
    nxt_msec_t  ms;

    if (nxt_router_schedule_msec(0, &ms) != NXT_OK || ms != 0) {
        nxt_log_alert(thr->log, "schedule msec(0) failed");
        return NXT_ERROR;
    }

    if (nxt_router_schedule_msec(300, &ms) != NXT_OK || ms != 300000) {
        nxt_log_alert(thr->log, "schedule msec(300) failed");
        return NXT_ERROR;
    }

    if (nxt_router_schedule_msec(NXT_SCHEDULE_SECONDS_MAX, &ms) != NXT_OK
        || ms != NXT_SCHEDULE_TEST_MSEC_MAX
        || (int32_t) ms < 0)
    {
        nxt_log_alert(thr->log, "schedule msec(max) failed");
        return NXT_ERROR;
    }

    if (nxt_router_schedule_msec(NXT_SCHEDULE_SECONDS_MAX + 1, &ms) == NXT_OK
        || nxt_router_schedule_msec(-1, &ms) == NXT_OK
        || nxt_router_schedule_msec(INT64_MAX, &ms) == NXT_OK
        || nxt_router_schedule_msec(INT64_MIN, &ms) == NXT_OK)
    {
        nxt_log_alert(thr->log, "schedule msec accepted an out-of-range "
                      "value");
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_router_schedule_delay_test(nxt_thread_t *thr)
{
    uint32_t      rnd, i;
    nxt_msec_t    d, lo, hi;
    nxt_random_t  random;
    nxt_bool_t    seen[11];

    /* No jitter: exactly the interval, for any random number. */

    if (nxt_router_schedule_delay(5000, 0, 0) != 5000
        || nxt_router_schedule_delay(5000, 0, UINT32_MAX) != 5000)
    {
        nxt_log_alert(thr->log, "schedule delay without jitter failed");
        return NXT_ERROR;
    }

    /* The two ends of the range are both reachable. */

    if (nxt_router_schedule_delay(1000, 10, 0) != 1000
        || nxt_router_schedule_delay(1000, 10, 10) != 1010
        || nxt_router_schedule_delay(1000, 10, 11) != 1000)
    {
        nxt_log_alert(thr->log, "schedule delay range ends failed");
        return NXT_ERROR;
    }

    /* Every value of a small range is produced, and none outside it. */

    nxt_memzero(seen, sizeof(seen));

    for (rnd = 0; rnd < 1000; rnd++) {
        d = nxt_router_schedule_delay(1000, 10, rnd);

        if (d < 1000 || d > 1010) {
            nxt_log_alert(thr->log, "schedule delay %M out of [1000, 1010]",
                          d);
            return NXT_ERROR;
        }

        seen[d - 1000] = 1;
    }

    for (i = 0; i < nxt_nitems(seen); i++) {
        if (!seen[i]) {
            nxt_log_alert(thr->log, "schedule delay never chose +%uD", i);
            return NXT_ERROR;
        }
    }

    /* The random source the router uses stays within bounds. */

    nxt_random_init(&random);

    lo = 15000;
    hi = 0;

    for (i = 0; i < 100000; i++) {
        d = nxt_router_schedule_delay(60000, 15000, nxt_random(&random));

        if (d < 60000 || d > 75000) {
            nxt_log_alert(thr->log, "schedule delay %M out of [60000, 75000]",
                          d);
            return NXT_ERROR;
        }

        lo = nxt_min(lo, d - 60000);
        hi = nxt_max(hi, d - 60000);
    }

    if (lo > 150 || hi < 14850) {
        nxt_log_alert(thr->log, "schedule delay spread [%M, %M] is too "
                      "narrow", lo, hi);
        return NXT_ERROR;
    }

    /* The largest configuration is at the limit, and nothing passes it. */

    d = nxt_router_schedule_delay(NXT_SCHEDULE_TEST_MSEC_MAX / 2,
                                  NXT_SCHEDULE_TEST_MSEC_MAX / 2, UINT32_MAX);

    if (d > NXT_SCHEDULE_TEST_MSEC_MAX || (int32_t) d < 0) {
        nxt_log_alert(thr->log, "schedule delay %M past the maximum", d);
        return NXT_ERROR;
    }

    if (nxt_router_schedule_delay(UINT32_MAX, UINT32_MAX, UINT32_MAX)
            != NXT_SCHEDULE_TEST_MSEC_MAX
        || nxt_router_schedule_delay(NXT_SCHEDULE_TEST_MSEC_MAX, 1000, 999)
            != NXT_SCHEDULE_TEST_MSEC_MAX)
    {
        nxt_log_alert(thr->log, "schedule delay did not clamp");
        return NXT_ERROR;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_router_schedule_uri_public_test(nxt_thread_t *thr)
{
    size_t      n;
    nxt_uint_t  i;

    static const struct {
        nxt_str_t  uri;
        size_t     public;
    } tests[] = {
        { nxt_string("/cron/SECRET_KEY"),        6 },
        { nxt_string("/cron"),                   1 },
        { nxt_string("/"),                       1 },
        { nxt_string("/a/b/"),                   5 },
        { nxt_string("/a/b/?key=x/y"),           5 },
        { nxt_string("/cron?key=a/b/c"),         1 },
        { nxt_string("/core/cron.php?cron_key"), 6 },
        { nxt_string("x"),                       0 },
    };

    for (i = 0; i < nxt_nitems(tests); i++) {
        n = nxt_router_schedule_uri_public(&tests[i].uri);

        if (n != tests[i].public) {
            nxt_log_alert(thr->log, "schedule uri public \"%V\": %uz, "
                          "expected %uz", &tests[i].uri, n, tests[i].public);
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


static nxt_int_t
nxt_router_schedule_parse(nxt_mp_t *mp, nxt_str_t *text,
    nxt_http_request_parse_t *rp)
{
    u_char         *p;
    nxt_buf_mem_t  mem;

    /* The parser may rewrite a complex target in place: work on a copy. */

    p = nxt_mp_nget(mp, text->length);
    if (p == NULL) {
        return NXT_ERROR;
    }

    nxt_memcpy(p, text->start, text->length);

    mem.start = p;
    mem.pos = p;
    mem.free = p + text->length;
    mem.end = mem.free;

    nxt_memzero(rp, sizeof(nxt_http_request_parse_t));

    if (nxt_http_parse_request_init(rp, mp) != NXT_OK) {
        return NXT_ERROR;
    }

    return nxt_http_parse_request(rp, &mem);
}


static nxt_http_field_t *
nxt_router_schedule_field(nxt_http_request_parse_t *rp, const char *name)
{
    size_t            len;
    nxt_http_field_t  *f;

    len = nxt_strlen(name);

    nxt_http_fields_each(f, rp->inline_fields, rp->num_inline_fields,
                         rp->fields)
    {
        if (f->name_length == len
            && nxt_strncasecmp(f->name, (u_char *) name, len) == 0)
        {
            return f;
        }

    } nxt_http_fields_loop;

    return NULL;
}


static nxt_int_t
nxt_router_schedule_field_is(nxt_http_request_parse_t *rp, const char *name,
    const char *value)
{
    nxt_http_field_t  *f;

    f = nxt_router_schedule_field(rp, name);

    return f != NULL
           && f->value_length == nxt_strlen(value)
           && memcmp(f->value, value, f->value_length) == 0;
}


static nxt_int_t
nxt_router_schedule_request_test(nxt_thread_t *thr)
{
    nxt_mp_t                  *mp;
    nxt_int_t                 ret;
    nxt_str_t                 json, line, target_s, target_c;
    nxt_uint_t                n;
    nxt_conf_value_t          *headers;
    nxt_http_field_t          *f;
    nxt_router_schedule_t     sched;
    nxt_http_request_parse_t  rs, rc;

    static const char  uri[] = "/a/%2E%2E/b/../c%2Fd/./e?x=1&y=%2F";

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    ret = NXT_ERROR;

    nxt_memzero(&sched, sizeof(sched));
    nxt_str_set(&sched.name, "drupal-cron");
    nxt_str_set(&sched.uri, uri);

    nxt_str_set(&json, "{\"Host\": \"example.org\", \"X-Cron\": \"a\\tb\"}");

    headers = nxt_conf_json_parse_str(mp, &json);
    if (headers == NULL) {
        nxt_log_alert(thr->log, "schedule request: headers json");
        goto done;
    }

    if (nxt_router_schedule_request_build(mp, &sched, headers) != NXT_OK) {
        nxt_log_alert(thr->log, "schedule request build failed");
        goto done;
    }

    if (nxt_router_schedule_parse(mp, &sched.request, &rs) != NXT_DONE) {
        nxt_log_alert(thr->log, "schedule request did not parse: \"%V\"",
                      &sched.request);
        goto done;
    }

    /* The same target as a client would send it. */

    nxt_str_set(&line, "GET /a/%2E%2E/b/../c%2Fd/./e?x=1&y=%2F HTTP/1.1\r\n"
                       "Host: example.org\r\n\r\n");

    if (nxt_router_schedule_parse(mp, &line, &rc) != NXT_DONE) {
        nxt_log_alert(thr->log, "schedule request: reference did not parse");
        goto done;
    }

    target_s.start = rs.target_start;
    target_s.length = rs.target_end - rs.target_start;
    target_c.start = rc.target_start;
    target_c.length = rc.target_end - rc.target_start;

    if (!nxt_strstr_eq(&rs.method, &rc.method)
        || !nxt_str_eq(&rs.method, "GET", 3)
        || !nxt_strstr_eq(&target_s, &target_c)
        || !nxt_strstr_eq(&rs.path, &rc.path)
        || !nxt_strstr_eq(&rs.args, &rc.args)
        || rs.complex_target != rc.complex_target
        || rs.quoted_target != rc.quoted_target
        || rs.version.ui64 != rc.version.ui64)
    {
        nxt_log_alert(thr->log, "schedule request: \"%V\" \"%V\" differs "
                      "from \"%V\" \"%V\"", &rs.path, &rs.args, &rc.path,
                      &rc.args);
        goto done;
    }

    if (!nxt_str_eq(&rs.args, "x=1&y=%2F", 9)
        || rs.path.length == 0
        || rs.path.start[0] != '/')
    {
        nxt_log_alert(thr->log, "schedule request: path \"%V\" args \"%V\"",
                      &rs.path, &rs.args);
        goto done;
    }

    if (!nxt_router_schedule_field_is(&rs, "Host", "example.org")
        || !nxt_router_schedule_field_is(&rs, "X-Cron", "a\tb")
        || !nxt_router_schedule_field_is(&rs, "User-Agent",
                                         "FreeUnit-Schedule/drupal-cron"))
    {
        nxt_log_alert(thr->log, "schedule request: fields \"%V\"",
                      &sched.request);
        goto done;
    }

    /* A configured User-Agent replaces the default one, it is not added. */

    nxt_str_set(&json, "{\"user-agent\": \"cron/1\"}");

    headers = nxt_conf_json_parse_str(mp, &json);

    if (headers == NULL
        || nxt_router_schedule_request_build(mp, &sched, headers) != NXT_OK
        || nxt_router_schedule_parse(mp, &sched.request, &rs) != NXT_DONE)
    {
        nxt_log_alert(thr->log, "schedule request with user-agent failed");
        goto done;
    }

    n = 0;

    nxt_http_fields_each(f, rs.inline_fields, rs.num_inline_fields, rs.fields)
    {
        if (f->name_length == nxt_length("User-Agent")
            && nxt_strncasecmp(f->name, (u_char *) "User-Agent",
                               f->name_length) == 0)
        {
            n++;
        }

    } nxt_http_fields_loop;

    if (n != 1 || !nxt_router_schedule_field_is(&rs, "User-Agent", "cron/1")) {
        nxt_log_alert(thr->log, "schedule request: %ui User-Agent fields",
                      n);
        goto done;
    }

    /* No headers at all. */

    if (nxt_router_schedule_request_build(mp, &sched, NULL) != NXT_OK
        || nxt_router_schedule_parse(mp, &sched.request, &rs) != NXT_DONE
        || nxt_router_schedule_field(&rs, "Host") != NULL
        || !nxt_router_schedule_field_is(&rs, "User-Agent",
                                         "FreeUnit-Schedule/drupal-cron"))
    {
        nxt_log_alert(thr->log, "schedule request without headers failed");
        goto done;
    }

    ret = NXT_OK;

done:

    nxt_mp_destroy(mp);

    return ret;
}


/*
 * A configuration as nxt_router_conf_create() leaves it, with "count" set
 * to what the listeners would hold.
 */

static nxt_router_conf_t *
nxt_router_schedule_test_rtcf(nxt_router_t *router, uint32_t count)
{
    nxt_mp_t           *mp;
    nxt_router_conf_t  *rtcf;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NULL;
    }

    rtcf = nxt_mp_zget(mp, sizeof(nxt_router_conf_t));
    if (rtcf == NULL) {
        nxt_mp_destroy(mp);
        return NULL;
    }

    rtcf->mem_pool = mp;
    rtcf->router = router;
    rtcf->count = count;

    rtcf->tstr_state = nxt_tstr_state_new(mp, 0);
    if (rtcf->tstr_state == NULL) {
        nxt_mp_destroy(mp);
        return NULL;
    }

    return rtcf;
}


static nxt_int_t
nxt_router_schedule_joint_test(nxt_thread_t *thr)
{
    uint32_t                 held;
    nxt_uint_t               pass;
    nxt_task_t               *task;
    nxt_router_t             router;
    nxt_queue_link_t         sentinel;
    nxt_router_conf_t        *rtcf;
    nxt_socket_conf_t        *skcf;
    nxt_router_schedules_t   *sc;
    nxt_socket_conf_joint_t  *joint;

    task = thr->task;

    nxt_memzero(&router, sizeof(router));
    nxt_queue_init(&router.sockets);
    nxt_queue_insert_tail(&router.sockets, &sentinel);

    /*
     * Pass 0: a listener also holds the configuration, so the schedule's
     * release is observable as rtcf->count going from 2 to 1.  Pass 1: the
     * schedule holds the only reference, and its release frees the
     * configuration (checked by the sanitizer builds).
     */

    for (pass = 0; pass < 2; pass++) {
        held = (pass == 0);

        rtcf = nxt_router_schedule_test_rtcf(&router, held);
        if (rtcf == NULL) {
            return NXT_ERROR;
        }

        sc = nxt_mp_zget(rtcf->mem_pool, sizeof(nxt_router_schedules_t));
        if (sc == NULL
            || nxt_router_schedules_joint_init(task, rtcf, sc, NULL) != NXT_OK)
        {
            nxt_log_alert(thr->log, "schedule joint init failed");
            return NXT_ERROR;
        }

        joint = &sc->joint;
        skcf = &sc->skcf;

        if (rtcf->count != held + 1 || skcf->count != 1 || joint->count != 1
            || joint->socket_conf != skcf || skcf->router_conf != rtcf
            || sc->local == NULL || sc->remote == NULL
            || nxt_sockaddr_port_number(sc->local) != 80
            || !skcf->server_version || !skcf->discard_unsafe_fields)
        {
            nxt_log_alert(thr->log, "schedule joint init: rtcf %uD skcf %uD "
                          "joint %uD", rtcf->count, skcf->count,
                          joint->count);
            return NXT_ERROR;
        }

        /* A run takes its reference, as nxt_router_schedule_run() does. */

        joint->count++;

        /*
         * The joint's own reference goes (the next configuration was
         * applied) while the run is in flight: nothing may be freed.
         */

        nxt_router_conf_release(task, joint);

        if (joint->count != 1 || skcf->count != 1 || rtcf->count != held + 1)
        {
            nxt_log_alert(thr->log, "schedule joint (pass %ui): released "
                          "too early", pass);
            return NXT_ERROR;
        }

        /* The run ends. */

        nxt_router_conf_release(task, joint);

        if (!held) {
            continue;
        }

        /* The schedule's reference on the rtcf went, and only it. */

        if (rtcf->count != 1) {
            nxt_log_alert(thr->log, "schedule joint: rtcf count %uD, "
                          "expected 1", rtcf->count);
            return NXT_ERROR;
        }

        nxt_tstr_state_release(rtcf->tstr_state);
        nxt_mp_destroy(rtcf->mem_pool);
    }

    /* The self-linked skcf and joint never touched router->sockets. */

    if (nxt_queue_first(&router.sockets) != &sentinel
        || nxt_queue_last(&router.sockets) != &sentinel
        || nxt_queue_next(&sentinel) != nxt_queue_tail(&router.sockets))
    {
        nxt_log_alert(thr->log, "schedule joint: router->sockets changed");
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * The parse through the real nxt_router_conf_resolve(): no schedules is a
 * no-op, and a schedule whose application is missing fails the whole
 * configuration rather than being dropped.
 */

static nxt_int_t
nxt_router_schedule_resolve_test(nxt_thread_t *thr)
{
    nxt_int_t               ret;
    nxt_str_t               json;
    nxt_uint_t              i;
    nxt_task_t              *task;
    nxt_conf_value_t        *root;
    nxt_router_conf_t       *rtcf;
    nxt_router_temp_conf_t  *tmcf;

    static const struct {
        const char  *json;
        nxt_int_t   ret;
    } tests[] = {
        { "{}", NXT_OK },
        { "{\"schedules\": {}}", NXT_OK },
        { "{\"schedules\": {\"x\": {\"pass\": \"applications/missing\","
          "\"uri\": \"/\", \"interval\": 1}}}", NXT_ERROR },
    };

    task = thr->task;

    for (i = 0; i < nxt_nitems(tests); i++) {
        tmcf = nxt_router_test_temp_conf(task);
        if (tmcf == NULL) {
            return NXT_ERROR;
        }

        rtcf = tmcf->router_conf;

        json.start = (u_char *) tests[i].json;
        json.length = nxt_strlen(tests[i].json);

        root = nxt_conf_json_parse_str(tmcf->mem_pool, &json);
        if (root == NULL) {
            return NXT_ERROR;
        }

        ret = nxt_router_conf_resolve(task, tmcf, root);

        if (ret != tests[i].ret || rtcf->schedules != NULL
            || rtcf->count != 0)
        {
            nxt_log_alert(thr->log, "schedule resolve \"%s\": %i, "
                          "schedules %p, count %uD", tests[i].json, ret,
                          rtcf->schedules, rtcf->count);
            return NXT_ERROR;
        }

        nxt_tstr_state_release(rtcf->tstr_state);
        nxt_mp_destroy(rtcf->mem_pool);
        nxt_mp_destroy(tmcf->mem_pool);
    }

    return NXT_OK;
}


/*
 * The devnull protocol (ADR section 6.4), through the nxt_http_proto[]
 * slot the request code calls: the response is counted and its head kept,
 * every buffer is completed exactly once -- which is what ends the request
 * -- and nothing runs inside the caller's frame.
 */

static nxt_uint_t  nxt_router_schedule_test_mem_done;
static nxt_uint_t  nxt_router_schedule_test_last_done;
static nxt_uint_t  nxt_router_schedule_test_body_calls;
static nxt_uint_t  nxt_router_schedule_test_closes;


static void
nxt_router_schedule_test_mem_completion(nxt_task_t *task, void *obj,
    void *data)
{
    nxt_buf_t  *b;

    for (b = obj; b != NULL; b = b->next) {
        nxt_router_schedule_test_mem_done++;
    }
}


static void
nxt_router_schedule_test_last_completion(nxt_task_t *task, void *obj,
    void *data)
{
    nxt_router_schedule_test_last_done++;
}


static void
nxt_router_schedule_test_body(nxt_task_t *task, void *obj, void *data)
{
    nxt_router_schedule_test_body_calls++;
}


static void
nxt_router_schedule_test_close(nxt_task_t *task, nxt_http_devnull_t *dn,
    nxt_socket_conf_joint_t *joint)
{
    nxt_router_schedule_test_closes++;
}


static void
nxt_router_schedule_test_drain(nxt_task_t *task, nxt_event_engine_t *engine)
{
    void                *obj, *data;
    nxt_task_t          *wq_task;
    nxt_work_handler_t  handler;

    while (engine->fast_work_queue.head != NULL) {
        handler = nxt_work_queue_pop(&engine->fast_work_queue, &wq_task, &obj,
                                     &data);
        if (handler != NULL) {
            handler(wq_task != NULL ? wq_task : task, obj, data);
        }
    }
}


static nxt_buf_t *
nxt_router_schedule_test_last(nxt_http_request_t *r)
{
    nxt_buf_t  *last;

    last = nxt_mp_zget(r->mem_pool, NXT_BUF_SYNC_SIZE);
    if (last == NULL) {
        return NULL;
    }

    nxt_buf_set_sync(last);
    nxt_buf_set_last(last);
    last->completion_handler = nxt_router_schedule_test_last_completion;
    last->parent = r;

    return last;
}


static nxt_buf_t *
nxt_router_schedule_test_mem(nxt_http_request_t *r, const char *text)
{
    size_t     len;
    nxt_buf_t  *b;

    len = nxt_strlen(text);

    b = nxt_buf_mem_alloc(r->mem_pool, len, 0);
    if (b == NULL) {
        return NULL;
    }

    b->mem.free = nxt_cpymem(b->mem.free, text, len);
    b->completion_handler = nxt_router_schedule_test_mem_completion;
    b->parent = r;

    return b;
}


static nxt_int_t
nxt_router_schedule_devnull_test(nxt_thread_t *thr)
{
    nxt_mp_t                  *mp;
    nxt_int_t                 ret;
    nxt_buf_t                 *a, *b, *last;
    nxt_task_t                *task;
    nxt_http_proto_t          proto;
    nxt_http_request_t        *r;
    nxt_http_devnull_t        dn;
    nxt_event_engine_t        engine, *saved_engine;
    nxt_socket_conf_joint_t   joint;

    const nxt_http_proto_table_t  *devnull;

    static const char  big[] = "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"
                               "tail beyond the head";

    task = thr->task;
    task->thread = thr;
    saved_engine = thr->engine;

    ret = NXT_ERROR;
    r = NULL;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return NXT_ERROR;
    }

    nxt_memzero(&engine, sizeof(engine));
    nxt_work_queue_cache_create(&engine.work_queue_cache, 64);
    engine.fast_work_queue.cache = &engine.work_queue_cache;
    nxt_work_queue_name(&engine.fast_work_queue, "fast");
    engine.mem_pool = mp;
    engine.task.thread = thr;
    engine.task.log = thr->log;

    thr->engine = &engine;

    devnull = &nxt_http_proto[NXT_HTTP_PROTO_DEVNULL];

    if (devnull->send == NULL || devnull->header_send == NULL
        || devnull->body_bytes_sent == NULL || devnull->discard == NULL
        || devnull->close == NULL || devnull->body_read == NULL
        || devnull->local_addr == NULL)
    {
        nxt_log_alert(thr->log, "devnull: the protocol slot is not filled");
        goto done;
    }

    r = nxt_http_request_create(task);
    if (r == NULL) {
        goto done;
    }

    nxt_memzero(&dn, sizeof(dn));
    dn.request = r;
    dn.close = nxt_router_schedule_test_close;

    r->protocol = NXT_HTTP_PROTO_DEVNULL;
    r->proto.any = &dn;
    r->status = NXT_HTTP_OK;

    /* The header: queued body handler, nothing called in place. */

    devnull->header_send(task, r, nxt_router_schedule_test_body, NULL);

    if (nxt_router_schedule_test_body_calls != 0 || !r->header_sent
        || dn.status != NXT_HTTP_OK)
    {
        nxt_log_alert(thr->log, "devnull: header send ran the body inline");
        goto done;
    }

    nxt_router_schedule_test_drain(task, &engine);

    if (nxt_router_schedule_test_body_calls != 1) {
        nxt_log_alert(thr->log, "devnull: body handler ran %ui times",
                      nxt_router_schedule_test_body_calls);
        goto done;
    }

    /* The body: counted, head kept, every buffer completed once. */

    a = nxt_router_schedule_test_mem(r, "hello ");
    b = nxt_router_schedule_test_mem(r, big);
    last = nxt_router_schedule_test_last(r);

    if (a == NULL || b == NULL || last == NULL) {
        goto done;
    }

    a->next = b;
    b->next = last;

    devnull->send(task, r, a);

    if (nxt_router_schedule_test_last_done != 0) {
        nxt_log_alert(thr->log, "devnull: \"last\" completed inline");
        goto done;
    }

    nxt_router_schedule_test_drain(task, &engine);

    proto.any = &dn;

    if (nxt_router_schedule_test_mem_done != 2
        || nxt_router_schedule_test_last_done != 1
        || devnull->body_bytes_sent(task, proto)
           != (nxt_off_t) (6 + nxt_length(big))
        || dn.head_length != NXT_HTTP_DEVNULL_HEAD
        || memcmp(dn.head, "hello 0123", 10) != 0
        || dn.head[NXT_HTTP_DEVNULL_HEAD - 1] != big[NXT_HTTP_DEVNULL_HEAD - 7])
    {
        nxt_log_alert(thr->log, "devnull send: mem %ui, last %ui, bytes %O, "
                      "head %uz", nxt_router_schedule_test_mem_done,
                      nxt_router_schedule_test_last_done,
                      devnull->body_bytes_sent(task, proto), dn.head_length);
        goto done;
    }

    /* A header with no body handler completes "last" itself. */

    r->last = nxt_router_schedule_test_last(r);
    devnull->header_send(task, r, NULL, NULL);
    nxt_router_schedule_test_drain(task, &engine);

    if (r->last != NULL || nxt_router_schedule_test_last_done != 2) {
        nxt_log_alert(thr->log, "devnull: header without body left \"last\"");
        goto done;
    }

    /* An error discards: "last" completes, the run is marked. */

    devnull->discard(task, r, nxt_router_schedule_test_last(r));
    nxt_router_schedule_test_drain(task, &engine);

    if (!dn.discarded || nxt_router_schedule_test_last_done != 3) {
        nxt_log_alert(thr->log, "devnull: discard did not complete \"last\"");
        goto done;
    }

    /* The close reports to the owner once and forgets the request. */

    r->status = NXT_HTTP_SERVICE_UNAVAILABLE;
    nxt_memzero(&joint, sizeof(joint));

    devnull->close(task, proto, &joint);

    if (nxt_router_schedule_test_closes != 1 || dn.request != NULL
        || dn.status != NXT_HTTP_SERVICE_UNAVAILABLE)
    {
        nxt_log_alert(thr->log, "devnull: close reported %ui times",
                      nxt_router_schedule_test_closes);
        goto done;
    }

    ret = NXT_OK;

done:

    if (r != NULL) {
        nxt_mp_release(r->mem_pool);
    }

    thr->engine = saved_engine;

    nxt_work_queue_cache_destroy(&engine.work_queue_cache);
    nxt_mp_destroy(mp);

    return ret;
}


nxt_int_t
nxt_router_schedule_test(nxt_thread_t *thr)
{
    nxt_thread_time_update(thr);

    if (nxt_router_schedule_msec_test(thr) != NXT_OK
        || nxt_router_schedule_delay_test(thr) != NXT_OK
        || nxt_router_schedule_uri_public_test(thr) != NXT_OK
        || nxt_router_schedule_request_test(thr) != NXT_OK
        || nxt_router_schedule_joint_test(thr) != NXT_OK
        || nxt_router_schedule_resolve_test(thr) != NXT_OK
        || nxt_router_schedule_devnull_test(thr) != NXT_OK)
    {
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "router schedule test passed");

    return NXT_OK;
}
