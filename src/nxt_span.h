
/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_SPAN_H_INCLUDED_
#define _NXT_SPAN_H_INCLUDED_

#include <nxt_checked.h>


/*
 * nxt_span_t is a bounded read cursor over a buffer: "pos" is the next
 * byte to be consumed and "end" is one past the last valid byte.  It is
 * meant for parsers that walk untrusted, length-prefixed input (shared
 * memory port messages, libunit request buffers) where every advance has
 * to be checked against the remaining bytes instead of trusting a length
 * field taken from the peer.
 *
 * A short read (not enough bytes left) and a partial tail (the span ends
 * in the middle of what should be a whole record) are both reported as
 * failure; nxt_span_take() never returns a pointer that reads past "end".
 */

typedef struct {
    const u_char  *pos;
    const u_char  *end;
} nxt_span_t;


nxt_inline void
nxt_span_init(nxt_span_t *span, const u_char *start, const u_char *end)
{
    span->pos = start;
    span->end = end;
}


nxt_inline size_t
nxt_span_len(const nxt_span_t *span)
{
    return (size_t) (span->end - span->pos);
}


/*
 * Takes "size" bytes off the front of the span and returns a pointer to
 * them via "out".  Fails (returns 1) when fewer than "size" bytes remain,
 * which covers both a short buffer and a partial tail left after taking
 * whole records off it; "span" and "out" are left untouched on failure.
 */

nxt_inline int
nxt_span_take(nxt_span_t *span, size_t size, const u_char **out)
{
    size_t  avail;

    avail = nxt_span_len(span);

    if (nxt_slow_path(size > avail)) {
        return 1;
    }

    *out = span->pos;
    span->pos += size;

    return 0;
}


/*
 * Copies "size" bytes off the front of the span into "dst".  Same failure
 * semantics as nxt_span_take(): a short or partial tail leaves both the
 * span and "dst" untouched.
 */

nxt_inline int
nxt_span_copy(nxt_span_t *span, void *dst, size_t size)
{
    const u_char  *src;

    if (nxt_span_take(span, size, &src) != 0) {
        return 1;
    }

    nxt_memcpy(dst, src, size);

    return 0;
}


#endif /* _NXT_SPAN_H_INCLUDED_ */
