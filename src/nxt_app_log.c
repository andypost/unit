
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>


static nxt_time_string_t  nxt_log_error_time_cache;
static u_char *nxt_log_error_time(u_char *buf, nxt_realtime_t *now,
    struct tm *tm, size_t size, const char *format);
static nxt_time_string_t  nxt_log_debug_time_cache;
static u_char *nxt_log_debug_time(u_char *buf, nxt_realtime_t *now,
    struct tm *tm, size_t size, const char *format);
static nxt_time_string_t  nxt_log_iso_time_cache;
static u_char *nxt_log_iso_time(u_char *buf, nxt_realtime_t *now,
    struct tm *tm, size_t size, const char *format);
static u_char *nxt_log_json_escape(u_char *dst, const u_char *src, size_t size);


void nxt_cdecl
nxt_log_time_handler(nxt_uint_t level, nxt_log_t *log, const char *fmt, ...)
{
    u_char             *p, *end;
#if 0
    u_char             *syslogmsg;
#endif
    va_list            args;
    nxt_thread_t       *thr;
    nxt_time_string_t  *time_cache;
    u_char             msg[NXT_MAX_ERROR_STR];

    thr = nxt_thread();

    end = msg + NXT_MAX_ERROR_STR;

    time_cache = (log->level != NXT_LOG_DEBUG) ? &nxt_log_error_time_cache:
                                                 &nxt_log_debug_time_cache;

    p = nxt_thread_time_string(thr, time_cache, msg);

#if 0
    syslogmsg = p;
#endif

#if 0
    nxt_fid_t    fid;
    const char   *id;
    nxt_fiber_t  *fib;

    fib = nxt_fiber_self(thr);

    if (fib != NULL) {
        id = "[%V] %PI#%PT#%PF ";
        fid = nxt_fiber_id(fib);

    } else {
        id = "[%V] %PI#%PT ";
        fid = 0;
    }

    p = nxt_sprintf(p, end, id, &nxt_log_levels[level], nxt_pid,
                    nxt_thread_tid(thr), fid);
#else
    p = nxt_sprintf(p, end, "[%V] %PI#%PT ", &nxt_log_levels[level], nxt_pid,
                    nxt_thread_tid(thr));
#endif

    if (log->ident != 0) {
        p = nxt_sprintf(p, end, "*%D ", log->ident);
    }

    va_start(args, fmt);
    p = nxt_vsprintf(p, end, fmt, args);
    va_end(args);

    if (level != NXT_LOG_DEBUG && log->ctx_handler != NULL) {
        p = log->ctx_handler(log->ctx, p, end);
    }

    if (p > end - nxt_length("\n")) {
        p = end - nxt_length("\n");
    }

    *p++ = '\n';

    (void) nxt_write_console(nxt_stderr, msg, p - msg);

#if 0
    if (level == NXT_LOG_ALERT) {
        *(p - nxt_length("\n")) = '\0';

        /*
         * The syslog LOG_ALERT level is enough, because
         * LOG_EMERG level broadcasts a message to all users.
         */
        nxt_write_syslog(LOG_ALERT, syslogmsg);
    }
#endif
}


static nxt_time_string_t  nxt_log_error_time_cache = {
    (nxt_atomic_uint_t) -1,
    nxt_log_error_time,
    "%4d/%02d/%02d %02d:%02d:%02d ",
    nxt_length("1970/09/28 12:00:00 "),
    NXT_THREAD_TIME_LOCAL,
    NXT_THREAD_TIME_SEC,
};


static u_char *
nxt_log_error_time(u_char *buf, nxt_realtime_t *now, struct tm *tm, size_t size,
    const char *format)
{
    return nxt_sprintf(buf, buf + size, format,
                       tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                       tm->tm_hour, tm->tm_min, tm->tm_sec);
}


static nxt_time_string_t  nxt_log_debug_time_cache = {
    (nxt_atomic_uint_t) -1,
    nxt_log_debug_time,
    "%4d/%02d/%02d %02d:%02d:%02d.%03d ",
    nxt_length("1970/09/28 12:00:00.000 "),
    NXT_THREAD_TIME_LOCAL,
    NXT_THREAD_TIME_MSEC,
};


