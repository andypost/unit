#define _GNU_SOURCE   /* accept4() */
/*
 * Spike: HTTP/2 over TLS with nghttp2 driven from a single-threaded epoll
 * loop, the way FreeUnit's event engine would drive it.
 *
 * Model under test (maps onto FreeUnit):
 *   - one non-blocking TCP socket per connection registered in epoll
 *     (nxt_conn_t / nxt_fd_event_t, edge-triggered like nxt_epoll_edge_engine)
 *   - OpenSSL SSL object with SSL_set_fd, ALPN selection callback on the
 *     SSL_CTX (nxt_openssl_server_init would add SSL_CTX_set_alpn_select_cb)
 *   - after the handshake, SSL_get0_alpn_selected() decides h2 vs h1
 *     (nxt_h1p_conn_proto_init would branch here)
 *   - nghttp2 session fed by nghttp2_session_mem_recv() from the SSL_read
 *     buffer (nxt_openssl_conn_io_recvbuf) and drained with
 *     nghttp2_session_mem_send() into SSL_write (nxt_openssl_conn_io_send);
 *     WANT_READ / WANT_WRITE toggle epoll interest like
 *     nxt_openssl_conn_test_error does
 *   - one "request" object per h2 stream, sharing the connection
 *     (nxt_http_request_t per stream, r->proto.h2 -> stream, stream->h2c)
 *   - the response body is produced by an nghttp2_data_provider that reads
 *     from a per-stream buffer chain and returns NGHTTP2_ERR_DEFERRED when
 *     empty, resumed with nghttp2_session_resume_data() when the
 *     application (here: a timer) produces more -- this is the shape of
 *     nxt_h2p_request_send()
 *   - an idle timeout computed into the epoll_wait() timeout, standing in
 *     for nxt_timer_t
 *
 * Build:
 *   cc -O1 -g -Wall -o h2_epoll_server h2_epoll_server.c -lssl -lcrypto -lnghttp2
 * Run:
 *   ./h2_epoll_server cert.pem key.pem 8443
 * Test:
 *   curl -k --http2 -v https://127.0.0.1:8443/hello
 *   nghttp -nv https://127.0.0.1:8443/a https://127.0.0.1:8443/b
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <nghttp2/nghttp2.h>

#define MAX_EVENTS     64
#define IDLE_TIMEOUT_MS 15000
#define STREAM_CHUNKS   3

typedef struct h2_conn_s   h2_conn_t;
typedef struct h2_stream_s h2_stream_t;

/* The per-stream object: what nxt_http_request_t + r->proto.h2 would be. */
struct h2_stream_s {
    h2_conn_t        *h2c;
    int32_t          stream_id;
    char             method[16];
    char             path[256];
    size_t           body_received;
    /* "response body producer": chunks left, filled by the timer tick */
    int              chunks_left;
    char             pending[256];
    size_t           pending_len;
    int              deferred;      /* data provider returned DEFERRED */
    int              eof;
    h2_stream_t      *next;
};

struct h2_conn_s {
    int              fd;
    SSL              *ssl;
    nghttp2_session  *session;
    int              handshaked;
    int              is_h2;
    int              want_write;    /* epoll interest currently includes OUT */
    int              closing;
    long long        last_activity_ms;
    h2_stream_t      *streams;
    unsigned char    outbuf[64 * 1024];
    size_t           outlen, outpos; /* bytes from mem_send not yet written */
    h2_conn_t        *next_dead;
};

