
/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_CHECKED_H_INCLUDED_
#define _NXT_CHECKED_H_INCLUDED_


/*
 * Checked arithmetic helpers built on top of the compiler's
 * __builtin_*_overflow() intrinsics (GCC and Clang both support them).
 * Every helper returns 0 on success and stores the result via the "out"
 * pointer, or returns 1 when the operation would overflow, leaving "out"
 * untouched by convention (the builtin itself still writes a wrapped
 * result, but callers must not rely on it once a helper reports failure).
 *
 * These exist so that length and offset arithmetic taken from a peer (an
 * HTTP request, a language module, another process across shared memory)
 * can be validated in one line instead of being open-coded ad hoc at each
 * call site.
 */

nxt_inline int
nxt_size_add(size_t a, size_t b, size_t *out)
{
    return __builtin_add_overflow(a, b, out);
}


nxt_inline int
nxt_size_mul(size_t a, size_t b, size_t *out)
{
    return __builtin_mul_overflow(a, b, out);
}


/*
 * Narrowing helpers: convert a size_t to a smaller unsigned type, failing
 * when the value does not fit rather than silently truncating it.
 */

nxt_inline int
nxt_u32_from_size(size_t v, uint32_t *out)
{
    return __builtin_add_overflow(v, 0, out);
}


nxt_inline int
nxt_u8_from_size(size_t v, uint8_t *out)
{
    return __builtin_add_overflow(v, 0, out);
}


#endif /* _NXT_CHECKED_H_INCLUDED_ */
