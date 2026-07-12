
/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>


/*
 * Stage 1 io_uring event engine: a poll-mode readiness bridge.
 *
 * Unit's readiness model is left unchanged.  io_uring is used as a "better
 * epoll": a multishot IORING_OP_POLL_ADD per armed direction feeds the
 * existing read_ready/write_ready latches and per-direction work queues, and
 * all recv/send/accept syscalls stay in userspace exactly as with epoll.
 *
 * Multishot poll delivery is EDGE-LIKE: the kernel posts one CQE per wait
 * queue wakeup and does not re-report an fd that stays ready.  The engine is
 * therefore the analogue of the epoll *edge* engine and ships the matching
 * conn_io (nxt_io_uring_conn_io) whose recvbuf shim guarantees a pending EOF
 * is observed when data and FIN arrive in a single wakeup.  Listen sockets
 * are the exception: they are armed with a non-multishot POLL_ADD re-armed
 * per accept batch, because POLL_ADD re-checks readiness at submission and
 * so emulates the level-triggered re-fire the accept loop depends on.
 *
 * The load-bearing correctness invariant is the per-direction generation
 * counter encoded into each SQE's user_data (see nxt_io_uring_slot_t and the
 * user_data helpers below).  disable/delete/close bump the generation so any
 * completion produced for a stale arming -- including one that lands after the
 * fd is closed and its number reused -- is dropped before the event object is
 * touched.
 *
 * Runtime control:
 *   NXT_IO_URING=0                  operational kill switch: forces epoll for
 *                                   the whole process tree with no rebuild
 *                                   (insurance against a kernel/seccomp
 *                                   io_uring regression).  Honored by both the
 *                                   registration probe and create().
 *
 * Debug-only overrides:
 *   NXT_IO_URING_FORCE_TIER=none    fails both the registration probe and
 *                                   create() so the runtime uses epoll.
 *   NXT_IO_URING_FORCE_TIER=create  passes the probe but fails create(),
 *                                   exercising the in-create epoll fallback
 *                                   (nxt_event_engine_create/_change).
 *   NXT_IO_URING_FORCE_TIER=poll    caps at the Stage-1 poll-mode bridge:
 *                                   accept and recv stay on poll+syscall.
 *   NXT_IO_URING_FORCE_TIER=accept  caps at completion-mode accept (oneshot
 *                                   IORING_OP_ACCEPT); recv stays on the
 *                                   poll+recv path.
 *   NXT_IO_URING_FORCE_TIER=recv    caps at buf-ring multishot recv.
 *   NXT_IO_URING_FORCE_TIER=opt     (== recv == unset) the full feature tier.
 *
 * The cap never raises the tier above what the kernel actually supports; it
 * only lowers it, so a modern kernel can drive every degraded path.
 *
 *   NXT_IO_URING_SETUP_OPT          opt-in for the SINGLE_ISSUER/DEFER_TASKRUN
 *                                   setup flags (see nxt_io_uring_setup): OFF
 *                                   by default because the kernel binds the
 *                                   ring's submitter task at setup time and
 *                                   Unit creates router-worker engines off the
 *                                   polling thread.
 */


/*
 * Resolved feature tiers.  Each is cumulative and degrades independently to the
 * poll-mode Stage-1 path: RECV implies ACCEPT implies POLL.  The kernel
 * capability is probed once (never by uname) and then capped by the debug
 * NXT_IO_URING_FORCE_TIER override so every degraded path is exercisable on a
 * modern kernel.
 *
 *   NONE    io_uring unusable                       -> epoll fallback
 *   POLL    multishot POLL_ADD (5.13)               -> Stage-1 readiness bridge
 *   ACCEPT  + IORING_OP_ACCEPT (5.5, probed)        -> completion-mode accept
 *   RECV    + buf_ring (5.19) + multishot recv (6.0)-> completion-mode recv
 *   OPT     + SINGLE_ISSUER (6.0) + DEFER_TASKRUN (6.1) setup-flag optimisation
 *
 * OPT is not a distinct conn behaviour: it caps at the RECV feature set and
 * additionally enables the submission/completion setup flags.  FORCE_TIER=recv
 * therefore exercises the recv path *without* those flags, and =opt/unset with
 * them.
 */
#define NXT_IOU_TIER_NONE    0
#define NXT_IOU_TIER_POLL    1
#define NXT_IOU_TIER_ACCEPT  2
#define NXT_IOU_TIER_RECV    3
#define NXT_IOU_TIER_OPT     4


/*
 * RECV (buf-ring multishot recv) is DETECTED but the completion-mode recv conn
 * path is not yet wired: the operational tier is clamped to ACCEPT in setup().
 * The deferral is deliberate, not an oversight -- three constraints must be
 * resolved together before it is safe, and none is a small change:
 *
 *   1. Scope (design §2.4).  Multishot recv must cover ONLY data connections,
 *      never the UDS port sockets, whose messages carry SCM_RIGHTS fd-passing
 *      and credential cmsgs that a provided-buffer IORING_OP_RECV cannot carry
 *      (it has no ancillary-data channel).  But data conns and port sockets
 *      arm read through the SAME engine enable_read (nxt_port_socket.c calls
 *      nxt_fd_event_enable_read directly), and nxt_fd_event_t carries no type
 *      tag to tell them apart -- and design §1.7 forbids adding one (ABI churn
 *      across every engine).  So recv cannot be armed from enable_read; it must
 *      be driven from a completion-mode nxt_conn_io_t whose read handler forks
 *      the readiness-based nxt_conn_io_read state machine.
 *
 *   2. Buffer lifetime (design OQ2).  The zero-copy win requires handing a ring
 *      buffer to the request pipeline as c->read; but the router may hold
 *      c->read across a round-trip to the application process, pinning a ring
 *      buffer far longer than the ring can afford and starving it.  The safe
 *      answer is copy-out at the HTTP-parse boundary for data that outlives the
 *      completion -- which reintroduces a copy for bodies -- while a pure
 *      copy-out-immediately model adds a userspace copy on top of the kernel's,
 *      making it likely net-negative versus poll+recv (one kernel->c->read
 *      copy, zero syscalls saved beyond recv).  Getting the pin/return/close
 *      lifecycle wrong is a use-after-free or a leaked buffer -- i.e. a soak
 *      signal-11, exactly what the acceptance gate forbids.
 *
 *   3. Read discipline.  Unit block_read()s after every successful read and
 *      re-enable_read()s when it wants more; multishot recv instead delivers
 *      continuously, so a "blocked" conn would accumulate ring buffers (bounded
 *      by TCP rcvbuf, but multiplied across conns) until ENOBUFS, needing the
 *      degrade-to-poll+recv path to be robust.
 *
 * Because a subtly wrong recv path breaks the DEFAULT build's soak with
 * signal-11, wiring it is left to a focused follow-up with its own soak gate;
 * the ACCEPT tier already delivers the dominant measured win (herd removal).
 * The kernel RECV capability is still probed (nxt_io_uring_kernel_tier) so the
 * follow-up only has to flip the clamp and light up the tier >= RECV branch.
 */


/*
 * user_data layout returned verbatim in every CQE:
 *
 *   bit  0        DIR    0 = read, 1 = write
 *   bit  1        KIND   0 = fd poll (slot-indexed), 1 = internal sentinel
 *   bit  2        ACCEPT 1 = completion-mode accept op (res carries the fd)
 *   bits 3..34    GEN    32-bit per-direction generation (stale-CQE rejection)
 *   bits 35..63   IDX    29-bit slot index == fd number (up to ~536M fds)
 *
 * This unifies stage1's repacked layout with the completion-mode accept bit:
 * KIND (internal-vs-fd) and ACCEPT (poll-vs-accept-op, only when KIND==fd) are
 * independent 1-bit fields, GEN keeps stage1's full 32-bit width -- so the mask
 * is an identity on the uint32_t slot counter and a stale CQE can only alias a
 * live arming after 2^32 disable/enable cycles on one fd number -- and IDX
 * gives up one bit (29, still ~536M fds, beyond any attainable RLIMIT_NOFILE)
 * to make room.  The ACCEPT bit keeps an accept CQE self-describing: even a
 * stale one (after cancel/close and possible fd reuse) is recognised and its
 * already-accepted fd is closed rather than leaked, and the deferred cancel of
 * a condemned accept arming reissues ASYNC_CANCEL under this exact user_data.
 */

#define NXT_IOU_DIR_READ     0
#define NXT_IOU_DIR_WRITE    1

#define NXT_IOU_KIND_FD      0
#define NXT_IOU_KIND_INT     1

#define NXT_IOU_GEN_MASK     0xFFFFFFFF

/*
 * The generation field is the same width (32 bits) as the uint32_t slot
 * counters, so the mask is an identity on the slot side and the user_data
 * side stays in lockstep with the counter across its full period; the macro
 * is kept for documentation and to keep the packing self-describing.
 */
#define nxt_iou_gen(g)        ((uint32_t) ((g) & NXT_IOU_GEN_MASK))

#define nxt_iou_ud(idx, gen, dir, acc)                                        \
    ( ((uint64_t) (idx) << 35)                                                \
      | (((uint64_t) ((gen) & NXT_IOU_GEN_MASK)) << 3)                        \
      | ((uint64_t) ((acc) != 0) << 2)                                        \
      | ((uint64_t) (NXT_IOU_KIND_FD) << 1)                                   \
      | (uint64_t) (dir) )

#define nxt_iou_ud_dir(ud)    ((uint32_t) ((ud) & 0x1))
#define nxt_iou_ud_kind(ud)   ((uint32_t) (((ud) >> 1) & 0x1))
#define nxt_iou_ud_accept(ud) ((uint32_t) (((ud) >> 2) & 0x1))
#define nxt_iou_ud_gen(ud)    ((uint32_t) (((ud) >> 3) & NXT_IOU_GEN_MASK))
#define nxt_iou_ud_idx(ud)    ((uint32_t) ((ud) >> 35))

/*
 * The slot stores generations as free-running uint16_t counters while
 * user_data carries only their low 13 bits, so every stale-CQE comparison
 * must reduce the slot value with this macro; comparing against the raw
 * counter would reject ALL completions once a generation passes 8191,
 * leaving the fd permanently dead.  Generation arithmetic is modulo 2^13:
 * a stale CQE is mis-accepted only if exactly 8192 disable/enable cycles
 * complete while that CQE is still in flight, which cannot happen -- CQEs
 * in flight are bounded by one CQ-drain window and each cycle costs at
 * least one submitted SQE.
 */
#define nxt_iou_gen(g)        ((uint16_t) ((g) & NXT_IOU_GEN_MASK))

/* Internal sentinel user_data values (KIND == INTERNAL). */
#define NXT_IOU_UD_POST      ((uint64_t) 0x2)   /* eventfd post channel     */
#define NXT_IOU_UD_REMOVE    ((uint64_t) 0x6)   /* POLL_REMOVE/cancel CQEs  */

/* Poll masks per direction (mirror epoll EPOLLIN/EPOLLOUT + error bits). */
#define NXT_IOU_READ_MASK    (POLLIN | POLLRDHUP | POLLERR | POLLHUP)
#define NXT_IOU_WRITE_MASK   (POLLOUT | POLLERR | POLLHUP)

/*
 * Upper bound on the poll wait while the eventfd doorbell's re-arm is owed
 * (post_rearm_pending): a cross-thread post writes the eventfd but produces no
 * CQE against the terminated poll, so the loop must wake on its own to retry
 * the re-arm and drain locked_work_queue.  Bounds the worst-case post latency
 * in that transient degraded state; off the hot path once the re-arm succeeds.
 */
#define NXT_IOU_POST_REARM_CAP_MSEC  100


static nxt_int_t nxt_io_uring_create(nxt_event_engine_t *engine,
    nxt_uint_t mchanges, nxt_uint_t mevents);
static nxt_int_t nxt_io_uring_setup(nxt_event_engine_t *engine,
    nxt_uint_t mchanges, nxt_uint_t mevents);
static nxt_bool_t nxt_io_uring_multishot_supported(struct io_uring *ring);
static nxt_int_t nxt_io_uring_force_cap(void);
static nxt_int_t nxt_io_uring_kernel_tier(void);
static nxt_bool_t nxt_io_uring_accept_supported(struct io_uring *ring);
static nxt_bool_t nxt_io_uring_recv_supported(struct io_uring *ring);
static void nxt_io_uring_test_accept4(nxt_event_engine_t *engine,
    nxt_conn_io_t *io);
#if (NXT_HAVE_SIGNALFD)
static nxt_int_t nxt_io_uring_add_signal(nxt_event_engine_t *engine);
static void nxt_io_uring_signalfd_handler(nxt_task_t *task, void *obj,
    void *data);
#endif
static void nxt_io_uring_free(nxt_event_engine_t *engine);
static nxt_io_uring_slot_t *nxt_io_uring_slot(nxt_event_engine_t *engine,
    nxt_fd_t fd);
