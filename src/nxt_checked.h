
/*
 * Copyright (C) FreeUnit contributors.
 */

#ifndef _NXT_CHECKED_H_INCLUDED_
#define _NXT_CHECKED_H_INCLUDED_


/*
 * Checked arithmetic on the compiler's __builtin_*_overflow() (GCC and
 * Clang).  Each returns 0 and stores the result in "out", or returns 1 when
 * the operation would overflow; "out" then holds the wrapped result, which
 * callers must not use.
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


#endif /* _NXT_CHECKED_H_INCLUDED_ */
