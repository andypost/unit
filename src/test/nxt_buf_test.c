/*
 * Copyright (C) FreeUnit
 */

#include <nxt_main.h>
#include "nxt_tests.h"


/*
 * nxt_buf_cpystr() with an empty nxt_str_t (start == NULL, length == 0)
 * must not call memcpy() with a NULL source: memcpy(dst, NULL, 0) is
 * undefined behaviour, even though it is harmless in practice.
 */

nxt_int_t
nxt_buf_test(nxt_thread_t *thr)
{
    nxt_mp_t         *mp;
    nxt_buf_t        *b;
    static nxt_str_t  empty = { 0, NULL };
    static nxt_str_t  name = nxt_string("name");

    mp = nxt_mp_create(1024, 128, 512, 16);
    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    b = nxt_buf_mem_alloc(mp, name.length + empty.length, 0);
    if (nxt_slow_path(b == NULL)) {
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_buf_cpystr(b, &name);
    nxt_buf_cpystr(b, &empty);

    if ((size_t) nxt_buf_mem_used_size(&b->mem) != name.length) {
        nxt_log_alert(thr->log, "nxt_buf_cpystr() empty string test failed");
        nxt_mp_destroy(mp);
        return NXT_ERROR;
    }

    nxt_mp_destroy(mp);

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_buf test passed");

    return NXT_OK;
}