static struct io_uring_sqe *nxt_io_uring_get_sqe(nxt_event_engine_t *engine);
static nxt_bool_t nxt_io_uring_arm(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev, nxt_uint_t dir, nxt_bool_t multishot);
static void nxt_io_uring_error_handler(nxt_task_t *task, void *obj, void *data);
static void nxt_io_uring_arm_failed(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_remove(nxt_event_engine_t *engine, nxt_fd_event_t *ev,
    nxt_uint_t dir);
static nxt_bool_t nxt_io_uring_submit_remove(nxt_event_engine_t *engine,
    nxt_fd_t fd, uint32_t gen, nxt_uint_t dir, nxt_bool_t accept);
static void nxt_io_uring_retry_pending_removes(nxt_event_engine_t *engine);
static void nxt_io_uring_arm_pend(nxt_event_engine_t *engine,
    nxt_io_uring_slot_t *slot, nxt_uint_t dir);
static void nxt_io_uring_retry_pending_arms(nxt_event_engine_t *engine);
static void nxt_io_uring_slot_reconcile(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);

static void nxt_io_uring_enable(nxt_event_engine_t *engine, nxt_fd_event_t *ev);
static void nxt_io_uring_disable(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_delete(nxt_event_engine_t *engine, nxt_fd_event_t *ev);
static nxt_bool_t nxt_io_uring_close(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_enable_read(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_enable_write(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_disable_read(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_disable_write(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_block_read(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_block_write(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_oneshot_read(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_oneshot_write(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
static void nxt_io_uring_enable_accept(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);
#if (NXT_HAVE_SIGNALFD)
static nxt_int_t nxt_io_uring_enable_post(nxt_event_engine_t *engine,
    nxt_work_handler_t handler);
static void nxt_io_uring_signal(nxt_event_engine_t *engine, nxt_uint_t signo);
#endif
static void nxt_io_uring_poll(nxt_event_engine_t *engine, nxt_msec_t timeout);
static void nxt_io_uring_handle_cqe(nxt_event_engine_t *engine,
    struct io_uring_cqe *cqe);
static void nxt_io_uring_error(nxt_event_engine_t *engine,
    nxt_io_uring_slot_t *slot, nxt_fd_event_t *ev);
static void nxt_io_uring_accept_cqe(nxt_event_engine_t *engine,
    nxt_io_uring_slot_t *slot, uint16_t gen, struct io_uring_cqe *cqe);
static void nxt_io_uring_internal_cqe(nxt_event_engine_t *engine,
    struct io_uring_cqe *cqe);

#if (NXT_HAVE_ACCEPT4)
static void nxt_io_uring_conn_io_accept4(nxt_task_t *task, void *obj,
    void *data);
#endif
static ssize_t nxt_io_uring_conn_io_recvbuf(nxt_conn_t *c, nxt_buf_t *b);


/*
 * A copy of nxt_unix_conn_io with recvbuf overridden by the edge-mode EOF
 * shim (see nxt_io_uring_conn_io_recvbuf); mirrors nxt_epoll_edge_conn_io.
 * Not const: create() may switch .accept to the accept4 variant.
 */

static nxt_conn_io_t  nxt_io_uring_conn_io = {
    .connect = nxt_conn_io_connect,
    .accept = nxt_conn_io_accept,

    .read = nxt_conn_io_read,
    .recvbuf = nxt_io_uring_conn_io_recvbuf,
    .recv = nxt_conn_io_recv,

    .write = nxt_conn_io_write,
    .sendbuf = nxt_conn_io_sendbuf,

#if (NXT_HAVE_LINUX_SENDFILE)
    .old_sendbuf = nxt_linux_event_conn_io_sendfile,
#else
    .old_sendbuf = nxt_event_conn_io_sendbuf,
#endif

    .writev = nxt_event_conn_io_writev,
    .send = nxt_event_conn_io_send,
};


const nxt_event_interface_t  nxt_io_uring_engine = {
    "io_uring",
    nxt_io_uring_create,
    nxt_io_uring_free,
    nxt_io_uring_enable,
    nxt_io_uring_disable,
    nxt_io_uring_delete,
    nxt_io_uring_close,
    nxt_io_uring_enable_read,
    nxt_io_uring_enable_write,
    nxt_io_uring_disable_read,
    nxt_io_uring_disable_write,
    nxt_io_uring_block_read,
    nxt_io_uring_block_write,
    nxt_io_uring_oneshot_read,
    nxt_io_uring_oneshot_write,
    nxt_io_uring_enable_accept,
    NULL,
    NULL,
    /*
     * enable_post + signal are the eventfd doorbell.  Install them only when
     * signalfd delivers real signals: nxt_event_engine_signal() prefers a
     * non-NULL .signal, and the eventfd doorbell cannot carry a signo, so
     * without signalfd the sigwait() thread's process-control signals would be
     * dropped.  Leaving both NULL routes post AND signals through the generic
     * signal pipe, exactly as the epoll engine does when its eventfd -- which
     * on Linux always accompanies signalfd -- is unavailable.
     */
#if (NXT_HAVE_SIGNALFD)
    nxt_io_uring_enable_post,
    nxt_io_uring_signal,
#else
    NULL,
    NULL,
#endif
    nxt_io_uring_poll,

    &nxt_io_uring_conn_io,

    NXT_NO_FILE_EVENTS,

#if (NXT_HAVE_SIGNALFD)
    NXT_SIGNAL_EVENTS,
#else
    NXT_NO_SIGNAL_EVENTS,
#endif
};


nxt_inline uint32_t
nxt_io_uring_pow2(uint32_t n)
{
    if (nxt_slow_path(n <= 1)) {
        return 1;
    }

    if (nxt_slow_path(n > 0x80000000)) {
        /* Saturate: no caller's size gets here, but never overflow to 0. */
        return 0x80000000;
    }

    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;

    return n + 1;
}


static nxt_int_t
nxt_io_uring_create(nxt_event_engine_t *engine, nxt_uint_t mchanges,
    nxt_uint_t mevents)
{
    engine->u.io_uring.tier = NXT_IOU_TIER_NONE;
    engine->u.io_uring.eventfd.fd = -1;
#if (NXT_HAVE_SIGNALFD)
    engine->u.io_uring.signalfd.fd = -1;
#endif

    if (nxt_io_uring_setup(engine, mchanges, mevents) != NXT_OK) {
        nxt_io_uring_free(engine);
        return NXT_ERROR;
    }

    if (engine->signals != NULL) {

#if (NXT_HAVE_SIGNALFD)
        if (nxt_io_uring_add_signal(engine) != NXT_OK) {
            nxt_io_uring_free(engine);
            return NXT_ERROR;
        }
#endif

        nxt_io_uring_test_accept4(engine, &nxt_io_uring_conn_io);
    }

    return NXT_OK;
}


static nxt_int_t
nxt_io_uring_setup(nxt_event_engine_t *engine, nxt_uint_t mchanges,
    nxt_uint_t mevents)
{
    int                     ret;
    char                    *force;
    uint32_t                nslots, flags;
    nxt_int_t               cap, tier;
    nxt_bool_t              want_opt;
    struct io_uring_params  params;
    nxt_io_uring_engine_t   *iou;

    iou = &engine->u.io_uring;

    /*
     * Operational kill switch: NXT_IO_URING=0 forces the epoll fallback with
     * no rebuild.  Checked here as well as in the registration probe so an
     * io_uring engine selected explicitly (bypassing the probe-gated default)
     * still degrades cleanly.
     */
    force = getenv("NXT_IO_URING");

    if (force != NULL && nxt_strcmp(force, "0") == 0) {
        nxt_log(&engine->task, NXT_LOG_INFO,
                "io_uring disabled by NXT_IO_URING=0");
        return NXT_ERROR;
    }

    /*
     * Debug override: NXT_IO_URING_FORCE_TIER caps the resolved tier.  =none
     * (also failing the registration probe) and =create force this create() to
     * fail cleanly so the caller degrades to epoll.  Any other value is a cap
     * that never raises the tier above kernel support.
     */
    cap = nxt_io_uring_force_cap();

    if (cap == NXT_IOU_TIER_NONE) {
        nxt_log(&engine->task, NXT_LOG_INFO,
                "io_uring disabled by NXT_IO_URING_FORCE_TIER");
        return NXT_ERROR;
    }

    /*
     * The SQ ring only needs to hold one loop's worth of arm/remove churn
     * (multishot registrations then live in the kernel, not the SQ).  The CQ
     * is sized generously so a readiness storm across all armed pollers fits;
     * IORING_FEAT_NODROP backstops any overflow.
     */
    iou->sq_entries = nxt_io_uring_pow2(nxt_max(mchanges, 256));
    iou->cq_entries = nxt_io_uring_pow2(nxt_max(8 * (uint32_t) mevents, 4096));

    /*
     * SINGLE_ISSUER (lock elision when a single task submits) and DEFER_TASKRUN
     * (completion task-work deferred to io_uring_enter time, which poll() calls
     * every loop) cut submission-lock and completion-latency jitter.  They are
     * kernel >= 6.0/6.1; on an older kernel queue_init returns -EINVAL, so drop
     * them and retry (DEFER_TASKRUN requires SINGLE_ISSUER, so both are added
     * and dropped together).
     *
     * OFF BY DEFAULT.  The kernel binds the ring's permitted submitter task at
     * io_uring_setup() time (verified on 7.0: -EEXIST on any submit from
     * another task, even for SINGLE_ISSUER alone).  Unit creates each router
     * worker's engine on the router-MAIN thread (nxt_event_engine_create in
     * nxt_router_engines_create) and only later runs poll() on the spawned
     * worker thread (nxt_event_engine_start) -- a different task.  Enabling the
     * flags therefore busy-loops every worker's poll() on -EEXIST.  The
     * design's "one thread owns each engine" (§2.5) is true for the poll loop
     * but NOT for engine creation, which the flags key off.  So the flags are
     * gated behind an explicit opt-in (NXT_IO_URING_SETUP_OPT) for deployments
     * where every engine is polled on its creating thread (the main/controller/
     * application processes, not router workers).
     */
    want_opt = (getenv("NXT_IO_URING_SETUP_OPT") != NULL);

    flags = IORING_SETUP_CQSIZE;

    if (want_opt) {
        flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    }

    for ( ;; ) {
        nxt_memzero(&params, sizeof(struct io_uring_params));
        params.flags = flags;
        params.cq_entries = iou->cq_entries;

        ret = io_uring_queue_init_params(iou->sq_entries, &iou->ring, &params);

        if (ret == 0) {
            iou->ring_inited = 1;
            break;
        }

        if (ret == -EINVAL
            && (flags & IORING_SETUP_SINGLE_ISSUER) != 0)
        {
            /* Optimisation flags unsupported on this kernel: drop and retry. */
            flags &= ~(IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN);
            continue;
        }

        /*
         * EPERM (seccomp / kernel.io_uring_disabled), ENOSYS (too old / built
         * out), EMFILE/ENFILE (fd exhaustion) and ENOMEM (memlock pressure)
         * are all treated as "unsupported" -> clean epoll fallback, never a
         * process-fatal error.
         */
        nxt_log(&engine->task, NXT_LOG_INFO,
                "io_uring_queue_init_params() failed %E", -ret);
        return NXT_ERROR;
    }

    iou->ring_inited = 1;

    iou->opt = (flags & IORING_SETUP_SINGLE_ISSUER) != 0;

    nxt_debug(&engine->task,
              "io_uring_queue_init(): sq:%uD cq:%uD features:%uxD opt:%d",
              iou->sq_entries, params.cq_entries, params.features,
              (int) iou->opt);

    /*
     * Resolve the kernel capability from a cached throwaway-ring probe rather
     * than probing this engine's ring: with IORING_SETUP_SINGLE_ISSUER the
     * kernel binds the ring to the FIRST submitting task, and Unit creates the
     * router-worker engines on the router-main thread while poll() runs on the
     * spawned worker thread (nxt_router.c: nxt_event_engine_create at engine
     * setup vs nxt_event_engine_start on the worker).  Submitting a probe SQE
     * here would bind the ring to the wrong thread and every worker submit
     * would then fail with -EEXIST.  So create() performs NO submission; the
     * first submit happens in poll() on the owning thread.  Multishot POLL_ADD
     * is the floor; without it there is no point.
     */
    tier = nxt_io_uring_kernel_tier();

    if (tier < NXT_IOU_TIER_POLL) {
        nxt_log(&engine->task, NXT_LOG_INFO,
                "io_uring multishot poll is not supported");
        return NXT_ERROR;
    }

    if (tier > cap) {
        tier = cap;
    }

    /*
     * Clamp the operational tier to ACCEPT: the completion-mode recv conn path
     * is not yet wired (see the RECV deferral note near the tier defines), so
     * the reported tier must reflect actual behaviour.  Kernel RECV support is
     * still detected above for the follow-up that lifts this clamp.
     */
    if (tier > NXT_IOU_TIER_ACCEPT) {
        tier = NXT_IOU_TIER_ACCEPT;
    }

    iou->tier = tier;

    nxt_log(&engine->task, NXT_LOG_INFO,
            "io_uring tier %d (opt:%d)", (int) iou->tier, (int) iou->opt);

    /* Slots are indexed by fd number; start small and grow on demand. */
    nslots = nxt_max((uint32_t) mevents * 2, 128);

    iou->slots = nxt_zalloc(nslots * sizeof(nxt_io_uring_slot_t));
    if (iou->slots == NULL) {
        return NXT_ERROR;
    }

    iou->nslots = nslots;

    return NXT_OK;
}


/*
 * Parse NXT_IO_URING_FORCE_TIER into a tier cap.  Returns NXT_IOU_TIER_NONE for
 * "none"/"create" (create() must fail), otherwise the capped feature tier.
 * Unset, "opt", or an unrecognised value means the full feature tier (RECV).
 * The SINGLE_ISSUER/DEFER_TASKRUN setup flags are governed separately (see
 * nxt_io_uring_setup); "opt" and "recv" therefore resolve to the same feature
 * set, the design's OPT tier being RECV plus those flags.
 */

static nxt_int_t
nxt_io_uring_force_cap(void)
{
    char  *force;

    force = getenv("NXT_IO_URING_FORCE_TIER");

    if (force == NULL) {
        return NXT_IOU_TIER_RECV;
    }

    if (nxt_strcmp(force, "none") == 0 || nxt_strcmp(force, "create") == 0) {
        return NXT_IOU_TIER_NONE;
    }

    if (nxt_strcmp(force, "poll") == 0) {
        return NXT_IOU_TIER_POLL;
    }

    if (nxt_strcmp(force, "accept") == 0) {
        return NXT_IOU_TIER_ACCEPT;
    }

    /* "recv", "opt", unset, or unrecognised: full feature tier. */
    return NXT_IOU_TIER_RECV;
}


/*
 * Kernel feature capability, probed once and cached process-wide (every engine
 * in a process shares the same kernel).  Never parses uname: IORING_OP_ACCEPT
 * and multishot recv are probed functionally on a throwaway ring.
 */

static nxt_int_t
nxt_io_uring_kernel_tier(void)
{
    int                     ret;
    struct io_uring         ring;
    static nxt_int_t        cached = -1;

    if (cached >= 0) {
        return cached;
    }

    cached = NXT_IOU_TIER_NONE;

    ret = io_uring_queue_init(64, &ring, 0);
    if (ret < 0) {
        return cached;
    }

    if (nxt_io_uring_multishot_supported(&ring)) {
        cached = NXT_IOU_TIER_POLL;

        if (nxt_io_uring_accept_supported(&ring)) {
            cached = NXT_IOU_TIER_ACCEPT;

            if (nxt_io_uring_recv_supported(&ring)) {
                cached = NXT_IOU_TIER_RECV;
            }
        }
    }

    io_uring_queue_exit(&ring);

    return cached;
}


/*
 * Functional probe for IORING_OP_ACCEPT (the accept tier uses ONESHOT accepts
 * re-armed per completion; see nxt_io_uring_enable_accept).  Arm one on a
 * throwaway listening socket: if the kernel rejects the opcode it posts an
 * immediate -EINVAL completion; if it is supported the accept simply waits
 * (no connection), so a short timeout with no CQE means "supported".
 */

static nxt_bool_t
nxt_io_uring_accept_supported(struct io_uring *ring)
{
    int                       lfd, ret;
    nxt_bool_t                ok;
    struct io_uring_cqe       *cqe;
    struct sockaddr_un        sa;
    struct __kernel_timespec  ts;
    struct io_uring_sqe       *sqe;

    lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd == -1) {
        return 0;
    }

    /* Abstract-namespace name: no filesystem entry to clean up. */
    nxt_memzero(&sa, sizeof(sa));
    sa.sun_family = AF_UNIX;
    sa.sun_path[0] = '\0';
    (void) snprintf(&sa.sun_path[1], sizeof(sa.sun_path) - 1,
                    "nxt_iou_probe_%d", (int) getpid());

    if (bind(lfd, (struct sockaddr *) &sa, sizeof(sa)) != 0
        || listen(lfd, 1) != 0)
    {
        close(lfd);
        return 0;
    }

    ok = 0;

    sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        close(lfd);
        return 0;
    }

    io_uring_prep_accept(sqe, lfd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, NXT_IOU_UD_REMOVE);

    if (io_uring_submit(ring) < 0) {
        close(lfd);
        return 0;
    }

    ts.tv_sec = 0;
    ts.tv_nsec = 20 * 1000000;

    ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);

    if (ret == -ETIME) {
        /* No completion: the accept is armed and waiting. */
        ok = 1;

    } else if (ret == 0 && cqe != NULL) {
        ok = (cqe->res != -EINVAL);
        io_uring_cqe_seen(ring, cqe);
    }

    /* Cancel the accept before the listening fd goes away. */
    sqe = io_uring_get_sqe(ring);
    if (sqe != NULL) {
        io_uring_prep_cancel64(sqe, NXT_IOU_UD_REMOVE, 0);
        io_uring_sqe_set_data64(sqe, NXT_IOU_UD_REMOVE);
        (void) io_uring_submit(ring);
    }

    close(lfd);

    /* Drain any leftover completions (the cancel and its target). */
    while (io_uring_peek_cqe(ring, &cqe) == 0 && cqe != NULL) {
        io_uring_cqe_seen(ring, cqe);
    }

    return ok;
}


/*
 * Functional probe for buf_ring (5.19) + IORING_RECV_MULTISHOT (6.0): register
 * a tiny buffer ring, arm a multishot recv with buffer-select on a socketpair,
 * push one byte, and confirm the completion is delivered (not -EINVAL).
 */

#define NXT_IOU_PROBE_BGID  0xFFFF

static nxt_bool_t
nxt_io_uring_recv_supported(struct io_uring *ring)
{
    int                       sv[2], err, ret;
    char                      buf[64];
    nxt_bool_t                ok;
    struct io_uring_cqe       *cqe;
    struct io_uring_sqe       *sqe;
    struct io_uring_buf_ring  *br;
    struct __kernel_timespec  ts;

    br = io_uring_setup_buf_ring(ring, 4, NXT_IOU_PROBE_BGID, 0, &err);
    if (br == NULL) {
        return 0;
    }

    ok = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        goto free_ring;
    }

    io_uring_buf_ring_add(br, buf, sizeof(buf), 0, io_uring_buf_ring_mask(4), 0);
    io_uring_buf_ring_advance(br, 1);

    sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        goto close_sv;
    }

    io_uring_prep_recv_multishot(sqe, sv[0], NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = NXT_IOU_PROBE_BGID;
    io_uring_sqe_set_data64(sqe, NXT_IOU_UD_REMOVE);

    if (io_uring_submit(ring) < 0) {
        goto close_sv;
    }

    buf[0] = 'x';
    if (write(sv[1], buf, 1) != 1) {
        goto cancel;
    }

    ts.tv_sec = 0;
    ts.tv_nsec = 20 * 1000000;

    ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);

    if (ret == 0 && cqe != NULL) {
        ok = (cqe->res >= 0);
        io_uring_cqe_seen(ring, cqe);
    }

cancel:

    sqe = io_uring_get_sqe(ring);
    if (sqe != NULL) {
        io_uring_prep_cancel64(sqe, NXT_IOU_UD_REMOVE, 0);
        io_uring_sqe_set_data64(sqe, NXT_IOU_UD_REMOVE);
        (void) io_uring_submit(ring);
    }

close_sv:

    close(sv[0]);
    close(sv[1]);

    while (io_uring_peek_cqe(ring, &cqe) == 0 && cqe != NULL) {
        io_uring_cqe_seen(ring, cqe);
    }

free_ring:

    io_uring_free_buf_ring(ring, br, 4, NXT_IOU_PROBE_BGID);

    return ok;
}


/*
 * Opcode support for POLL_ADD does not by itself prove IORING_POLL_ADD_MULTI
 * works (it is a flag bit, not an opcode), so verify it functionally: arm a
 * multishot poll on a throwaway eventfd, poke it, and confirm the CQE carries
 * IORING_CQE_F_MORE.
 */

static nxt_bool_t
nxt_io_uring_multishot_supported(struct io_uring *ring)
{
    int                  fd, ret;
    uint64_t             value;
    nxt_bool_t           ok;
    struct io_uring_sqe  *sqe;
    struct io_uring_cqe  *cqe;

    fd = eventfd(0, EFD_NONBLOCK);
    if (fd == -1) {
        return 0;
    }

    ok = 0;

    sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        goto done;
    }

    io_uring_prep_poll_multishot(sqe, fd, POLLIN);
    io_uring_sqe_set_data64(sqe, NXT_IOU_UD_REMOVE);

    if (io_uring_submit(ring) < 0) {
        goto done;
    }

    value = 1;
    if (write(fd, &value, sizeof(value)) != (ssize_t) sizeof(value)) {
        goto done;
    }

    ret = io_uring_wait_cqe_timeout(ring, &cqe, NULL);
    if (ret < 0 || cqe == NULL) {
        goto done;
    }

    ok = (cqe->res >= 0) && ((cqe->flags & IORING_CQE_F_MORE) != 0);

    io_uring_cqe_seen(ring, cqe);

    /* Cancel the throwaway multishot poll before the fd goes away. */
    sqe = io_uring_get_sqe(ring);
    if (sqe != NULL) {
        io_uring_prep_poll_remove(sqe, NXT_IOU_UD_REMOVE);
        io_uring_sqe_set_data64(sqe, NXT_IOU_UD_REMOVE);
        (void) io_uring_submit(ring);
    }

done:

    close(fd);

    return ok;
}


/*
 * Registration-time functional probe: stand up a throwaway ring and verify
 * multishot poll actually works.  All "unsupported" conditions -- EPERM
 * (seccomp / kernel.io_uring_disabled), ENOSYS (too old / built out),
 * EMFILE/ENFILE (fd exhaustion) and ENOMEM (memlock) -- surface as
 * io_uring_queue_init() returning a negative errno and map to NXT_ERROR, so
 * the caller keeps epoll as the default.  NXT_IO_URING_FORCE_TIER=none forces
 * failure for testing the fallback path.
 */

nxt_int_t
nxt_io_uring_probe(void)
{
    int              ret;
    char             *force;
    nxt_bool_t       ok;
    struct io_uring  ring;

    /*
     * NXT_IO_URING=0 is the operational kill switch: it forces epoll for the
     * whole process tree (the engine is never registered) with no rebuild --
     * cheap insurance against a kernel/seccomp io_uring regression.
     * NXT_IO_URING_FORCE_TIER=none is the debug-only equivalent.
     */
    force = getenv("NXT_IO_URING");

    if (force != NULL && nxt_strcmp(force, "0") == 0) {
        return NXT_ERROR;
    }

    force = getenv("NXT_IO_URING_FORCE_TIER");

    if (force != NULL && nxt_strcmp(force, "none") == 0) {
        return NXT_ERROR;
    }

    ret = io_uring_queue_init(8, &ring, 0);
    if (ret < 0) {
        return NXT_ERROR;
    }

    ok = nxt_io_uring_multishot_supported(&ring);

    io_uring_queue_exit(&ring);

    return ok ? NXT_OK : NXT_ERROR;
}


static void
nxt_io_uring_test_accept4(nxt_event_engine_t *engine, nxt_conn_io_t *io)
{
    static nxt_work_handler_t  handler;

    if (handler == NULL) {

        handler = io->accept;

#if (NXT_HAVE_ACCEPT4)

        (void) accept4(-1, NULL, NULL, SOCK_NONBLOCK);

        if (nxt_errno != NXT_ENOSYS) {
            handler = nxt_io_uring_conn_io_accept4;

        } else {
            nxt_log(&engine->task, NXT_LOG_INFO, "accept4() failed %E",
                    NXT_ENOSYS);
        }

#endif
    }

    io->accept = handler;
}


#if (NXT_HAVE_SIGNALFD)

/*
 * io_uring has no signalfd analogue of its own, so -- exactly like epoll --
 * real Unix signals are delivered through a signalfd registered as an ordinary
 * read event (a multishot poll).  Providing this makes signal_support true, so
 * nxt_event_engine_create() never spawns the sigwait() thread whose
 * signo-carrying signal() the ring's eventfd doorbell cannot reproduce.
 */

static nxt_int_t
nxt_io_uring_add_signal(nxt_event_engine_t *engine)
{
    int                    fd;
    nxt_io_uring_engine_t  *iou;

    iou = &engine->u.io_uring;

    if (sigprocmask(SIG_BLOCK, &engine->signals->sigmask, NULL) != 0) {
        nxt_alert(&engine->task, "sigprocmask(SIG_BLOCK) failed %E", nxt_errno);
        return NXT_ERROR;
    }

    fd = signalfd(-1, &engine->signals->sigmask, 0);

    if (fd == -1) {
        nxt_alert(&engine->task, "signalfd(%d) failed %E",
                  iou->signalfd.fd, nxt_errno);
        return NXT_ERROR;
    }

    iou->signalfd.fd = fd;

    if (nxt_fd_nonblocking(&engine->task, fd) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_debug(&engine->task, "io_uring signalfd(): %d", fd);

    iou->signalfd.data = engine->signals->handler;
    iou->signalfd.read_work_queue = &engine->fast_work_queue;
    iou->signalfd.read_handler = nxt_io_uring_signalfd_handler;
    iou->signalfd.log = engine->task.log;
    iou->signalfd.task = &engine->task;

    /*
     * Arm the signalfd's multishot read poll through the fallible primitive
     * rather than nxt_io_uring_enable_read(): the latter reports a failed arm
     * only by queuing nxt_io_uring_error_handler, which is a no-op for an
     * internal fd (the signalfd has no error_handler).  A silently swallowed
     * failure would let create() succeed with the signals blocked (sigprocmask
     * above) but never delivered.  Propagate NXT_ERROR instead so create()
     * unwinds -- nxt_io_uring_free() closes the signalfd, the sigmask stays
     * blocked exactly as on the epoll engine's own signalfd failure paths --
     * and the runtime falls back to epoll, whose add_signal() re-blocks and
     * re-arms it, mirroring how epoll treats a failed epoll_ctl() there.
     */
    if (nxt_slow_path(!nxt_io_uring_arm(engine, &iou->signalfd,
                                        NXT_IOU_DIR_READ, 1)))
    {
        nxt_alert(&engine->task, "io_uring failed to arm signalfd(%d)", fd);
        return NXT_ERROR;
    }

    iou->signalfd.read = NXT_EVENT_ACTIVE;

    return NXT_OK;
}


static void
nxt_io_uring_signalfd_handler(nxt_task_t *task, void *obj, void *data)
{
    int                      n;
    nxt_err_t                err;
    nxt_fd_event_t           *ev;
    nxt_work_handler_t       handler;
    struct signalfd_siginfo  sfd;

    ev = obj;
    handler = data;

    nxt_debug(task, "io_uring signalfd handler");

    /*
     * Drain the signalfd to EAGAIN: multishot poll delivery is edge-like
     * (one CQE per wakeup) and the CQ drain dedupes to one handler call, so
     * a single read() would strand any additional queued siginfo records
     * (e.g. a SIGTERM arriving while a SIGCHLD is already pending) until
     * some later, unrelated wakeup.
     */
    for ( ;; ) {
        n = read(ev->fd, &sfd, sizeof(struct signalfd_siginfo));
        err = (n == -1) ? nxt_errno : 0;

        nxt_debug(task, "read signalfd(%d): %d", ev->fd, n);

        if (n != sizeof(struct signalfd_siginfo)) {
            if (n == -1 && err == NXT_EAGAIN) {
                return;
            }

            nxt_alert(task, "read signalfd(%d) failed %E", ev->fd, err);
            return;
        }

        nxt_debug(task, "signalfd(%d) signo:%d", ev->fd, sfd.ssi_signo);

        handler(task, (void *) (uintptr_t) sfd.ssi_signo, NULL);
    }
}

#endif


static void
nxt_io_uring_free(nxt_event_engine_t *engine)
{
    int                    fd;
    nxt_io_uring_engine_t  *iou;

    iou = &engine->u.io_uring;

    nxt_debug(&engine->task, "io_uring free");

#if (NXT_HAVE_SIGNALFD)

    fd = iou->signalfd.fd;

    if (fd != -1 && close(fd) != 0) {
        nxt_alert(&engine->task, "signalfd close(%d) failed %E", fd, nxt_errno);
    }

#endif

    fd = iou->eventfd.fd;

    if (fd != -1 && close(fd) != 0) {
        nxt_alert(&engine->task, "eventfd close(%d) failed %E", fd, nxt_errno);
    }

    if (iou->ring_inited) {
        io_uring_queue_exit(&iou->ring);
    }

    nxt_free(iou->slots);

    nxt_memzero(iou, sizeof(nxt_io_uring_engine_t));
}


static nxt_io_uring_slot_t *
nxt_io_uring_slot(nxt_event_engine_t *engine, nxt_fd_t fd)
{
    uint32_t               nslots;
    nxt_io_uring_slot_t    *slots;
    nxt_io_uring_engine_t  *iou;

    iou = &engine->u.io_uring;

    /*
     * A negative fd (closed/reset socket) must not reach the table: cast to
     * uint32_t it would pass the grow check and then shrink the table to one
     * slot and index it out of bounds.  Callers all tolerate NULL.
     */
    if (nxt_slow_path(fd < 0)) {
        return NULL;
    }

    if (nxt_slow_path((uint32_t) fd >= iou->nslots)) {

        nslots = nxt_io_uring_pow2((uint32_t) fd + 1);

        slots = nxt_realloc(iou->slots, nslots * sizeof(nxt_io_uring_slot_t));
        if (nxt_slow_path(slots == NULL)) {
            return NULL;
        }

        nxt_memzero(&slots[iou->nslots],
                    (nslots - iou->nslots) * sizeof(nxt_io_uring_slot_t));

        iou->slots = slots;
        iou->nslots = nslots;
    }

    return &iou->slots[fd];
}


/*
 * Whether either poll direction is still live in the kernel for this fd.
 * delete/disable consult this rather than only ev->read/write because the
 * arm-failure escalation (nxt_io_uring_error_handler) force-clears the ev
 * state without disarming; an ev-state-only gate would then skip the
 * POLL_REMOVE and strand a live multishot poll.  Unlike a closed epoll fd
 * (which the kernel auto-deregisters), an io_uring poll keeps a kernel file
 * reference until POLL_REMOVE'd and its slot->ev keeps pointing at the
 * about-to-be-freed event -- a use-after-free once a stale-but-gen-matching
 * CQE lands.  A negative or out-of-range fd was never armed, so it reads
 * false without touching (or growing) the slot table.
 */

nxt_inline nxt_bool_t
nxt_io_uring_slot_armed(nxt_io_uring_engine_t *iou, nxt_fd_t fd)
{
    return (nxt_uint_t) fd < iou->nslots
           && (iou->slots[fd].read_armed || iou->slots[fd].write_armed);
}


static struct io_uring_sqe *
nxt_io_uring_get_sqe(nxt_event_engine_t *engine)
{
    struct io_uring_sqe  *sqe;

    sqe = io_uring_get_sqe(&engine->u.io_uring.ring);

    if (nxt_slow_path(sqe == NULL)) {
        /* SQ ring full: submit the accumulated batch to free slots. */
        (void) io_uring_submit(&engine->u.io_uring.ring);
        engine->u.io_uring.nsubmitted = 0;

        sqe = io_uring_get_sqe(&engine->u.io_uring.ring);
    }

    return sqe;
}


/*
 * Reset both directions and hand the fd to its error_handler; a faithful copy
 * of the epoll engine's nxt_epoll_error_handler().  Internal fds (the eventfd
 * doorbell and signalfd) have no error_handler, so guard the call -- the epoll
 * engine cannot reach this path for its own internal fds, but the io_uring
 * signalfd is armed through nxt_io_uring_enable_read() and therefore can.
 */

static void
nxt_io_uring_error_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_fd_event_t  *ev;

    ev = obj;

    ev->read = NXT_EVENT_INACTIVE;
    ev->write = NXT_EVENT_INACTIVE;

    if (nxt_fast_path(ev->error_handler != NULL)) {
        ev->error_handler(ev->task, ev, data);
    }
}


/*
 * A poller could not be installed: the slot table realloc failed (ENOMEM) or
 * the SQ ring stayed full even after a submit flush.  Leaving the fd-event
 * direction ACTIVE with no poll registered would strand it forever -- a
 * permanently stalled connection, or, via enable_accept, a listener that stops
 * accepting.  Mirror the epoll engine, whose nxt_epoll_commit_changes() queues
 * nxt_epoll_error_handler on the same fast_work_queue when epoll_ctl() fails.
 * Callers invoke this at most once per operation (they return right after), so
 * no per-fd dedupe is needed here.
 */

static void
nxt_io_uring_arm_failed(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_work_queue_add(&engine->fast_work_queue, nxt_io_uring_error_handler,
                       ev->task, ev, ev->data);
}


static nxt_bool_t
nxt_io_uring_arm(nxt_event_engine_t *engine, nxt_fd_event_t *ev, nxt_uint_t dir,
    nxt_bool_t multishot)
{
    uint32_t             gen;
    uint32_t             mask;
    struct io_uring_sqe  *sqe;
    nxt_io_uring_slot_t  *slot;

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        nxt_alert(ev->task, "io_uring slot alloc failed for fd %d", ev->fd);
        return 0;
    }

    /*
     * Flush this direction's deferred cancel (see nxt_io_uring_remove) before
     * installing a new poll.  This is what bounds the recorded state to a
     * single condemned arming per direction: remove() can only fail against
     * the arming installed here, and it can only record it because this flush
     * left the pending slot empty.  Arming on top of an unflushed cancel could
     * otherwise require two recorded generations if the new poll's remove
     * failed as well, losing one of the cancels (a kernel poll leaked to ring
     * teardown).  If even the cancel cannot get an SQE, fail the arm too; the
     * callers escalate exactly as for a failed arm proper.
     */
    if (dir == NXT_IOU_DIR_READ) {
        if (nxt_slow_path(slot->read_remove_pending)) {
            if (!nxt_io_uring_submit_remove(engine, ev->fd,
                                            slot->read_remove_gen,
                                            NXT_IOU_DIR_READ,
                                            slot->read_remove_accept))
            {
                return 0;
            }
            slot->read_remove_pending = 0;
            engine->u.io_uring.npending_removes--;
        }

    } else {
        if (nxt_slow_path(slot->write_remove_pending)) {
            if (!nxt_io_uring_submit_remove(engine, ev->fd,
                                            slot->write_remove_gen,
                                            NXT_IOU_DIR_WRITE, 0))
            {
                return 0;
            }
            slot->write_remove_pending = 0;
            engine->u.io_uring.npending_removes--;
        }
    }

    sqe = nxt_io_uring_get_sqe(engine);
    if (nxt_slow_path(sqe == NULL)) {
        nxt_alert(ev->task, "io_uring_get_sqe() failed for fd %d", ev->fd);
        return 0;
    }

    slot->ev = ev;

    if (dir == NXT_IOU_DIR_READ) {
        gen = slot->read_generation;
        mask = NXT_IOU_READ_MASK;
        slot->read_armed = 1;
        slot->read_multishot = multishot;
        slot->accept = 0;             /* a poll arming, not an accept         */

    } else {
        gen = slot->write_generation;
        mask = NXT_IOU_WRITE_MASK;
        slot->write_armed = 1;
    }

    if (multishot) {
        io_uring_prep_poll_multishot(sqe, ev->fd, mask);

    } else {
        io_uring_prep_poll_add(sqe, ev->fd, mask);
    }

    io_uring_sqe_set_data64(sqe, nxt_iou_ud(ev->fd, gen, dir, 0));

    engine->u.io_uring.nsubmitted++;

    nxt_debug(ev->task, "io_uring arm fd:%d dir:%d ms:%d gen:%uD",
              ev->fd, (int) dir, (int) multishot, (uint32_t) gen);

    return 1;
}


static void
nxt_io_uring_remove(nxt_event_engine_t *engine, nxt_fd_event_t *ev,
    nxt_uint_t dir)
{
    uint32_t             gen;
    nxt_bool_t           accept;
    nxt_io_uring_slot_t  *slot;

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        return;
    }

    /*
     * The armed flag is cleared and the generation bumped up front -- before
     * the cancel SQE is (attempted to be) obtained.  This is deliberate:
     * it keeps the slot immediately safe to re-arm and the fd safe to close and
     * reuse, because the bump invalidates every completion still in flight for
     * this arming (they carry the pre-bump generation and are rejected).  If
     * the SQE cannot be obtained the cancel is not lost -- it is recorded
     * (together with the arming's kind, see read_remove_accept) and retried
     * from nxt_io_uring_poll() under the pre-bump generation, so the leaked
     * kernel arming's file reference is dropped even after a close/reuse.
     */
    /*
     * Any disable/delete/close/oneshot on a direction invalidates a re-arm
     * owed for it (nxt_io_uring_arm_pend): the direction is no longer wanted,
     * so drop the pending record before it is retried.  Cleared up front so
     * the !armed early-return below cannot skip it.
     */
    accept = 0;

    if (dir == NXT_IOU_DIR_READ) {
        if (slot->read_arm_pending) {
            slot->read_arm_pending = 0;
            engine->u.io_uring.npending_arms--;
        }

        if (!slot->read_armed) {
            slot->read_generation++;
            return;
        }
        gen = slot->read_generation;
        accept = slot->accept;
        slot->read_armed = 0;
        slot->accept = 0;
        slot->read_generation++;

    } else {
        if (slot->write_arm_pending) {
            slot->write_arm_pending = 0;
            engine->u.io_uring.npending_arms--;
        }

        if (!slot->write_armed) {
            slot->write_generation++;
            return;
        }
        gen = slot->write_generation;
        slot->write_armed = 0;
        slot->write_generation++;
    }

    if (nxt_slow_path(!nxt_io_uring_submit_remove(engine, ev->fd, gen, dir,
                                                  accept)))
    {
        nxt_alert(ev->task, "io_uring_get_sqe() failed for fd %d, "
                  "deferring poll cancel", ev->fd);

        /*
         * Record the owed cancel for retry from the poll loop.  One recorded
         * generation per direction is provably sufficient: arm() flushes any
         * pending cancel before installing a new poll (failing the arm if it
         * cannot), so at most one condemned arming can exist per direction and
         * the pending slot is always empty when a remove fails.  The flag guard
         * is belt-and-braces so npending_removes cannot double-count.
         */
        if (dir == NXT_IOU_DIR_READ) {
            if (!slot->read_remove_pending) {
                slot->read_remove_pending = 1;
                engine->u.io_uring.npending_removes++;
            }
            slot->read_remove_gen = gen;
            slot->read_remove_accept = accept;

        } else {
            if (!slot->write_remove_pending) {
                slot->write_remove_pending = 1;
                engine->u.io_uring.npending_removes++;
            }
            slot->write_remove_gen = gen;
        }

        return;
    }

    nxt_debug(ev->task, "io_uring remove fd:%d dir:%d gen:%uD acc:%d",
              ev->fd, (int) dir, (uint32_t) gen, (int) accept);
}


/*
 * Prepare and account the cancel for a specific (fd, generation, direction)
 * arming.  Both cancel forms match by the exact user_data the arming was
 * issued with, so the pre-bump generation must be used: POLL_REMOVE for a
 * poll, ASYNC_CANCEL for a oneshot accept (which is not a poll and would be
 * missed by POLL_REMOVE) -- the accept flag selects which, and MUST reflect
 * the condemned arming's kind, not the slot's current one.  Returns 0 iff no
 * SQE could be obtained even after the get_sqe() submit-flush (SQ ring
 * exhausted while the CQ overflowed); the caller records the cancel for retry.
 */

static nxt_bool_t
nxt_io_uring_submit_remove(nxt_event_engine_t *engine, nxt_fd_t fd, uint32_t gen,
    nxt_uint_t dir, nxt_bool_t accept)
{
    struct io_uring_sqe  *sqe;

    sqe = nxt_io_uring_get_sqe(engine);
    if (nxt_slow_path(sqe == NULL)) {
        return 0;
    }

    if (accept) {
        io_uring_prep_cancel64(sqe,
                               nxt_iou_ud(fd, gen, NXT_IOU_DIR_READ, 1), 0);

    } else {
        io_uring_prep_poll_remove(sqe, nxt_iou_ud(fd, gen, dir, 0));
    }

    io_uring_sqe_set_data64(sqe, NXT_IOU_UD_REMOVE);

    engine->u.io_uring.nsubmitted++;

    return 1;
}


/*
 * Retry, at the top of nxt_io_uring_poll(), the cancels that could not get
 * an SQE when their direction was disabled/deleted/closed.  Gated by
 * npending_removes so the slot scan never runs in steady state.  A still-live
 * kernel arming keeps its file reference until this lands, so retrying until
 * an SQE is available is what stops a close-with-live-poller from leaking that
 * reference to ring teardown; the generation bump done at record time keeps any
 * completion the arming emits meanwhile harmless (and safe across an fd reuse).
 * The recorded read_remove_accept restores the correct cancel form for a
 * condemned accept arming.
 */

static void
nxt_io_uring_retry_pending_removes(nxt_event_engine_t *engine)
{
    uint32_t               fd;
    nxt_io_uring_slot_t    *slot;
    nxt_io_uring_engine_t  *iou;

    iou = &engine->u.io_uring;

    for (fd = 0; fd < iou->nslots && iou->npending_removes != 0; fd++) {
        slot = &iou->slots[fd];

        if (slot->read_remove_pending
            && nxt_io_uring_submit_remove(engine, fd, slot->read_remove_gen,
                                          NXT_IOU_DIR_READ,
                                          slot->read_remove_accept))
        {
            slot->read_remove_pending = 0;
            iou->npending_removes--;
        }

        if (slot->write_remove_pending
            && nxt_io_uring_submit_remove(engine, fd, slot->write_remove_gen,
                                          NXT_IOU_DIR_WRITE, 0))
        {
            slot->write_remove_pending = 0;
            iou->npending_removes--;
        }
    }
}


/*
 * Record a re-arm (POLL_ADD) that could not get an SQE so nxt_io_uring_poll()
 * can retry it.  Used only for a still-wanted direction whose re-arm failed on
 * SQ exhaustion (a transient condition): escalating such a failure through
 * nxt_io_uring_error() would be swallowed by its per-drain dedupe (the fd
 * already dispatched a handler this drain) and strand the direction ACTIVE with
 * no poller -- silent listener death, or a stalled connection.  The flag guard
 * keeps npending_arms from double-counting.
 */

static void
nxt_io_uring_arm_pend(nxt_event_engine_t *engine, nxt_io_uring_slot_t *slot,
    nxt_uint_t dir)
{
    if (dir == NXT_IOU_DIR_READ) {
        if (!slot->read_arm_pending) {
            slot->read_arm_pending = 1;
            engine->u.io_uring.npending_arms++;
        }

    } else {
        if (!slot->write_arm_pending) {
            slot->write_arm_pending = 1;
            engine->u.io_uring.npending_arms++;
        }
    }
}


/*
 * Retry, at the top of nxt_io_uring_poll(), the re-arms that could not get an
 * SQE (nxt_io_uring_arm_pend).  Gated by npending_arms so the scan never runs in
 * steady state.  The retry arms under the *current* generation exactly like a
 * fresh arm, which is correct: a disable/delete/close/oneshot in the meantime
 * would have cleared the record (nxt_io_uring_remove), so a surviving record
 * still refers to the same wanted arming.  The re-check (ev still present and
 * the direction still wanted and not already armed) is belt-and-braces against
 * a record that outlived its direction.  The previous iteration's
 * submit_and_wait flushed the SQ, so in practice an SQE is available here and
 * the arm rides this iteration's submit; a still-exhausted SQ leaves the record
 * for the next iteration (mirrors the pending-removes recovery).
 */

static void
nxt_io_uring_retry_pending_arms(nxt_event_engine_t *engine)
{
    uint32_t               fd;
    nxt_fd_event_t         *ev;
    nxt_io_uring_slot_t    *slot;
    nxt_io_uring_engine_t  *iou;

    iou = &engine->u.io_uring;

    for (fd = 0; fd < iou->nslots && iou->npending_arms != 0; fd++) {
        slot = &iou->slots[fd];

        if (slot->read_arm_pending) {
            ev = slot->ev;

            if (ev == NULL
                || ev->read == NXT_EVENT_INACTIVE
                || ev->read == NXT_EVENT_DISABLED
                || slot->read_armed
                || nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ,
                                    slot->read_multishot))
            {
                slot->read_arm_pending = 0;
                iou->npending_arms--;
            }
        }

        if (slot->write_arm_pending) {
            ev = slot->ev;

            if (ev == NULL
                || ev->write == NXT_EVENT_INACTIVE
                || ev->write == NXT_EVENT_DISABLED
                || slot->write_armed
                || nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_WRITE, 1))
            {
                slot->write_arm_pending = 0;
                iou->npending_arms--;
            }
        }
    }
}


/*
 * Self-heal a slot whose fd number was closed and reused without the engine
 * being told to cancel the previous owner's poll.  Connection fds are torn
 * down through nxt_fd_event_close() (nxt_conn_close), which reaches this
 * engine's close/delete and POLL_REMOVEs the poll; port fds, however, are
 * closed raw (nxt_port_close/nxt_port_read_close call nxt_socket_close/
 * nxt_fd_close directly), which is harmless for epoll -- the kernel drops a
 * closed fd from the epoll set -- but leaves an io_uring multishot poll armed
 * with slot->ev pointing at the freed event and the generation un-bumped.
 * When the fd number is then reused for a new event, enable_* would see the
 * stale armed flag and skip arming, stranding the new event's readiness.
 *
 * Detect the reuse (slot->ev set to a *different* event on the same fd) and
 * condemn both directions of the stale arming: nxt_io_uring_remove bumps the
 * generation -- so any CQE still produced by the old poll is rejected -- and
 * POLL_REMOVEs the leaked kernel poll on the old file.  arm() then installs a
 * fresh poll under the new generation and repoints slot->ev.
 *
 * Residual (documented, not closed here): a CQE that lands on the raw-closed
 * fd *before* the reuse can still write to / dispatch through the stale
 * slot->ev.  Fully closing that window requires the port teardown to
 * deregister from the engine, which cannot be done safely from
 * nxt_port_close() because a port may be closed from a thread other than the
 * one that owns its engine's ring (the router already posts cross-thread port
 * work via nxt_event_engine_post for exactly this reason); it is left as a
 * follow-up (a thread-safe port deregistration posted to port->engine).
 */

static void
nxt_io_uring_slot_reconcile(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

    slot = nxt_io_uring_slot(engine, ev->fd);

    if (slot == NULL || slot->ev == NULL || slot->ev == ev) {
        return;
    }

    if (slot->read_armed || slot->write_armed
        || slot->read_arm_pending || slot->write_arm_pending)
    {
        nxt_debug(ev->task, "io_uring stale slot reuse fd:%d", ev->fd);

        nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_READ);
        nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_WRITE);
    }

    slot->ev = NULL;
}


