
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
 * The engine therefore reuses the generic nxt_unix_conn_io (level-emulating)
 * I/O layer and needs no edge-mode shims.
 *
 * The load-bearing correctness invariant is the per-direction generation
 * counter encoded into each SQE's user_data (see nxt_io_uring_slot_t and the
 * user_data helpers below).  disable/delete/close bump the generation so any
 * completion produced for a stale arming -- including one that lands after the
 * fd is closed and its number reused -- is dropped before the event object is
 * touched.
 *
 * Debug-only override:
 *   NXT_IO_URING_FORCE_TIER=none  forces the create()-time probe to fail so
 *                                 the runtime falls back to epoll.
 */


/* Resolved feature tiers.  Stage 1 only needs multishot POLL_ADD. */
#define NXT_IOU_TIER_NONE    0
#define NXT_IOU_TIER_POLL    1


/*
 * user_data layout returned verbatim in every CQE:
 *
 *   bit  0      DIR   0 = read poll, 1 = write poll
 *   bit  1      KIND  0 = fd poll (slot-indexed), 1 = internal sentinel
 *   bits 2..15  GEN   14-bit per-direction generation (stale-CQE rejection)
 *   bits 16..   IDX   slot index == fd number
 */

#define NXT_IOU_DIR_READ     0
#define NXT_IOU_DIR_WRITE    1

#define NXT_IOU_KIND_FD      0
#define NXT_IOU_KIND_INT     1

#define NXT_IOU_GEN_MASK     0x3FFF

#define nxt_iou_ud(idx, gen, dir)                                             \
    ( ((uint64_t) (idx) << 16)                                                \
      | (((uint64_t) ((gen) & NXT_IOU_GEN_MASK)) << 2)                        \
      | ((uint64_t) (NXT_IOU_KIND_FD) << 1)                                   \
      | (uint64_t) (dir) )

#define nxt_iou_ud_dir(ud)    ((uint32_t) ((ud) & 0x1))
#define nxt_iou_ud_kind(ud)   ((uint32_t) (((ud) >> 1) & 0x1))
#define nxt_iou_ud_gen(ud)    ((uint16_t) (((ud) >> 2) & NXT_IOU_GEN_MASK))
#define nxt_iou_ud_idx(ud)    ((uint32_t) ((ud) >> 16))

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
    nxt_fd_t fd, uint16_t gen, nxt_uint_t dir);
static void nxt_io_uring_retry_pending_removes(nxt_event_engine_t *engine);

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
static void nxt_io_uring_internal_cqe(nxt_event_engine_t *engine,
    struct io_uring_cqe *cqe);

#if (NXT_HAVE_ACCEPT4)
static void nxt_io_uring_conn_io_accept4(nxt_task_t *task, void *obj,
    void *data);
#endif


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

    &nxt_unix_conn_io,

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
    uint32_t  p;

    p = 1;

    while (p < n) {
        p <<= 1;
    }

    return p;
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

        nxt_io_uring_test_accept4(engine, &nxt_unix_conn_io);
    }

    return NXT_OK;
}


static nxt_int_t
nxt_io_uring_setup(nxt_event_engine_t *engine, nxt_uint_t mchanges,
    nxt_uint_t mevents)
{
    int                     ret;
    char                    *force;
    uint32_t                nslots;
    struct io_uring_params  params;
    nxt_io_uring_engine_t   *iou;

    iou = &engine->u.io_uring;

    /*
     * Debug override: NXT_IO_URING_FORCE_TIER=none forces this create() to
     * fail cleanly so the runtime falls back to epoll.  Any other value keeps
     * the resolved tier (Stage 1 caps at POLL regardless).
     */
    force = getenv("NXT_IO_URING_FORCE_TIER");

    if (force != NULL && nxt_strcmp(force, "none") == 0) {
        nxt_log(&engine->task, NXT_LOG_INFO,
                "io_uring disabled by NXT_IO_URING_FORCE_TIER=none");
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

    nxt_memzero(&params, sizeof(struct io_uring_params));

    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = iou->cq_entries;

    ret = io_uring_queue_init_params(iou->sq_entries, &iou->ring, &params);

    if (ret < 0) {
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

    nxt_debug(&engine->task, "io_uring_queue_init(): sq:%uD cq:%uD features:%uxD",
              iou->sq_entries, params.cq_entries, params.features);

    if (!nxt_io_uring_multishot_supported(&iou->ring)) {
        nxt_log(&engine->task, NXT_LOG_INFO,
                "io_uring multishot poll is not supported");
        return NXT_ERROR;
    }

    iou->tier = NXT_IOU_TIER_POLL;

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
    nxt_fd_event_t           *ev;
    nxt_work_handler_t       handler;
    struct signalfd_siginfo  sfd;

    ev = obj;
    handler = data;

    nxt_debug(task, "io_uring signalfd handler");

    n = read(ev->fd, &sfd, sizeof(struct signalfd_siginfo));

    nxt_debug(task, "read signalfd(%d): %d", ev->fd, n);

    if (n != sizeof(struct signalfd_siginfo)) {
        nxt_alert(task, "read signalfd(%d) failed %E", ev->fd, nxt_errno);
        return;
    }

    nxt_debug(task, "signalfd(%d) signo:%d", ev->fd, sfd.ssi_signo);

    handler(task, (void *) (uintptr_t) sfd.ssi_signo, NULL);
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

    if (iou->tier != NXT_IOU_TIER_NONE) {
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
    uint16_t             gen;
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
                                            NXT_IOU_DIR_READ))
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
                                            NXT_IOU_DIR_WRITE))
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

    io_uring_sqe_set_data64(sqe, nxt_iou_ud(ev->fd, gen, dir));

    engine->u.io_uring.nsubmitted++;

    nxt_debug(ev->task, "io_uring arm fd:%d dir:%d ms:%d gen:%uD",
              ev->fd, (int) dir, (int) multishot, (uint32_t) gen);

    return 1;
}


