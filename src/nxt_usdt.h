/*
 * Copyright (C) F5, Inc.
 */

#ifndef _NXT_USDT_H_INCLUDED_
#define _NXT_USDT_H_INCLUDED_


/*
 * Static USDT tracepoints, provider "freeunit".
 *
 * NXT_USDT(name, ...) takes 0-6 cheap arguments and expands to a
 * DTRACE_PROBEn()/STAP_PROBEn() call (sys/sdt.h defines the former in terms
 * of the latter, so one macro covers dtrace, bpftrace and SystemTap) when
 * built with --usdt (NXT_HAVE_USDT).  Per the SDT contract each probe
 * compiles to a single `nop` (plus an ELF note) when a tracer is not
 * attached, and to nothing at all -- not even the nop -- when the build does
 * not define NXT_HAVE_USDT.  Call sites therefore pay zero cost in a
 * default build; see docs/observability/usdt.md for the verification
 * method (disassembly diff of a probed function).
 *
 * Usage, one line per call site so probe insertions stay a trivial diff
 * against unrelated changes to the same function:
 *
 *     NXT_USDT(port__send, pid, size);
 *
 * The probe name uses "__" the way the DTrace/SDT convention renders it as
 * "-" in the provider:probe form (e.g. "port__send" -> "port-send").
 */

#if (NXT_HAVE_USDT)

#include <sys/sdt.h>

#ifndef DTRACE_PROBE0
#define DTRACE_PROBE0(provider, probe)  DTRACE_PROBE(provider, probe)
#endif

#define nxt_usdt_nargs_(_0, _1, _2, _3, _4, _5, _6, N, ...)  N
#define nxt_usdt_nargs(...)                                                 \
    nxt_usdt_nargs_(_, ##__VA_ARGS__, 6, 5, 4, 3, 2, 1, 0)

#define nxt_usdt_cat_(a, b)  a##b
#define nxt_usdt_cat(a, b)   nxt_usdt_cat_(a, b)

#define NXT_USDT(name, ...)                                                 \
    nxt_usdt_cat(DTRACE_PROBE, nxt_usdt_nargs(__VA_ARGS__))                  \
        (freeunit, name, ##__VA_ARGS__)

#else

#define NXT_USDT(name, ...)

#endif


#endif /* _NXT_USDT_H_INCLUDED_ */