static int        epfd;
static h2_conn_t  *dead;
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void conn_set_interest(h2_conn_t *c, int want_write)
{
    struct epoll_event ev;

    if (c->want_write == want_write) {
        return;
    }

    c->want_write = want_write;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | (want_write ? EPOLLOUT : 0);
    ev.data.ptr = c;
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static void conn_close(h2_conn_t *c)
{
    h2_stream_t *s, *next;

    if (c->closing) {
        return;
    }
    c->closing = 1;

    fprintf(stderr, "[conn %d] close\n", c->fd);

    for (s = c->streams; s != NULL; s = next) {
        next = s->next;
        free(s);
    }

    if (c->session != NULL) {
        nghttp2_session_del(c->session);
    }
    if (c->ssl != NULL) {
        SSL_set_quiet_shutdown(c->ssl, 1);
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    /* deferred free, like nxt_conn_recycle_pending(): the caller may still
     * hold the pointer until the end of this loop iteration */
    c->next_dead = dead;
    dead = c;
}

/* ---- ALPN: what nxt_openssl_server_init() would add to the SSL_CTX ---- */

static const unsigned char alpn_protos[] = "\x02h2\x08http/1.1";

static int
alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
    const unsigned char *in, unsigned int inlen, void *arg)
{
    (void) ssl; (void) arg;

    if (SSL_select_next_proto((unsigned char **) out, outlen, alpn_protos,
                              sizeof(alpn_protos) - 1, in, inlen)
        != OPENSSL_NPN_NEGOTIATED)
    {
        return SSL_TLSEXT_ERR_NOACK;
    }

    return SSL_TLSEXT_ERR_OK;
}

/* ---- nghttp2 callbacks: the h2 equivalent of nxt_h1p_* parsing ---- */


static int
on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame,
    void *user_data)
{
    h2_conn_t   *c = user_data;
    h2_stream_t *s;

    (void) session;

    if (frame->hd.type != NGHTTP2_HEADERS
        || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    {
        return 0;
    }

    /* nxt_http_request_create() per stream. */
    s = calloc(1, sizeof(h2_stream_t));
    s->h2c = c;
    s->stream_id = frame->hd.stream_id;
    s->chunks_left = STREAM_CHUNKS;
    s->next = c->streams;
    c->streams = s;

    nghttp2_session_set_stream_user_data(session, s->stream_id, s);

    fprintf(stderr, "[conn %d] stream %d begin\n", c->fd, s->stream_id);
    return 0;
}

static int
on_header(nghttp2_session *session, const nghttp2_frame *frame,
    const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen,
    uint8_t flags, void *user_data)
{
    h2_stream_t *s;

    (void) flags; (void) user_data;

    if (frame->hd.type != NGHTTP2_HEADERS) {
        return 0;
    }

    s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (s == NULL) {
        return 0;
    }

    /*
     * HPACK hands us one decoded (name, value) at a time, already lower-case,
     * pseudo-headers first.  In FreeUnit this is where an nxt_http_field_t is
     * appended to r->fields (nxt_http_req_field_add) with name/value copied
     * into r->mem_pool, and ":method"/":path"/":authority"/":scheme" are
     * routed to r->method/r->target/r->host instead of the field list.
     */
    if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
        snprintf(s->method, sizeof(s->method), "%.*s", (int) valuelen, value);
    } else if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
        snprintf(s->path, sizeof(s->path), "%.*s", (int) valuelen, value);
    }

    return 0;
}

static int
on_data_chunk_recv(nghttp2_session *session, uint8_t flags, int32_t stream_id,
    const uint8_t *data, size_t len, void *user_data)
{
    h2_conn_t   *c = user_data;
    h2_stream_t *s;

    (void) flags; (void) data;

    s = nghttp2_session_get_stream_user_data(session, stream_id);
    if (s != NULL) {
        s->body_received += len;
    }

    /*
     * Flow control: with automatic window update disabled we would call
     * nghttp2_session_consume() only once the router has actually consumed
     * the bytes (moved them into r->body / the temp file).  Here we consume
     * immediately, which is what nghttp2's default auto-update does anyway.
     */
    nghttp2_session_consume(session, stream_id, len);
    (void) c;
    return 0;
}

static ssize_t
body_read_cb(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
    size_t length, uint32_t *data_flags, nghttp2_data_source *source,
    void *user_data)
{
    h2_stream_t *s = source->ptr;
    size_t       n;

    (void) session; (void) stream_id; (void) user_data;

    if (s->pending_len == 0) {
        if (s->eof) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        /* Nothing produced yet: park the stream (nxt_h2p_request_send
         * resumes it when the application sends the next buffer). */
        s->deferred = 1;
        return NGHTTP2_ERR_DEFERRED;
    }

    n = s->pending_len < length ? s->pending_len : length;
    memcpy(buf, s->pending, n);
    memmove(s->pending, s->pending + n, s->pending_len - n);
    s->pending_len -= n;

    if (s->pending_len == 0 && s->eof) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (ssize_t) n;
}