static void
nxt_io_uring_enable(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

    nxt_io_uring_slot_reconcile(engine, ev);

    ev->read = NXT_EVENT_ACTIVE;
    ev->write = NXT_EVENT_ACTIVE;

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }

    if (!slot->read_armed
        && nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ, 1)))
    {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }

    if (!slot->write_armed
        && nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_WRITE, 1)))
    {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }
}


static void
nxt_io_uring_disable(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    if (ev->read > NXT_EVENT_DISABLED || ev->write > NXT_EVENT_DISABLED
        || nxt_io_uring_slot_armed(&engine->u.io_uring, ev->fd))
    {
        ev->read = NXT_EVENT_INACTIVE;
        ev->write = NXT_EVENT_INACTIVE;

        nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_READ);
        nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_WRITE);
    }
}


static void
nxt_io_uring_delete(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    if (ev->read != NXT_EVENT_INACTIVE || ev->write != NXT_EVENT_INACTIVE
        || nxt_io_uring_slot_armed(&engine->u.io_uring, ev->fd))
    {
        ev->read = NXT_EVENT_INACTIVE;
        ev->write = NXT_EVENT_INACTIVE;

        nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_READ);
        nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_WRITE);
    }
}


/*
 * The POLL_REMOVE SQEs are submitted immediately so the kernel drops the
 * poll's file reference before Unit calls close(2); the generation bump makes
 * any completion that still slips through harmless, so the fd may be closed at
 * once and this returns false (no deferral needed).
 *
 * If the SQ ring was exhausted the cancel is deferred (nxt_io_uring_remove
 * records it; nxt_io_uring_poll retries).  Deferring the close(2) itself via
 * the return value would not help: the only caller that honors it
 * (nxt_conn_close_handler) re-closes from a zero-timer without re-entering the
 * engine, so it cannot retry the cancel.  It is also unnecessary: POLL_REMOVE
 * matches the request by user_data, not by fd, and the kernel poll pins its
 * own file reference, so the deferred cancel still lands after close(2) --
 * even if the fd number is reused, since the reused arming carries a bumped
 * generation and the recorded cancel targets the old one.
 */