static u_char *
nxt_log_debug_time(u_char *buf, nxt_realtime_t *now, struct tm *tm, size_t size,
    const char *format)
{
    return nxt_sprintf(buf, buf + size, format,
                       tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                       tm->tm_hour, tm->tm_min, tm->tm_sec,
                       now->nsec / 1000000);
}


static nxt_time_string_t  nxt_log_iso_time_cache = {
    (nxt_atomic_uint_t) -1,
    nxt_log_iso_time,
    "%4d-%02d-%02dT%02d:%02d:%02d.%03dZ",
    nxt_length("1970-09-28T12:00:00.000Z"),
    NXT_THREAD_TIME_GMT,
    NXT_THREAD_TIME_MSEC,
};


static u_char *
nxt_log_iso_time(u_char *buf, nxt_realtime_t *now, struct tm *tm, size_t size,
    const char *format)
{
    return nxt_sprintf(buf, buf + size, format,
                       tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                       tm->tm_hour, tm->tm_min, tm->tm_sec,
                       now->nsec / 1000000);
}


/*
 * Escape a UTF-8 string for embedding inside a JSON string literal.
 * Worst-case expansion is six bytes per input byte ("\u00XX") so callers
 * must size the destination buffer accordingly.  Logic mirrors
 * nxt_conf_json_escape() in src/nxt_conf.c so JSON output stays consistent
 * with the access-log JSON formatter.
 */
static u_char *
nxt_log_json_escape(u_char *dst, const u_char *src, size_t size)
{
    u_char  ch;

    while (size) {
        ch = *src++;

        if (ch > 0x1F) {

            if (ch == '\\' || ch == '"') {
                *dst++ = '\\';
            }

            *dst++ = ch;

        } else {
            *dst++ = '\\';

            switch (ch) {
            case '\n':
                *dst++ = 'n';
                break;

            case '\r':
                *dst++ = 'r';
                break;

            case '\t':
                *dst++ = 't';
                break;

            case '\b':
                *dst++ = 'b';
                break;

            case '\f':
                *dst++ = 'f';
                break;

            default:
                *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
                *dst++ = '0' + (ch >> 4);

                ch &= 0xF;

                *dst++ = (ch < 10) ? ('0' + ch) : ('A' + ch - 10);
            }
        }

        size--;
    }

    return dst;
}


void nxt_cdecl
nxt_log_json_handler(nxt_uint_t level, nxt_log_t *log, const char *fmt, ...)
{
    u_char        *p, *q, *qend;
    size_t        msg_len;
    va_list       args;
    nxt_thread_t  *thr;
    u_char        raw[NXT_MAX_ERROR_STR];
    /*
     * JSON line buffer.  Worst-case msg expansion is 6x; reserve room
     * for the framing fields ("ts", "level", "pid", "app", "request_id"
     * keys plus quotes/commas) on top.
     */
    u_char        out[NXT_MAX_ERROR_STR * 6 + 256];

    thr = nxt_thread();

    /* Format the human-readable message into raw[]. */
    p = raw;
    va_start(args, fmt);
    p = nxt_vsprintf(p, raw + NXT_MAX_ERROR_STR, fmt, args);
    va_end(args);

    if (level != NXT_LOG_DEBUG && log->ctx_handler != NULL) {
        p = log->ctx_handler(log->ctx, p, raw + NXT_MAX_ERROR_STR);
    }

    msg_len = p - raw;

    /* Build the JSON line into out[]. */
    q = out;
    qend = out + sizeof(out);

    q = nxt_cpymem(q, "{\"ts\":\"", 7);
    q = nxt_thread_time_string(thr, &nxt_log_iso_time_cache, q);

    q = nxt_cpymem(q, "\",\"level\":\"", 11);
    q = nxt_cpymem(q, nxt_log_levels[level].start, nxt_log_levels[level].length);

    q = nxt_sprintf(q, qend, "\",\"pid\":%PI,\"app\":\"unit\",\"msg\":\"",
                    nxt_pid);

    q = nxt_log_json_escape(q, raw, msg_len);

    *q++ = '"';

    if (log->ident != 0) {
        q = nxt_sprintf(q, qend, ",\"request_id\":%uD", log->ident);
    }

    *q++ = '}';
    *q++ = '\n';

    (void) nxt_write_console(nxt_stderr, out, q - out);
}