static int
stream_submit_response(h2_conn_t *c, h2_stream_t *s)
{
    nghttp2_data_provider dp;
    nghttp2_nv            nva[3];
    int                   rv;

    /* nxt_h2p_request_header_send(): r->resp fields -> nghttp2_nv[] */
    nva[0].name = (uint8_t *) ":status";  nva[0].namelen = 7;
    nva[0].value = (uint8_t *) "200";     nva[0].valuelen = 3;
    nva[0].flags = NGHTTP2_NV_FLAG_NONE;
    nva[1].name = (uint8_t *) "content-type"; nva[1].namelen = 12;
    nva[1].value = (uint8_t *) "text/plain";  nva[1].valuelen = 10;
    nva[1].flags = NGHTTP2_NV_FLAG_NONE;
    nva[2].name = (uint8_t *) "server";   nva[2].namelen = 6;
    nva[2].value = (uint8_t *) "freeunit-h2-spike"; nva[2].valuelen = 17;
    nva[2].flags = NGHTTP2_NV_FLAG_NONE;

    dp.source.ptr = s;
    dp.read_callback = body_read_cb;

    rv = nghttp2_submit_response(c->session, s->stream_id, nva, 3, &dp);
    if (rv != 0) {
        fprintf(stderr, "submit_response: %s\n", nghttp2_strerror(rv));
        return -1;
    }

    return 0;
}

static int
on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame,
    void *user_data)
{
    h2_conn_t   *c = user_data;
    h2_stream_t *s;

    switch (frame->hd.type) {
    case NGHTTP2_HEADERS:
    case NGHTTP2_DATA:
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0) {
            return 0;
        }

        s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
        if (s == NULL) {
            return 0;
        }

        /* END_STREAM on the request side == body complete: in FreeUnit the
         * request state ready_handler runs here (routing, application). */
        fprintf(stderr, "[conn %d] stream %d request %s %s body=%zu\n",
                c->fd, s->stream_id, s->method, s->path, s->body_received);

        return stream_submit_response(c, s);

    default:
        return 0;
    }
}

static int
on_stream_close(nghttp2_session *session, int32_t stream_id,
    uint32_t error_code, void *user_data)
{
    h2_conn_t    *c = user_data;
    h2_stream_t  *s, **prev;

    (void) session;

    for (prev = &c->streams; (s = *prev) != NULL; prev = &s->next) {
        if (s->stream_id == stream_id) {
            *prev = s->next;
            fprintf(stderr, "[conn %d] stream %d close err=%u\n",
                    c->fd, stream_id, error_code);
            free(s);   /* nxt_http_request_close_handler: pool release */
            break;
        }
    }

    return 0;
}

/* ---- I/O plumbing: what nxt_conn_io_read / nxt_conn_io_write + the
 *      OpenSSL io vtable do ---- */

static int
conn_flush(h2_conn_t *c)
{
    int          ret, err;
    ssize_t      n;
    const uint8_t *data;

    for ( ;; ) {
        if (c->outpos == c->outlen) {
            c->outpos = c->outlen = 0;

            n = nghttp2_session_mem_send(c->session, &data);
            if (n < 0) {
                fprintf(stderr, "mem_send: %s\n", nghttp2_strerror((int) n));
                return -1;
            }
            if (n == 0) {
                conn_set_interest(c, 0);
                return 0;
            }

            if ((size_t) n > sizeof(c->outbuf)) {
                n = sizeof(c->outbuf);   /* nghttp2 frames are <= 16K+9 */
            }
            memcpy(c->outbuf, data, n);
            c->outlen = n;
        }

        ret = SSL_write(c->ssl, c->outbuf + c->outpos, c->outlen - c->outpos);
        if (ret > 0) {
            c->outpos += ret;
            continue;
        }

        err = SSL_get_error(c->ssl, ret);
        if (err == SSL_ERROR_WANT_WRITE) {
            conn_set_interest(c, 1);
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            /* renegotiation-ish: wait for readability, keep the bytes */
            conn_set_interest(c, 0);
            return 0;
        }
        return -1;
    }
}