static nxt_bool_t
nxt_io_uring_close(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_delete(engine, ev);

    if (engine->u.io_uring.nsubmitted != 0) {
        (void) io_uring_submit(&engine->u.io_uring.ring);
        engine->u.io_uring.nsubmitted = 0;
    }

    return ev->changing;
}


static void
nxt_io_uring_enable_read(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

    nxt_io_uring_slot_reconcile(engine, ev);

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }

    /*
     * If the multishot read poll is still armed in the kernel (the BLOCKED
     * case) re-enabling is a pure state write with zero SQEs -- a strict
     * syscall reduction over epoll's re-MOD.
     */
    if (!slot->read_armed
        && nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ, 1)))
    {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }

    ev->read = NXT_EVENT_ACTIVE;
}


static void
nxt_io_uring_enable_write(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

    nxt_io_uring_slot_reconcile(engine, ev);

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }

    if (!slot->write_armed
        && nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_WRITE, 1)))
    {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }

    ev->write = NXT_EVENT_ACTIVE;
}


static void
nxt_io_uring_disable_read(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    ev->read = NXT_EVENT_INACTIVE;

    nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_READ);
}


static void
nxt_io_uring_disable_write(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    ev->write = NXT_EVENT_INACTIVE;

    nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_WRITE);
}


/*
 * block_read/block_write only flip the state; the multishot poll stays armed
 * in the kernel and the poll loop simply refuses to dispatch a BLOCKED
 * direction (and, level-style, disarms it if it keeps firing).  This mirrors
 * epoll, where block_* issues no syscall.
 */

