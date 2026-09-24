
/*
 * Copyright (C) FreeUnit contributors.
 */

#include <nxt_main.h>
#include <nxt_checked.h>
#include <nxt_span.h>
#include "nxt_tests.h"


static nxt_int_t
nxt_checked_arith_test(nxt_thread_t *thr)
{
    size_t   r;
    uint32_t u32;
    uint8_t  u8;

    /* Plain, non-overflowing arithmetic still has to work. */

    if (nxt_size_add(2, 3, &r) != 0 || r != 5) {
        nxt_log_alert(thr->log, "nxt_size_add(2, 3) failed");
        return NXT_ERROR;
    }

    if (nxt_size_mul(6, 7, &r) != 0 || r != 42) {
        nxt_log_alert(thr->log, "nxt_size_mul(6, 7) failed");
        return NXT_ERROR;
    }

    /* size_t addition overflow. */

    if (nxt_size_add(SIZE_MAX, 1, &r) == 0) {
        nxt_log_alert(thr->log, "nxt_size_add(SIZE_MAX, 1) did not overflow");
        return NXT_ERROR;
    }

    /* size_t multiplication overflow. */

    if (nxt_size_mul(SIZE_MAX, 2, &r) == 0) {
        nxt_log_alert(thr->log, "nxt_size_mul(SIZE_MAX, 2) did not overflow");
        return NXT_ERROR;
    }

    /* Narrowing that fits. */

    if (nxt_u32_from_size(1234, &u32) != 0 || u32 != 1234) {
        nxt_log_alert(thr->log, "nxt_u32_from_size(1234) failed");
        return NXT_ERROR;
    }

    if (nxt_u8_from_size(200, &u8) != 0 || u8 != 200) {
        nxt_log_alert(thr->log, "nxt_u8_from_size(200) failed");
        return NXT_ERROR;
    }

    /* Narrowing that overflows the destination type. */

    if (nxt_u32_from_size((size_t) UINT32_MAX + 1, &u32) == 0) {
        nxt_log_alert(thr->log, "nxt_u32_from_size() did not overflow");
        return NXT_ERROR;
    }

    if (nxt_u8_from_size(256, &u8) == 0) {
        nxt_log_alert(thr->log, "nxt_u8_from_size(256) did not overflow");
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_checked arithmetic test "
                  "passed");
    return NXT_OK;
}


static nxt_int_t
nxt_span_take_test(nxt_thread_t *thr)
{
    static const u_char  buf[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

    const u_char  *p;
    nxt_span_t    span;

    /* Two whole 4-byte records fit exactly. */

    nxt_span_init(&span, buf, buf + 8);

    if (nxt_span_take(&span, 4, &p) != 0 || p != buf) {
        nxt_log_alert(thr->log, "nxt_span_take() rejected a full record");
        return NXT_ERROR;
    }

    if (nxt_span_take(&span, 4, &p) != 0 || p != buf + 4) {
        nxt_log_alert(thr->log, "nxt_span_take() rejected the tail record");
        return NXT_ERROR;
    }

    if (nxt_span_len(&span) != 0) {
        nxt_log_alert(thr->log, "nxt_span_take() left bytes unconsumed");
        return NXT_ERROR;
    }

    /* A read past the end of an exhausted span must fail. */

    if (nxt_span_take(&span, 1, &p) == 0) {
        nxt_log_alert(thr->log, "nxt_span_take() read past the end");
        return NXT_ERROR;
    }

    /* A short buffer must fail outright. */

    nxt_span_init(&span, buf, buf + 3);

    if (nxt_span_take(&span, 4, &p) == 0) {
        nxt_log_alert(thr->log, "nxt_span_take() accepted a short buffer");
        return NXT_ERROR;
    }

    /*
     * A partial tail: two whole 4-byte records plus 1..3 extra bytes that
     * do not make up another whole record.  Each must be rejected once the
     * whole records have been consumed, without reading past "end".
     */

    for (size_t extra = 1; extra <= 3; extra++) {
        nxt_span_init(&span, buf, buf + 8 + extra);

        if (nxt_span_take(&span, 4, &p) != 0
            || nxt_span_take(&span, 4, &p) != 0)
        {
            nxt_log_alert(thr->log, "nxt_span_take() rejected a whole "
                          "record ahead of a partial tail");
            return NXT_ERROR;
        }

        if (nxt_span_len(&span) != extra) {
            nxt_log_alert(thr->log, "nxt_span_take() miscounted the "
                          "partial tail");
            return NXT_ERROR;
        }

        if (nxt_span_take(&span, 4, &p) == 0) {
            nxt_log_alert(thr->log, "nxt_span_take() accepted a partial "
                          "tail of %uz byte(s)", extra);
            return NXT_ERROR;
        }
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_span_take() test passed");
    return NXT_OK;
}


static nxt_int_t
nxt_span_copy_test(nxt_thread_t *thr)
{
    static const u_char  buf[6] = { 10, 11, 12, 13, 14, 15 };

    u_char      dst[4];
    nxt_span_t  span;

    nxt_span_init(&span, buf, buf + 6);

    if (nxt_span_copy(&span, dst, 4) != 0
        || memcmp(dst, buf, 4) != 0)
    {
        nxt_log_alert(thr->log, "nxt_span_copy() failed on a full record");
        return NXT_ERROR;
    }

    /* Only 2 bytes remain; asking for 4 must fail and not touch "dst". */

    dst[0] = 0xAA;

    if (nxt_span_copy(&span, dst, 4) == 0) {
        nxt_log_alert(thr->log, "nxt_span_copy() accepted a partial tail");
        return NXT_ERROR;
    }

    if (dst[0] != 0xAA) {
        nxt_log_alert(thr->log, "nxt_span_copy() wrote on a failed copy");
        return NXT_ERROR;
    }

    nxt_log_error(NXT_LOG_NOTICE, thr->log, "nxt_span_copy() test passed");
    return NXT_OK;
}


nxt_int_t
nxt_checked_test(nxt_thread_t *thr)
{
    if (nxt_checked_arith_test(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_span_take_test(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_span_copy_test(thr) != NXT_OK) {
        return NXT_ERROR;
    }

    return NXT_OK;
}