static int
conn_read(h2_conn_t *c)
{
    unsigned char buf[16 * 1024];
    int           ret, err;
    ssize_t       n;

    for ( ;; ) {
        ret = SSL_read(c->ssl, buf, sizeof(buf));
        if (ret > 0) {
            c->last_activity_ms = now_ms();
            n = nghttp2_session_mem_recv(c->session, buf, ret);
            if (n < 0) {
                fprintf(stderr, "mem_recv: %s\n", nghttp2_strerror((int) n));
                return -1;
            }
            continue;
        }

        err = SSL_get_error(c->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            return 0;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            conn_set_interest(c, 1);
            return 0;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return -1;   /* close_notify */
        }
        return -1;
    }
}

static int
conn_h2_init(h2_conn_t *c)
{
    nghttp2_session_callbacks *cbs;
    nghttp2_settings_entry     iv[2];
    int                        rv;

    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close);

    rv = nghttp2_session_server_new(&c->session, cbs, c);
    nghttp2_session_callbacks_del(cbs);
    if (rv != 0) {
        return -1;
    }

    iv[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    iv[0].value = 100;
    iv[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
    iv[1].value = 64 * 1024;   /* would follow body_buffer_size */

    return nghttp2_submit_settings(c->session, NGHTTP2_FLAG_NONE, iv, 2);
}

static int
conn_handshake(h2_conn_t *c)
{
    int                  ret, err;
    const unsigned char  *alpn;
    unsigned int         alpnlen;

    ret = SSL_do_handshake(c->ssl);
    if (ret == 1) {
        c->handshaked = 1;
        c->last_activity_ms = now_ms();

        SSL_get0_alpn_selected(c->ssl, &alpn, &alpnlen);
        c->is_h2 = (alpnlen == 2 && memcmp(alpn, "h2", 2) == 0);

        fprintf(stderr, "[conn %d] handshake ok, alpn=%.*s -> %s\n", c->fd,
                (int) alpnlen, alpn ? (const char *) alpn : "",
                c->is_h2 ? "h2" : "h1 (not served by this spike)");

        if (!c->is_h2) {
            return -1;
        }

        if (conn_h2_init(c) != 0) {
            return -1;
        }

        conn_set_interest(c, 0);
        return conn_flush(c);   /* SETTINGS go out now */
    }

    err = SSL_get_error(c->ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        conn_set_interest(c, 0);
        return 0;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        conn_set_interest(c, 1);
        return 0;
    }

    ERR_print_errors_fp(stderr);
    return -1;
}

static void
conn_event(h2_conn_t *c, uint32_t events)
{
    if (events & (EPOLLERR | EPOLLHUP)) {
        conn_close(c);
        return;
    }

    if (!c->handshaked) {
        if (conn_handshake(c) < 0) {
            conn_close(c);
        }
        return;
    }

    if (events & (EPOLLIN | EPOLLRDHUP)) {
        if (conn_read(c) < 0) {
            conn_close(c);
            return;
        }
    }

    if (conn_flush(c) < 0) {
        conn_close(c);
        return;
    }

    if (!nghttp2_session_want_read(c->session)
        && !nghttp2_session_want_write(c->session))
    {
        conn_close(c);
    }
}

/*
 * Timer tick: stands in for the application producing response buffers
 * asynchronously (nxt_http_request_send from the app port handler), then
 * nxt_h2p_request_send() appending to the stream and resuming the deferred
 * data provider.
 */
static void
conn_tick(h2_conn_t *c)
{
    h2_stream_t *s;
    int          produced = 0;

    for (s = c->streams; s != NULL; s = s->next) {
        if (s->chunks_left > 0 && s->pending_len == 0) {
            s->pending_len = snprintf(s->pending, sizeof(s->pending),
                                      "stream %d chunk %d for %s\n",
                                      s->stream_id,
                                      STREAM_CHUNKS - s->chunks_left + 1,
                                      s->path);
            s->chunks_left--;
            if (s->chunks_left == 0) {
                s->eof = 1;
            }
            if (s->deferred) {
                s->deferred = 0;
                nghttp2_session_resume_data(c->session, s->stream_id);
            }
            produced = 1;
        }
    }

    if (produced && conn_flush(c) < 0) {
        conn_close(c);
    }
}

int
main(int argc, char **argv)
{
    int                 lfd, one = 1, port, nev, i;
    SSL_CTX             *ctx;
    struct sockaddr_in  sin;
    struct epoll_event  ev, events[MAX_EVENTS];
    h2_conn_t           *conns[FD_SETSIZE] = { 0 };
    long long           now, next_tick;

    if (argc < 4) {
        fprintf(stderr, "usage: %s cert.pem key.pem port\n", argv[0]);
        return 1;
    }
    port = atoi(argv[3]);
    signal(SIGPIPE, SIG_IGN);

    ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE
                          | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
                          | SSL_MODE_RELEASE_BUFFERS);
    if (SSL_CTX_use_certificate_chain_file(ctx, argv[1]) != 1
        || SSL_CTX_use_PrivateKey_file(ctx, argv[2], SSL_FILETYPE_PEM) != 1)
    {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    /* RFC 9113 9.2.2 needs a TLS 1.2 cipher black list; TLS 1.3 is fine. */
    SSL_CTX_set_cipher_list(ctx, "ECDHE+AESGCM:ECDHE+CHACHA20");
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *) &sin, sizeof(sin)) != 0
        || listen(lfd, 128) != 0)
    {
        perror("bind/listen");
        return 1;
    }
    set_nonblock(lfd);

    epfd = epoll_create1(0);
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    fprintf(stderr, "h2 spike listening on 127.0.0.1:%d\n", port);

    next_tick = now_ms() + 100;

    for ( ;; ) {
        now = now_ms();
        /* nxt_timer_find(): nearest timer bounds the poll */
        nev = epoll_wait(epfd, events, MAX_EVENTS,
                         next_tick > now ? (int) (next_tick - now) : 0);
        if (nev < 0 && errno != EINTR) {
            perror("epoll_wait");
            return 1;
        }

        for (i = 0; i < nev; i++) {
            if (events[i].data.ptr == NULL) {
                for ( ;; ) {
                    int        cfd;
                    h2_conn_t  *c;

                    cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
                    if (cfd < 0) {
                        break;
                    }
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                    c = calloc(1, sizeof(h2_conn_t));
                    c->fd = cfd;
                    c->ssl = SSL_new(ctx);
                    SSL_set_fd(c->ssl, cfd);
                    SSL_set_accept_state(c->ssl);
                    c->last_activity_ms = now_ms();
                    c->want_write = -1;
                    ev.events = 0;
                    ev.data.ptr = c;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
                    conn_set_interest(c, 0);
                    if (cfd < FD_SETSIZE) {
                        conns[cfd] = c;
                    }
                    fprintf(stderr, "[conn %d] accepted\n", cfd);
                    conn_handshake(c);
                }
                continue;
            }

            h2_conn_t *c = events[i].data.ptr;
            int        fd = c->fd;

            conn_event(c, events[i].events);
            if (fd < FD_SETSIZE && c->closing) {
                conns[fd] = NULL;
            }
        }

        while (dead != NULL) {
            h2_conn_t *d = dead;
            dead = d->next_dead;
            free(d);
        }

        /* nxt_timer_expire() */
        now = now_ms();
        if (now >= next_tick) {
            next_tick = now + 100;
            for (i = 0; i < FD_SETSIZE; i++) {
                if (conns[i] == NULL || !conns[i]->handshaked) {
                    continue;
                }
                if (now - conns[i]->last_activity_ms > IDLE_TIMEOUT_MS
                    && conns[i]->streams == NULL)
                {
                    fprintf(stderr, "[conn %d] idle timeout\n", conns[i]->fd);
                    conn_close(conns[i]);
                    conns[i] = NULL;
                    continue;
                }
                conn_tick(conns[i]);
                if (conns[i]->closing) {
                    conns[i] = NULL;
                }
            }
        }
    }
}