static void
nxt_io_uring_block_read(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    if (ev->read != NXT_EVENT_INACTIVE) {
        ev->read = NXT_EVENT_BLOCKED;
    }
}


static void
nxt_io_uring_block_write(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    if (ev->write != NXT_EVENT_INACTIVE) {
        ev->write = NXT_EVENT_BLOCKED;
    }
}


/*
 * Oneshot is a single non-multishot POLL_ADD: one CQE without F_MORE, which
 * auto-terminates the arming.  The direction moves ONESHOT -> DISABLED on
 * completion, mirroring epoll's EPOLLONESHOT bookkeeping.
 *
 * epoll arms a oneshot with EPOLL_CTL_MOD(EPOLLIN|EPOLLONESHOT), which
 * *replaces* the fd's whole interest mask, so the opposite direction's poll
 * is dropped and no readiness can be latched behind the caller's back.  The
 * bridge must reproduce that: cancel BOTH directions' live polls before
 * arming the oneshot.  Otherwise a still-armed multishot read poll (a conn
 * whose sendfile path calls oneshot_write with its read multishot live) would
 * keep consuming CQEs while ev->read is INACTIVE -- latching read_ready/EOF
 * that no handler runs for -- and, because slot->read_armed stays set, a later
 * enable_read() re-arms nothing and never re-checks the latched readiness,
 * stranding pipelined data or a FIN and stalling the keep-alive flow.
 * Cancelling first (remove bumps the generation, so a fresh enable_read()
 * re-arms a POLL_ADD that re-checks readiness at submission) also prevents a
 * duplicate poll when the armed direction is the one being re-armed here.
 * Removing only the directions that are actually armed avoids registering a
 * second poll under the SAME user_data and leaking an armed poll when the
 * opposite direction is flipped INACTIVE.
 */

