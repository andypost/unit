
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_UNIT_SPTR_H_INCLUDED_
#define _NXT_UNIT_SPTR_H_INCLUDED_


#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "nxt_unit_typedefs.h"


/* Serialized pointer. */
union nxt_unit_sptr_u {
    uint8_t   base[1];
    uint32_t  offset;
};


static inline void
nxt_unit_sptr_set(nxt_unit_sptr_t *sptr, void *ptr)
{
    sptr->offset = (uint8_t *) ptr - sptr->base;
}


static inline void *
nxt_unit_sptr_get(nxt_unit_sptr_t *sptr)
{
    return sptr->base + sptr->offset;
}


/*
 * Validate that an sptr field within a peer-supplied buffer dereferences
 * to a [length]-byte range that is wholly inside the buffer.  Shared by
 * both directions of the trust boundary: libunit uses it at request-arrival
 * time to vet every sptr in nxt_unit_request_t before the application sees
 * it (src/nxt_unit.c), and the router uses it to vet an application's
 * nxt_unit_response_t before it trusts a name/value/piggyback sptr
 * (src/nxt_router.c).
 *
 * sptr->base aliases the address of the sptr itself (the union encodes an
 * offset relative to that location), so this also implicitly checks that
 * the sptr is inside the buffer.
 */
static inline int
nxt_unit_sptr_in_buf(nxt_unit_sptr_t *sptr, uint32_t length,
    void *buf_start, uint32_t buf_size)
{
    size_t  sptr_off, end_off;

    if ((uint8_t *) sptr < (uint8_t *) buf_start) {
        return 0;
    }

    sptr_off = (uint8_t *) sptr - (uint8_t *) buf_start;

    /*
     * The sptr struct itself must fit inside the buffer before we
     * dereference sptr->offset.  Reject a buffer too small to hold an
     * sptr first: buf_size is uint32_t and sizeof() is size_t, so
     * "buf_size - sizeof(nxt_unit_sptr_t)" is evaluated in size_t and
     * underflows to a huge value -- wrongly passing the bound check --
     * when buf_size is smaller than the struct.  The short-circuit keeps
     * the subtraction below from ever underflowing.
     */
    if (buf_size < sizeof(nxt_unit_sptr_t)
        || sptr_off > buf_size - sizeof(nxt_unit_sptr_t))
    {
        return 0;
    }

    if (sptr->offset > buf_size - sptr_off) {
        return 0;
    }

    end_off = sptr_off + sptr->offset;
    if (length > buf_size - end_off) {
        return 0;
    }

    return 1;
}


#endif /* _NXT_UNIT_SPTR_H_INCLUDED_ */