static void
nxt_io_uring_remove(nxt_event_engine_t *engine, nxt_fd_event_t *ev,
    nxt_uint_t dir)
{
    uint16_t             gen;
    nxt_io_uring_slot_t  *slot;

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        return;
    }

    /*
     * The armed flag is cleared and the generation bumped up front -- before
     * the POLL_REMOVE SQE is (attempted to be) obtained.  This is deliberate:
     * it keeps the slot immediately safe to re-arm and the fd safe to close and
     * reuse, because the bump invalidates every completion still in flight for
     * this arming (they carry the pre-bump generation and are rejected).  If
     * the SQE cannot be obtained the cancel is not lost -- it is recorded and
     * retried from nxt_io_uring_poll() under the pre-bump generation, so the
     * leaked kernel poll's file reference is dropped even after a close/reuse.
     */
    if (dir == NXT_IOU_DIR_READ) {
        if (!slot->read_armed) {
            slot->read_generation++;
            return;
        }
        gen = slot->read_generation;
        slot->read_armed = 0;
        slot->read_generation++;

    } else {
        if (!slot->write_armed) {
            slot->write_generation++;
            return;
        }
        gen = slot->write_generation;
        slot->write_armed = 0;
        slot->write_generation++;
    }

    if (nxt_slow_path(!nxt_io_uring_submit_remove(engine, ev->fd, gen, dir))) {
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

        } else {
            if (!slot->write_remove_pending) {
                slot->write_remove_pending = 1;
                engine->u.io_uring.npending_removes++;
            }
            slot->write_remove_gen = gen;
        }

        return;
    }

    nxt_debug(ev->task, "io_uring remove fd:%d dir:%d gen:%uD",
              ev->fd, (int) dir, (uint32_t) gen);
}


/*
 * Prepare and account a POLL_REMOVE for a specific (fd, generation, direction)
 * arming.  POLL_REMOVE matches by the exact user_data the POLL_ADD was issued
 * with, so the pre-bump generation must be used.  Returns 0 iff no SQE could be
 * obtained even after the get_sqe() submit-flush (SQ ring exhausted while the
 * CQ overflowed); the caller records the cancel for retry.
 */

static nxt_bool_t
nxt_io_uring_submit_remove(nxt_event_engine_t *engine, nxt_fd_t fd, uint16_t gen,
    nxt_uint_t dir)
{
    struct io_uring_sqe  *sqe;

    sqe = nxt_io_uring_get_sqe(engine);
    if (nxt_slow_path(sqe == NULL)) {
        return 0;
    }

    io_uring_prep_poll_remove(sqe, nxt_iou_ud(fd, gen, dir));
    io_uring_sqe_set_data64(sqe, NXT_IOU_UD_REMOVE);

    engine->u.io_uring.nsubmitted++;

    return 1;
}


/*
 * Retry, at the top of nxt_io_uring_poll(), the POLL_REMOVEs that could not get
 * an SQE when their direction was disabled/deleted/closed.  Gated by
 * npending_removes so the slot scan never runs in steady state.  A still-live
 * kernel poll keeps its file reference until this lands, so retrying until an
 * SQE is available is what stops a close-with-live-poller from leaking that
 * reference to ring teardown; the generation bump done at record time keeps any
 * completion the poll emits meanwhile harmless (and safe across an fd reuse).
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
                                          NXT_IOU_DIR_READ))
        {
            slot->read_remove_pending = 0;
            iou->npending_removes--;
        }

        if (slot->write_remove_pending
            && nxt_io_uring_submit_remove(engine, fd, slot->write_remove_gen,
                                          NXT_IOU_DIR_WRITE))
        {
            slot->write_remove_pending = 0;
            iou->npending_removes--;
        }
    }
}


static void
nxt_io_uring_enable(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

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
 */