static void
nxt_io_uring_oneshot_read(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

    slot = nxt_io_uring_slot(engine, ev->fd);

    if (slot != NULL) {
        if (slot->read_armed) {
            nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_READ);
        }

        if (slot->write_armed) {
            nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_WRITE);
        }
    }

    ev->read = NXT_EVENT_ONESHOT;
    ev->write = NXT_EVENT_INACTIVE;

    if (nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ, 0))) {
        nxt_io_uring_arm_failed(engine, ev);
    }
}


static void
nxt_io_uring_oneshot_write(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

    slot = nxt_io_uring_slot(engine, ev->fd);

    if (slot != NULL) {
        if (slot->read_armed) {
            nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_READ);
        }

        if (slot->write_armed) {
            nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_WRITE);
        }
    }

    ev->read = NXT_EVENT_INACTIVE;
    ev->write = NXT_EVENT_ONESHOT;

    if (nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_WRITE, 0))) {
        nxt_io_uring_arm_failed(engine, ev);
    }
}


/*
 * Completion-mode accept (tier >= ACCEPT): a ONESHOT IORING_OP_ACCEPT per
 * listen fd, re-armed after each completion.  res carries the accepted fd.
 *
 * Why oneshot and not IORING_ACCEPT_MULTISHOT: a multishot accept is a
 * PERSISTENT wake-one registration.  With N worker rings arming multishot
 * accepts on the same shared listen fd, the kernel keeps completing every
 * incoming connection into the same (first-armed) ring's CQ regardless of
 * whether that worker is busy -- one worker takes 100% of the connections
 * while the rest idle, capping throughput at a single worker (observed:
 * all 32 conns of a c=32 load accepted by one thread).  EPOLLEXCLUSIVE does
 * not have this pathology because it wakes only workers actually WAITING in
 * epoll_wait, so busy workers shed load organically.  A oneshot accept
 * restores exactly that property: each ring keeps at most ONE pending accept,
 * so the set of pending accepts is the set of workers that have reached
 * poll() -- the kernel distributes connections among them like N threads
 * blocked in accept(2), herd-free (accept is consuming) AND load-balanced
 * (a saturated worker re-arms late, naturally yielding to idle workers).
 * Because the SQ ring is only flushed at the top of poll(), a re-arm prepped
 * during a CQ drain is submitted when the worker next enters poll(), i.e.
 * after its queued work has run -- the arming inherently reflects worker
 * progress.  IORING_OP_ACCEPT evaluates the backlog at submission and
 * completes immediately when it is non-empty, so a burst larger than the
 * per-completion batch cannot stall (level-equivalent, contract item #5).
 *
 * SOCK_NONBLOCK | SOCK_CLOEXEC is passed as the accept flag to match Unit's
 * accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC) contract: nonblocking lets the
 * accepted fd skip the per-conn fixup, and CLOEXEC keeps it from leaking into
 * spawned application processes (the invariant the accept4 paths enforce).
 * The remote sockaddr is filled per accepted fd with getpeername() rather than
 * the accept addr buffer: the buffer would have to live in the slot across the
 * completion, and getpeername on the accepted fd is exact and simpler.
 *
 * Backpressure reuses the existing accept machinery unchanged: when a conn slot
 * cannot be allocated (max_connections) nxt_conn_accept_next -> _close_idle
 * calls disable_read, which cancels a pending oneshot accept (ASYNC_CANCEL by
 * its user_data), and the 100 ms listen timer's enable_accept re-arms it.  Any
 * connection the kernel accepted into a CQE we cannot service (over the limit,
 * or completed before the cancel took effect) is closed, never leaked.
 */

static nxt_bool_t nxt_io_uring_arm_accept(nxt_event_engine_t *engine,
    nxt_fd_event_t *ev);


static void
nxt_io_uring_enable_accept(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

    nxt_io_uring_slot_reconcile(engine, ev);

    ev->read = NXT_EVENT_ACTIVE;

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }

    if (slot->read_armed) {
        return;
    }

    if (engine->u.io_uring.tier >= NXT_IOU_TIER_ACCEPT) {
        if (nxt_slow_path(!nxt_io_uring_arm_accept(engine, ev))) {
            nxt_io_uring_arm_failed(engine, ev);
        }
        return;
    }

    /*
     * Tier POLL: a NON-multishot POLL_ADD (POLLIN) re-armed per accept batch.
     * Multishot poll never re-reports a still-non-empty backlog, so a partial
     * accept batch would strand the remaining connections (edge stall, contract
     * item #5); a single POLL_ADD re-checks readiness at submission -- firing
     * immediately when the backlog is non-empty -- which the !F_MORE re-arm
     * turns into the required level-triggered re-fire at one SQE per batch.
     */
    if (nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ, 0))) {
        nxt_io_uring_arm_failed(engine, ev);
    }
}


static nxt_bool_t
nxt_io_uring_arm_accept(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    struct io_uring_sqe  *sqe;
    nxt_io_uring_slot_t  *slot;

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        nxt_alert(ev->task, "io_uring slot alloc failed for fd %d", ev->fd);
        return 0;
    }

    /*
     * Flush the R direction's deferred cancel before installing the accept,
     * mirroring nxt_io_uring_arm(): it keeps the recorded state bounded to a
     * single condemned arming per direction.  Fail the arm if the cancel
     * cannot get an SQE; the callers escalate exactly as for a failed arm.
     */
    if (nxt_slow_path(slot->read_remove_pending)) {
        if (!nxt_io_uring_submit_remove(engine, ev->fd, slot->read_remove_gen,
                                        NXT_IOU_DIR_READ,
                                        slot->read_remove_accept))
        {
            return 0;
        }
        slot->read_remove_pending = 0;
        engine->u.io_uring.npending_removes--;
    }

    sqe = nxt_io_uring_get_sqe(engine);
    if (nxt_slow_path(sqe == NULL)) {
        nxt_alert(ev->task, "io_uring_get_sqe() failed for fd %d", ev->fd);
        return 0;
    }

    slot->ev = ev;
    slot->read_armed = 1;
    slot->accept = 1;
    slot->read_multishot = 0;

    io_uring_prep_accept(sqe, ev->fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe,
                        nxt_iou_ud(ev->fd, slot->read_generation,
                                   NXT_IOU_DIR_READ, 1));

    engine->u.io_uring.nsubmitted++;

    nxt_debug(ev->task, "io_uring arm accept fd:%d gen:%uD",
              ev->fd, (uint32_t) slot->read_generation);

    return 1;
}


#if (NXT_HAVE_SIGNALFD)

static nxt_int_t
nxt_io_uring_enable_post(nxt_event_engine_t *engine, nxt_work_handler_t handler)
{
    int                    fd;
    struct io_uring_sqe    *sqe;
    nxt_io_uring_engine_t  *iou;

    iou = &engine->u.io_uring;

    iou->post_handler = handler;

    fd = eventfd(0, EFD_NONBLOCK);
    if (fd == -1) {
        nxt_alert(&engine->task, "eventfd() failed %E", nxt_errno);
        return NXT_ERROR;
    }

    iou->eventfd.fd = fd;
    iou->eventfd.data = engine;
    iou->eventfd.log = engine->task.log;
    iou->eventfd.task = &engine->task;

    nxt_debug(&engine->task, "io_uring eventfd(): %d", fd);

    sqe = nxt_io_uring_get_sqe(engine);
    if (nxt_slow_path(sqe == NULL)) {
        return NXT_ERROR;
    }

    io_uring_prep_poll_multishot(sqe, fd, POLLIN);
    io_uring_sqe_set_data64(sqe, NXT_IOU_UD_POST);

    iou->nsubmitted++;

    return NXT_OK;
}


static void
nxt_io_uring_signal(nxt_event_engine_t *engine, nxt_uint_t signo)
{
    uint64_t  event;

    /*
     * Cross-thread wake-up: the eventfd write(2) is a kernel syscall,
     * independent of the ring (which is not safe to submit to from another
     * thread).  The signo is ignored; the work payload rides
     * locked_work_queue exactly as with epoll.
     */

    event = 1;

    if (nxt_slow_path(write(engine->u.io_uring.eventfd.fd, &event,
                            sizeof(uint64_t)) != sizeof(uint64_t)))
    {
        nxt_alert(&engine->task, "write(%d) to eventfd failed %E",
                  engine->u.io_uring.eventfd.fd, nxt_errno);
    }
}

#endif


