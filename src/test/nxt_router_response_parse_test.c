/*
 * Copyright (C) FreeUnit contributors.
 */

/*
 * nxt_router_response_header_parse() (src/nxt_router.c) decodes the
 * nxt_unit_response_t an application hands back.  Every field's name and
 * value are serialized pointers (nxt_unit_sptr_t) that resolve to
 * base + offset with no bounds check of their own (nxt_unit_sptr_get(),
 * src/nxt_unit_sptr.h); the application is untrusted, and used to be able
 * to point one anywhere in the router's address space by choosing an
 * offset that lands outside the response buffer.  fuzzing/
 * nxt_router_app_response_fuzz.c found a SEGV in nxt_memcasecmp() (via
 * nxt_http_field_process() -> nxt_lvlhsh_bucket_find()) from exactly this:
 * a Content-Type field whose name sptr resolved off the end of its buffer.
 *
 * This pins the fix: an out-of-bounds name or value sptr, or an
 * out-of-bounds piggyback_content, must be refused (NXT_ERROR) rather than
 * dereferenced, while an in-bounds response still parses normally.
 */

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_http.h>
#include <nxt_unit_response.h>
#include "nxt_tests.h"


nxt_int_t nxt_router_test_response_header_parse(nxt_task_t *task,
    nxt_http_request_t *r, nxt_buf_t *b);


static nxt_buf_t *
nxt_router_response_parse_test_buf(nxt_mp_t *mp, const char *name,
    const char *value, nxt_bool_t name_oob, nxt_bool_t value_oob)
{
    size_t                data_size, buf_size;
    u_char                *data, *far;
    nxt_buf_t             *b;
    nxt_unit_field_t      *f;
    nxt_unit_response_t   *resp;

    data_size = nxt_strlen(name) + nxt_strlen(value);
    buf_size = sizeof(nxt_unit_response_t) + sizeof(nxt_unit_field_t)
               + data_size;

    resp = nxt_mp_zalloc(mp, buf_size);
    if (resp == NULL) {
        return NULL;
    }

    resp->fields_count = 1;
    resp->status = 200;

    f = &resp->fields[0];
    f->hash = 0;
    f->name_length = nxt_strlen(name);
    f->value_length = nxt_strlen(value);

    data = (u_char *) resp + sizeof(nxt_unit_response_t)
           + sizeof(nxt_unit_field_t);
    nxt_memcpy(data, name, f->name_length);
    nxt_memcpy(data + f->name_length, value, f->value_length);

    /*
     * A pointer that resolves outside "resp" but is still mapped memory
     * (a second, unrelated allocation well past it), so a build without
     * the bounds check reads garbage instead of necessarily faulting --
     * the point being tested is the check's return value, not a crash.
     */
    far = nxt_mp_zalloc(mp, 4096);
    if (far == NULL) {
        return NULL;
    }

    nxt_unit_sptr_set(&f->name, name_oob ? far : data);
    nxt_unit_sptr_set(&f->value,
                       value_oob ? far : data + f->name_length);

    b = nxt_mp_zalloc(mp, sizeof(nxt_buf_t));
    if (b == NULL) {
        return NULL;
    }

    b->mem.start = (u_char *) resp;
    b->mem.pos = (u_char *) resp;
    b->mem.free = (u_char *) resp + buf_size;
    b->mem.end = (u_char *) resp + buf_size;

    return b;
}


static nxt_int_t
nxt_router_response_parse_test_run(nxt_task_t *task, nxt_mp_t *mp,
    const char *case_name, nxt_bool_t name_oob, nxt_bool_t value_oob,
    nxt_int_t expect)
{
    nxt_int_t            ret;
    nxt_buf_t            *b;
    nxt_http_request_t   *r;

    r = nxt_mp_zalloc(mp, sizeof(nxt_http_request_t));
    if (r == NULL) {
        return NXT_ERROR;
    }

    r->mem_pool = mp;
    r->task = *task;

    b = nxt_router_response_parse_test_buf(mp, "X-Test", "value",
                                            name_oob, value_oob);
    if (b == NULL) {
        return NXT_ERROR;
    }

    ret = nxt_router_test_response_header_parse(task, r, b);

    if (ret != expect) {
        nxt_log_alert(task->log,
                      "response parse test \"%s\": got %d, expected %d",
                      case_name, (int) ret, (int) expect);
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_int_t
nxt_router_response_parse_test(nxt_thread_t *thr)
{
    nxt_mp_t    *mp;
    nxt_int_t   ret;
    nxt_task_t  *task;

    task = thr->task;
    task->thread = thr;

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    ret = nxt_router_response_parse_test_run(task, mp, "in-bounds field",
                                              0, 0, NXT_OK);

    if (ret == NXT_OK) {
        ret = nxt_router_response_parse_test_run(task, mp,
                                    "out-of-bounds name sptr", 1, 0,
                                    NXT_ERROR);
    }

    if (ret == NXT_OK) {
        ret = nxt_router_response_parse_test_run(task, mp,
                                    "out-of-bounds value sptr", 0, 1,
                                    NXT_ERROR);
    }

    nxt_mp_destroy(mp);

    if (ret == NXT_OK) {
        nxt_log(task, NXT_LOG_NOTICE, "router response parse test passed");
    }

    return ret;
}