static void
nxt_io_uring_oneshot_read(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_READ);
    nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_WRITE);

    ev->read = NXT_EVENT_ONESHOT;
    ev->write = NXT_EVENT_INACTIVE;

    if (nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ, 0))) {
        nxt_io_uring_arm_failed(engine, ev);
    }
}


static void
nxt_io_uring_oneshot_write(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_READ);
    nxt_io_uring_remove(engine, ev, NXT_IOU_DIR_WRITE);

    ev->read = NXT_EVENT_INACTIVE;
    ev->write = NXT_EVENT_ONESHOT;

    if (nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_WRITE, 0))) {
        nxt_io_uring_arm_failed(engine, ev);
    }
}


/*
 * enable_accept arms a multishot POLL_ADD (POLLIN) on the listen socket.  A
 * multishot poll on a persistently-readable fd keeps posting CQEs while the
 * backlog is non-empty, giving the level-triggered re-fire the accept re-arm
 * loop depends on.  IORING_OP_POLL_ADD has no EPOLLEXCLUSIVE analogue, so the
 * cross-worker thundering herd is accepted for Stage 1.
 */

static void
nxt_io_uring_enable_accept(nxt_event_engine_t *engine, nxt_fd_event_t *ev)
{
    nxt_io_uring_slot_t  *slot;

    ev->read = NXT_EVENT_ACTIVE;

    slot = nxt_io_uring_slot(engine, ev->fd);
    if (nxt_slow_path(slot == NULL)) {
        nxt_io_uring_arm_failed(engine, ev);
        return;
    }

    if (!slot->read_armed
        && nxt_slow_path(!nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ, 1)))
    {
        nxt_io_uring_arm_failed(engine, ev);
    }
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

    if (ret < 0 && ret != -ETIME && ret != -EINTR && ret != -EAGAIN) {
        nxt_alert(&engine->task, "io_uring_submit_and_wait_timeout() failed %E",
                  -ret);
        return;
    }

    count = 0;

    io_uring_for_each_cqe(&iou->ring, head, cqe) {
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
    uint16_t               gen;
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

    /* Reject completions for a stale (disabled/closed) arming. */
    if (dir == NXT_IOU_DIR_READ) {
        if (gen != slot->read_generation) {
            return;
        }

    } else {
        if (gen != slot->write_generation) {
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
        /* -ECANCELED/-EBADF/-ENOENT after a remove or close: fd is gone. */
        if (res == -ECANCELED || res == -EBADF || res == -ENOENT) {
            return;
        }

        ev->error = -res;

        nxt_work_queue_add(&engine->fast_work_queue, ev->error_handler,
                           ev->task, ev, ev->data);
        return;
    }

    mask = (uint32_t) res;

    if (dir == NXT_IOU_DIR_READ) {

        if (mask & NXT_IOU_READ_MASK) {
            ev->read_ready = 1;

            if (ev->read == NXT_EVENT_BLOCKED) {
                /* Level-style: disarm to avoid a busy-loop of BLOCKED CQEs. */
                nxt_io_uring_disable_read(engine, ev);
                return;
            }

            if (ev->read == NXT_EVENT_ONESHOT) {
                ev->read = NXT_EVENT_DISABLED;
            }

            if (ev->read != NXT_EVENT_INACTIVE) {
                nxt_work_queue_add(ev->read_work_queue, ev->read_handler,
                                   ev->task, ev, ev->data);
            }
        }

        /*
         * A terminated multishot (!F_MORE) on a still-armed direction is
         * re-issued here so no readiness is missed -- including for events
         * such as the signalfd whose handler never calls enable_read.  A
         * oneshot (now DISABLED) or a disabled direction is left alone.
         */
        if (!more
            && ev->read != NXT_EVENT_INACTIVE
            && ev->read != NXT_EVENT_DISABLED)
        {
            nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_READ, 1);
        }

    } else {

        if (mask & NXT_IOU_WRITE_MASK) {
            ev->write_ready = 1;

            if (ev->write == NXT_EVENT_BLOCKED) {
                nxt_io_uring_disable_write(engine, ev);
                return;
            }

            if (ev->write == NXT_EVENT_ONESHOT) {
                ev->write = NXT_EVENT_DISABLED;
            }

            if (ev->write != NXT_EVENT_INACTIVE) {
                nxt_work_queue_add(ev->write_work_queue, ev->write_handler,
                                   ev->task, ev, ev->data);
            }
        }

        if (!more
            && ev->write != NXT_EVENT_INACTIVE
            && ev->write != NXT_EVENT_DISABLED)
        {
            nxt_io_uring_arm(engine, ev, NXT_IOU_DIR_WRITE, 1);
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