static void
nxt_io_uring_poll(nxt_event_engine_t *engine, nxt_msec_t timeout)
{
    int                      ret;
    unsigned                 head, count;
    nxt_io_uring_engine_t    *iou;
    struct io_uring_cqe      *cqe;
    struct __kernel_timespec ts, *pts;

    iou = &engine->u.io_uring;

    /*
     * Recover the SQEs that an exhausted SQ ring refused earlier.  Both retries
     * run before the timeout is computed (post_rearm_pending may still be set
     * after the doorbell retry and then caps the wait) and their SQEs ride this
     * iteration's submit_and_wait.  Both are gated/flagged so they cost nothing
     * in steady state.
     */
    if (nxt_slow_path(iou->npending_removes != 0)) {
        nxt_io_uring_retry_pending_removes(engine);
    }

    if (nxt_slow_path(iou->npending_arms != 0)) {
        nxt_io_uring_retry_pending_arms(engine);
    }

    if (nxt_slow_path(iou->post_rearm_pending)) {
        struct io_uring_sqe  *sqe;

        sqe = nxt_io_uring_get_sqe(engine);
        if (sqe != NULL) {
            io_uring_prep_poll_multishot(sqe, iou->eventfd.fd, POLLIN);
            io_uring_sqe_set_data64(sqe, NXT_IOU_UD_POST);
            iou->nsubmitted++;
            iou->post_rearm_pending = 0;
        }
    }

    if (timeout == NXT_INFINITE_MSEC) {
        pts = NULL;

    } else {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (long) (timeout % 1000) * 1000000;
        pts = &ts;
    }

    /*
     * The eventfd doorbell's re-arm is still owed (SQ still exhausted above).
     * A cross-thread post writes the eventfd but yields no CQE against the dead
     * poll, so bound the wait: on the capped wake the doorbell retry above
     * re-arms a multishot POLL_ADD, which re-checks the still-signaled eventfd
     * at submission and fires at once, draining locked_work_queue.  Without the
     * cap a NXT_INFINITE_MSEC wait could strand posted work indefinitely.  Only
     * ever tightens the wait.
     */
    if (nxt_slow_path(iou->post_rearm_pending)
        && (pts == NULL || timeout > NXT_IOU_POST_REARM_CAP_MSEC))
    {
        ts.tv_sec = 0;
        ts.tv_nsec = (long) NXT_IOU_POST_REARM_CAP_MSEC * 1000000;
        pts = &ts;
    }

    /*
     * A previous drain saw CQ overflow: do not block so the kernel flushes its
     * completion backlog promptly (NODROP guarantees nothing was lost).
     */
    if (iou->overflowed) {
        iou->overflowed = 0;
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        pts = &ts;
    }

    nxt_debug(&engine->task, "io_uring submit_and_wait timeout:%M", timeout);

    /* One syscall flushes the batched SQEs and waits for a completion. */
    ret = io_uring_submit_and_wait_timeout(&iou->ring, &cqe, 1, pts, NULL);

    iou->nsubmitted = 0;

    nxt_thread_time_update(engine->task.thread);

    /*
     * -ETIME (deadline), -EINTR (signal) and -EAGAIN (transient) all just mean
     * "no wait satisfied"; there may still be completions to drain, so fall
     * through.  -EBUSY is load-bearing: with IORING_FEAT_NODROP the kernel
     * returns it when the CQ ring is full and refuses to submit more (which
     * could generate yet more completions) until it is drained -- returning
     * here without draining would livelock (the CQ never empties, every
     * subsequent submit_and_wait re-hits -EBUSY).  So drain the visible CQEs
     * (which frees CQ space) and let the next iteration re-submit the batch
     * liburing still holds queued.  Only a genuinely unexpected negative return
     * is fatal.
     */
    if (ret < 0
        && ret != -ETIME && ret != -EINTR && ret != -EAGAIN && ret != -EBUSY)
    {
        nxt_alert(&engine->task, "io_uring_submit_and_wait_timeout() failed %E",
                  -ret);
        return;
    }

    count = 0;

    /*
     * A new drain sequence: at most one read and one write handler dispatch
     * per fd for all CQEs of this drain, mirroring epoll's one event per fd
     * per epoll_wait().  Queued handlers always run before the next poll.
     */
    iou->drain_seq++;

    io_uring_for_each_cqe(&iou->ring, head, cqe) {
        nxt_debug(&engine->task, "io_uring cqe ud:%uxL res:%d flags:%uxD",
                  (uint64_t) cqe->user_data, cqe->res, cqe->flags);
        nxt_io_uring_handle_cqe(engine, cqe);
        count++;
    }

    io_uring_cq_advance(&iou->ring, count);

    if (nxt_slow_path(io_uring_cq_has_overflow(&iou->ring))) {
        iou->overflowed = 1;
    }
}


static void
nxt_io_uring_handle_cqe(nxt_event_engine_t *engine, struct io_uring_cqe *cqe)
{
    int                    res;
    uint32_t               gen;
    uint32_t               idx, mask;
    uint64_t               ud;
    nxt_uint_t             dir;
    nxt_bool_t             more;
    nxt_fd_event_t         *ev;
    nxt_io_uring_slot_t    *slot;

    ud = cqe->user_data;

    if (nxt_iou_ud_kind(ud) == NXT_IOU_KIND_INT) {
        nxt_io_uring_internal_cqe(engine, cqe);
        return;
    }

    idx = nxt_iou_ud_idx(ud);
    dir = nxt_iou_ud_dir(ud);
    gen = nxt_iou_ud_gen(ud);

    if (nxt_slow_path(idx >= engine->u.io_uring.nslots)) {
        return;
    }

    slot = &engine->u.io_uring.slots[idx];

    /*
     * A multishot-accept completion carries an accepted fd in res, not a poll
     * mask, and is self-identified by the ACCEPT bit so it is routed correctly
     * even after the slot's arming kind has changed (fd reuse).
     */
    if (nxt_iou_ud_accept(ud)) {
        nxt_io_uring_accept_cqe(engine, slot, gen, cqe);
        return;
    }

    /* Reject completions for a stale (disabled/closed) arming. */
    if (dir == NXT_IOU_DIR_READ) {
        if (gen != nxt_iou_gen(slot->read_generation)) {
            return;
        }

    } else {
        if (gen != nxt_iou_gen(slot->write_generation)) {
            return;
        }
    }

    res = cqe->res;
    more = (cqe->flags & IORING_CQE_F_MORE) != 0;

    /*
     * Multishot terminated (kernel dropped the registration under pressure, or
     * this was a oneshot): mark the direction unarmed so a later enable_*
     * re-issues an SQE.
     */
    if (!more) {
        if (dir == NXT_IOU_DIR_READ) {
            slot->read_armed = 0;

        } else {
            slot->write_armed = 0;
        }
    }

    ev = slot->ev;

    if (nxt_slow_path(ev == NULL)) {
        return;
    }

    if (res < 0) {

#if (NXT_HAVE_SIGNALFD)
        /*
         * The signalfd is armed as an ordinary fd poll but has no
         * error_handler (nxt_io_uring_error would tear it down to INACTIVE and
         * never re-arm, blocking signal delivery for the process's lifetime).
         * A poll error is almost always transient, so self-heal: re-arm the
         * multishot read poll (the !more branch above already cleared
         * read_armed), deferring to the pending-arm retry if no SQE is free,
         * and alert so a persistent failure is visible.
         */
        if (ev == &engine->u.io_uring.signalfd) {
            nxt_alert(&engine->task, "io_uring signalfd(%d) poll failed %E, "
                      "re-arming", ev->fd, -res);

            if (!slot->read_armed
                && nxt_slow_path(!nxt_io_uring_arm(engine, ev,
                                                   NXT_IOU_DIR_READ, 1)))
            {
                nxt_io_uring_arm_pend(engine, slot, NXT_IOU_DIR_READ);
            }

            return;
        }
#endif

        /*
         * A current-generation res<0 is a genuine poll failure, never a
         * self-inflicted cancel: every POLL_REMOVE this engine issues -- the
         * immediate one in nxt_io_uring_remove and the deferred retry from
         * nxt_io_uring_poll -- targets the *pre-bump* generation, so a
         * -ECANCELED/-EBADF/-ENOENT produced by one of our own cancels carries
         * the stale generation and was already dropped by the generation gate
         * above.  What reaches here is a real error on a live arming: treat the
         * fd as dead and hand it to its error_handler exactly once, deduped
         * across both direction pollers (see nxt_io_uring_error).
         */
        ev->error = -res;

        nxt_io_uring_error(engine, slot, ev);
        return;
    }

    mask = (uint32_t) res;

    /*
     * Multishot poll posts one CQE per wait-queue wakeup and never
     * re-reports a still-ready fd, so every "the wakeup was consumed"
     * scenario must resolve without a further kernel notification:
     *
     * - Data + FIN in one wakeup: a single CQE carries POLLIN|POLLRDHUP.
     *   recv() consuming the data as a short read clears read_ready (the
     *   level contract), losing the pending EOF.  The epoll_eof mark below
     *   plus the nxt_io_uring_conn_io_recvbuf shim force read_ready back on
     *   so the caller loops and observes the EOF.  (The fix for the
     *   proxy-keepalive stall.)
     * - Full-buffer read (n == buffer size): recvbuf keeps read_ready set,
     *   so the reader continues without a new CQE.  Safe.
     * - Port-socket loops drain to EAGAIN while read_ready holds, so a
     *   message + more-data single wakeup is fully consumed.  Safe.
     * - Write EAGAIN then later writability: the socket-buffer drain is a
     *   new wakeup, producing a new CQE on the still-armed W poll.  Safe.
     * - BLOCKED fires -> latch read_ready only, poll stays armed (edge-like
     *   multishot cannot re-report steady state, so no busy loop); the conn
     *   read discipline consumes the latch on its next voluntary read and the
     *   subsequent enable_read is a free state write.  Safe.
     * - Listen sockets: non-multishot POLL_ADD; the !F_MORE re-arm below
     *   re-checks the backlog at submission.  Safe (contract item #5).
     */

    if (dir == NXT_IOU_DIR_READ) {

        /*
         * Pending EOF under edge-like delivery; consumed by the recvbuf
         * shim.  Set-only: CQEs are per-wakeup snapshots, not state-merged
         * like epoll events, so a plain POLLIN CQE can be delivered after
         * the POLLRDHUP one within a single drain and must not erase the
         * latched EOF.  Socket EOF/error never un-happens; a fresh conn
         * starts zeroed.
         */
        if (mask & (POLLRDHUP | POLLHUP)) {
            ev->epoll_eof = 1;
        }

        if (mask & (POLLERR | POLLHUP)) {
            ev->epoll_error = 1;
        }

        /*
         * Single-dispatch policy (mirrors nxt_epoll_poll()).  Read and write
         * are independently armed pollers whose masks both carry the error
         * bits, so a dead socket posts a CQE on BOTH directions.  Dispatch the
         * read_handler only on real read readiness -- POLLIN, i.e. data or a
         * readable EOF whose recv() returns 0; a combined data+error CQE lands
         * here too and recv() surfaces the error, exactly as epoll clears its
         * `error` flag once it has queued read_handler.  An ERR/HUP-only CQE
         * (no POLLIN) instead routes to a single ev->error_handler via
         * nxt_io_uring_error(), which dedupes the two pollers' error CQEs so a
         * socket error never runs both direction handlers -- the second on
         * state the first handler may already have freed.
         *
         * The dispatch gate also skips this fd if an error_handler was already
         * queued for it this drain (error_seq): the error CQE and this readiness
         * CQE arrive on independent pollers in arbitrary order, and epoll's
         * single per-fd event never runs a readiness handler *after* the error
         * handler queued for the same fd -- doing so here would dispatch onto a
         * connection the error_handler may free first.
         */
        if (mask & POLLIN) {
            ev->read_ready = 1;

            if (ev->read == NXT_EVENT_ONESHOT) {
                ev->read = NXT_EVENT_DISABLED;
            }

            /*
             * Dispatch only an active, non-BLOCKED direction, once per drain.
             *
             * A BLOCKED direction latches read_ready above but is NOT
             * dispatched and -- the Stage-1 refinement -- is NOT disarmed: the
             * multishot poll stays armed exactly as epoll leaves a blocked fd
             * in its set (block_read makes no syscall).  Multishot poll is
             * edge-like (one CQE per new wait-queue wakeup, never a re-report
             * of steady state), so a BLOCKED direction cannot busy-loop the way
             * the Stage-1 disarm feared; staying armed makes the following
             * enable_read a pure state write (zero SQEs), removing the
             * POLL_REMOVE + POLL_ADD pair Stage 1 paid per read.  The load-
             * bearing invariants are unchanged: dispatch is gated on state
             * (never BLOCKED) and on the per-drain read_seq dedupe, generations
             * still reject stale CQEs, and the conn read discipline consumes
             * the latched read_ready on its next voluntary read.  DISABLED
             * (a just-fired oneshot) still dispatches its single event.
             */
            if (ev->read != NXT_EVENT_INACTIVE
                && ev->read != NXT_EVENT_BLOCKED
                && slot->read_seq != engine->u.io_uring.drain_seq
                && slot->error_seq != engine->u.io_uring.drain_seq)
            {
                slot->read_seq = engine->u.io_uring.drain_seq;

                nxt_work_queue_add(ev->read_work_queue, ev->read_handler,
                                   ev->task, ev, ev->data);
            }

        } else if (mask & (POLLERR | POLLHUP)) {

            if (ev->read == NXT_EVENT_BLOCKED) {
                nxt_io_uring_disable_read(engine, ev);
                return;
            }

            nxt_io_uring_error(engine, slot, ev);
            return;
        }

        /*
         * A terminated arming (!F_MORE) on a still-armed direction is
         * re-issued so no readiness is missed: a kernel-dropped multishot,
         * the per-batch listen POLL_ADD, and events such as the signalfd
         * whose handler never calls enable_read.  The slot's recorded mode
         * keeps listen sockets non-multishot.  A oneshot (now DISABLED) or
         * a disabled direction is left alone.  If the re-arm cannot get an
         * SQE (transient SQ exhaustion), record it for retry from the poll
         * loop rather than escalating: escalation goes through
         * nxt_io_uring_error(), which self-dedupes and would swallow this call
         * whenever the fd already dispatched a read handler this drain (the
         * common case for a listen POLL_ADD whose CQE we just dispatched),
         * leaving the listener ACTIVE with no poller -- it silently stops
         * accepting.  The pending-arm retry re-issues the POLL_ADD next
         * iteration instead of killing a healthy listener for a transient
         * exhaustion.
         */
        if (!more
            && ev->read != NXT_EVENT_INACTIVE
            && ev->read != NXT_EVENT_DISABLED)
        {
            if (nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ,
                                                slot->read_multishot)))
            {
                nxt_io_uring_arm_pend(engine, slot, NXT_IOU_DIR_READ);
            }
        }

    } else {

        if (mask & (POLLERR | POLLHUP)) {
            ev->epoll_error = 1;
        }

        /* See the read direction's single-dispatch policy note above. */
        if (mask & POLLOUT) {
            ev->write_ready = 1;

            if (ev->write == NXT_EVENT_ONESHOT) {
                ev->write = NXT_EVENT_DISABLED;
            }

            /* Stay-armed-while-BLOCKED, latch only (see the read branch). */
            if (ev->write != NXT_EVENT_INACTIVE
                && ev->write != NXT_EVENT_BLOCKED
                && slot->write_seq != engine->u.io_uring.drain_seq
                && slot->error_seq != engine->u.io_uring.drain_seq)
            {
                slot->write_seq = engine->u.io_uring.drain_seq;

                nxt_work_queue_add(ev->write_work_queue, ev->write_handler,
                                   ev->task, ev, ev->data);
            }

        } else if (mask & (POLLERR | POLLHUP)) {

            if (ev->write == NXT_EVENT_BLOCKED) {
                nxt_io_uring_disable_write(engine, ev);
                return;
            }

            nxt_io_uring_error(engine, slot, ev);
            return;
        }

        if (!more
            && ev->write != NXT_EVENT_INACTIVE
            && ev->write != NXT_EVENT_DISABLED)
        {
            if (nxt_slow_path(!nxt_io_uring_arm(engine, ev,
                                                NXT_IOU_DIR_WRITE, 1)))
            {
                nxt_io_uring_arm_pend(engine, slot, NXT_IOU_DIR_WRITE);
            }
        }
    }
}


