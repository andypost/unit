
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
 * to a [length]-byte range that is wholly inside the buffer.  Used to
 * vet every peer-supplied sptr (request fields on the libunit side,
 * response fields on the router side) before the receiver follows the
 * embedded offset.
 *
 * sptr->base aliases the address of the sptr itself (the union encodes
 * an offset relative to that location), so this also implicitly checks
 * that the sptr is inside the buffer.
 *
 * Underflow-safe: subtracts on the constant side throughout.
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