/*
 * Queue ev->error_handler once for an fd whose poll surfaced ERR/HUP with no
 * actionable readiness, or whose re-arm could not get an SQE.  This is the
 * bridge's analogue of nxt_epoll_poll()'s error path (which queues exactly one
 * nxt_epoll_error_handler per event): epoll dispatches it only when a direction
 * is still active and no read/write handler already ran for the fd, since a
 * dispatched readiness handler will itself observe the error via recv()/send().
 * The bridge's read and write pollers each raise the error separately, so gate
 * on all three per-fd drain sequences -- skip if this fd already dispatched a
 * read, write, or error handler this drain -- to keep the error_handler from
 * running a second time on a connection an earlier handler may have freed.
 */

static void
nxt_io_uring_error(nxt_event_engine_t *engine, nxt_io_uring_slot_t *slot,
    nxt_fd_event_t *ev)
{
    uint64_t  seq;

    if (!nxt_fd_event_is_active(ev->read)
        && !nxt_fd_event_is_active(ev->write))
    {
        return;
    }

    seq = engine->u.io_uring.drain_seq;

    if (slot->read_seq == seq || slot->write_seq == seq
        || slot->error_seq == seq)
    {
        return;
    }

    slot->error_seq = seq;

    ev->read_ready = 1;
    ev->write_ready = 1;

    nxt_work_queue_add(&engine->fast_work_queue, nxt_io_uring_error_handler,
                       ev->task, ev, ev->data);
}


/*
 * Oneshot-accept completion.  res is the accepted fd (>= 0) or a negative
 * errno.  A oneshot accept CQE never carries F_MORE -- consuming the CQE
 * consumes the arming -- so re-arming is the NORMAL path here, not the
 * exception: every outcome below either re-arms (success, benign error),
 * defers the re-arm to the 100 ms listen timer (resource errors, and
 * max_connections backpressure via close_idle -> disable_read), or drops the
 * listener deliberately (cancel/close).  Each CQE feeds an adapted
 * nxt_conn_accept flow directly, replacing the poll-mode accept4 syscall.
 */

static void
nxt_io_uring_accept_cqe(nxt_event_engine_t *engine, nxt_io_uring_slot_t *slot,
    uint16_t gen, struct io_uring_cqe *cqe)
{
    int                 res;
    socklen_t           socklen;
    nxt_conn_t          *c;
    nxt_fd_event_t      *ev;
    nxt_listen_event_t  *lev;

    res = cqe->res;

    /*
     * Stale arming: reject on generation mismatch -- the same load-bearing
     * invariant used for poll CQEs -- but first close any fd the CQE carries
     * so it is not leaked (the accept analogue of "a stale recv CQE must
     * return its buffer").  A stale oneshot CQE with a real fd arises when the
     * kernel completed the accept before a disable/close-issued ASYNC_CANCEL
     * took effect: the cancel then finds nothing, the completed CQE still
     * sits in the CQ, and the generation bump from the disable is what marks
     * it stale.  The connection was already accepted from the backlog, so
     * close(2) is the only correct disposition.
     */
    if (gen != nxt_iou_gen(slot->read_generation)) {
        if (res >= 0) {
            (void) close(res);
        }
        return;
    }

    /* Oneshot: the completion consumes the arming. */
    slot->read_armed = 0;
    slot->accept = 0;

    ev = slot->ev;

    if (nxt_slow_path(ev == NULL)) {
        if (res >= 0) {
            (void) close(res);
        }
        return;
    }

    lev = nxt_container_of(ev, nxt_listen_event_t, socket);

    if (res < 0) {
        /* -ECANCELED/-EBADF/-ENOENT: the accept was cancelled; fd is gone. */
        if (res == -ECANCELED || res == -EBADF || res == -ENOENT) {
            return;
        }

        /*
         * nxt_conn_accept_error keeps the poll-path semantics: EAGAIN and
         * ECONNABORTED are benign (listener stays ACTIVE -> re-arm below);
         * EMFILE/ENFILE/ENOBUFS/ENOMEM schedule the idle-conn reaper, arm the
         * 100 ms listen timer and disable the listener (INACTIVE -> the re-arm
         * below is skipped; the timer's enable_accept re-arms instead).
         */
        nxt_conn_accept_error(ev->task, lev, "accept", -res);

        if (ev->read == NXT_EVENT_ACTIVE) {
            if (nxt_slow_path(!nxt_io_uring_arm_accept(engine, ev))) {
                nxt_io_uring_arm_failed(engine, ev);
            }
        }

        return;
    }

    /* res >= 0: a newly accepted connection fd. */

    c = lev->next;

    if (nxt_slow_path(c == NULL)) {
        /*
         * No pre-allocated conn: max_connections backpressure is in force
         * (close_idle disabled the listener and armed the 100 ms timer), but
         * the kernel completed this accept before the cancel took effect.
         * Close it; the timer will resume accepting.
         */
        (void) close(res);
        return;
    }

    /*
     * Fill the remote sockaddr for this specific fd with getpeername(): exact,
     * and avoids keeping a per-arming addr buffer alive in the slot across the
     * completion.  On failure (the peer already dropped the connection, e.g.
     * an RST between the kernel's accept and this drain) the connection is
     * DROPPED rather than served with a zeroed sockaddr: sa_family 0 would
     * flow into access logs and ACL matching as a bogus client address.  The
     * fd is closed, the pre-allocated conn stays in lev->next for the next
     * completion, and the re-arm below continues accepting -- the same
     * disposition as the poll-path accept4() failing for a torn-down peer.
     */
    socklen = c->remote->socklen;

    if (nxt_slow_path(getpeername(res, &c->remote->u.sockaddr, &socklen)
                      != 0))
    {
        nxt_debug(ev->task, "io_uring accept(%d): getpeername(%d) failed %E",
                  ev->fd, res, nxt_errno);

        (void) close(res);

    } else {
        c->socket.fd = res;

        nxt_debug(ev->task, "io_uring accept(%d): %d", ev->fd, res);

        /*
         * Adapted accept flow: nxt_conn_accept sets the conn up, enqueues the
         * listen handler, and pre-allocates the next lev->next (or triggers
         * close_idle backpressure, which disables/cancels this listener).  Its
         * poll-mode re-arm tail is inert here: lev->socket.read_ready is never
         * set in completion mode, so it never reschedules the poll accept
         * handler.
         */
        nxt_conn_accept(ev->task, lev, c);
    }

    /*
     * Re-arm the oneshot accept unless backpressure disabled the listener
     * (close_idle leaves it INACTIVE; the listen timer re-arms instead).  The
     * SQE is prepped inline but only submitted when this worker next enters
     * poll(), i.e. after all queued work has run -- so a busy worker's accept
     * goes pending late, yielding connections to idle workers.  Deferring the
     * re-arm to a work item behind the listen handler was measured to give an
     * identical per-thread accept distribution (SQ batching already delays
     * both to the same submit), so the inline form is kept as the simpler one.
     * A failed re-arm must not silently strand the listener: escalate to its
     * error_handler (at most once per completion; the caller returns after).
     */
    if (ev->read == NXT_EVENT_ACTIVE && !slot->read_armed) {
        if (nxt_slow_path(!nxt_io_uring_arm_accept(engine, ev))) {
            nxt_io_uring_arm_failed(engine, ev);
        }
    }
}


static void
nxt_io_uring_internal_cqe(nxt_event_engine_t *engine, struct io_uring_cqe *cqe)
{
    ssize_t                n;
    uint64_t               value;
    struct io_uring_sqe    *sqe;
    nxt_io_uring_engine_t  *iou;

    iou = &engine->u.io_uring;

    if (cqe->user_data != NXT_IOU_UD_POST) {
        /* POLL_REMOVE / cancel completions: nothing to do. */
        return;
    }

    /* Re-arm the doorbell if the kernel dropped the multishot eventfd poll. */
    if ((cqe->flags & IORING_CQE_F_MORE) == 0) {
        sqe = nxt_io_uring_get_sqe(engine);

        if (nxt_fast_path(sqe != NULL)) {
            io_uring_prep_poll_multishot(sqe, iou->eventfd.fd, POLLIN);
            io_uring_sqe_set_data64(sqe, NXT_IOU_UD_POST);
            iou->nsubmitted++;
            iou->post_rearm_pending = 0;

        } else {
            /*
             * SQ exhausted: the doorbell would otherwise be silently dead, and a
             * dead doorbell means a cross-thread post writes the eventfd but
             * wakes nothing -- an unbounded sleep with locked_work_queue work
             * stranded.  Flag the owed re-arm; nxt_io_uring_poll() retries it
             * and, until it lands, caps the wait so the miss is bounded.
             */
            iou->post_rearm_pending = 1;
        }
    }

    /* Drain the eventfd counter so the next post produces a fresh edge. */
    do {
        n = read(iou->eventfd.fd, &value, sizeof(uint64_t));
    } while (n == sizeof(uint64_t));

    if (iou->post_handler != NULL) {
        iou->post_handler(&engine->task, NULL, NULL);
    }
}


#if (NXT_HAVE_ACCEPT4)

static void
nxt_io_uring_conn_io_accept4(nxt_task_t *task, void *obj, void *data)
{
    socklen_t           socklen;
    nxt_conn_t          *c;
    nxt_socket_t        s;
    struct sockaddr     *sa;
    nxt_listen_event_t  *lev;

    lev = obj;
    c = lev->next;

    lev->ready--;
    lev->socket.read_ready = (lev->ready != 0);

    sa = &c->remote->u.sockaddr;
    socklen = c->remote->socklen;
    /*
     * The returned socklen is ignored here,
     * see comment in nxt_conn_io_accept().
     *
     * SOCK_CLOEXEC keeps the accepted client socket from leaking into spawned
     * application processes, matching the fcntl(FD_CLOEXEC) that the generic
     * nxt_conn_io_accept() applies on the plain accept() path.
     */
    s = accept4(lev->socket.fd, sa, &socklen, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (s != -1) {
        c->socket.fd = s;

        nxt_debug(task, "accept4(%d): %d", lev->socket.fd, s);

        nxt_conn_accept(task, lev, c);
        return;
    }

    nxt_conn_accept_error(task, lev, "accept4", nxt_errno);
}

#endif


/*
 * A wrapper around the standard nxt_conn_io_recvbuf() to enforce reading a
 * pending EOF under the engine's edge-like delivery, identical to the epoll
 * edge engine's shim: when data and FIN arrive in a single wakeup the kernel
 * posts one CQE and never re-reports the fd, so a short read that clears
 * read_ready would strand the EOF forever.
 */

static ssize_t
nxt_io_uring_conn_io_recvbuf(nxt_conn_t *c, nxt_buf_t *b)
{
    ssize_t  n;

    n = nxt_conn_io_recvbuf(c, b);

    if (n > 0 && c->socket.epoll_eof) {
        c->socket.read_ready = 1;
    }

    return n;
}
