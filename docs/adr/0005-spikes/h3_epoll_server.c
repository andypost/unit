/*
 * Spike: HTTP/3 with quiche's C FFI driven from a single-threaded epoll
 * loop over ONE non-blocking UDP socket, the "bring your own socket and
 * timer" model shared by quiche and ngtcp2 (and by OpenSSL 3.5's QUIC
 * server API in non-blocking mode, except that OpenSSL owns the socket BIO).
 *
 * Model under test (maps onto FreeUnit):
 *   - the UDP listener is ONE nxt_fd_event_t per engine (EPOLLIN|EPOLLET)
 *   - QUIC connections are NOT nxt_conn_t: they have no fd.  They live in a
 *     table keyed by connection ID (here a list; FreeUnit would use
 *     nxt_lvlhsh_t) hung off the listener event.
 *   - every datagram: quiche_header_info() -> DCID -> lookup -> quiche_conn_recv()
 *   - after each recv and each timer: quiche_conn_send() loop -> sendto()
 *   - the per-connection timer is quiche_conn_timeout_as_millis(); the loop
 *     takes the minimum into epoll_wait(), which is exactly nxt_timer_find()
 *     over one nxt_timer_t per QUIC connection.
 *   - HTTP/3 request = quiche_h3 HEADERS+FINISHED events on a stream; that is
 *     where nxt_http_request_t would be created with r->proto.h3 = stream.
 *
 * Build (after `cargo build --release --features ffi` of quiche 0.30):
 *   cc -O1 -g -Wall -I$QUICHE/include -o h3_epoll_server h3_epoll_server.c \
 *      $QUICHE/target/release/libquiche.a -lm -lpthread -ldl
 * Run:
 *   ./h3_epoll_server cert.pem key.pem 4433
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <quiche.h>

#define LOCAL_CONN_ID_LEN 16
#define MAX_DATAGRAM_SIZE 1350

typedef struct h3_conn_s h3_conn_t;

struct h3_conn_s {
    uint8_t                  scid[QUICHE_MAX_CONN_ID_LEN];
    size_t                   scid_len;
    uint8_t                  odcid[QUICHE_MAX_CONN_ID_LEN];
    size_t                   odcid_len;
    struct sockaddr_storage  peer;
    socklen_t                peer_len;
    quiche_conn              *q;
    quiche_h3_conn           *h3;
    h3_conn_t                *next;
};

static int                     sock;
static struct sockaddr_storage local_addr;
static socklen_t               local_len;
static quiche_config           *config;
static quiche_h3_config        *h3config;
static h3_conn_t               *conns;

static void
debug_log(const char *line, void *argp)
{
    (void) argp;
    fprintf(stderr, "quiche: %s\n", line);
}

static h3_conn_t *
conn_lookup(const uint8_t *dcid, size_t dcid_len)
{
    h3_conn_t *c;

    for (c = conns; c != NULL; c = c->next) {
        if ((dcid_len == c->scid_len && memcmp(dcid, c->scid, dcid_len) == 0)
            || (dcid_len == c->odcid_len
                && memcmp(dcid, c->odcid, dcid_len) == 0))
        {
            return c;
        }
    }
    return NULL;
}

/* nxt_h3p_conn_send(): drain the engine's packets into the UDP socket. */
static void
conn_flush(h3_conn_t *c)
{
    static uint8_t   out[MAX_DATAGRAM_SIZE];
    ssize_t          n, sent;
    quiche_send_info si;

    for ( ;; ) {
        n = quiche_conn_send(c->q, out, sizeof(out), &si);
        if (n == QUICHE_ERR_DONE) {
            return;
        }
        if (n < 0) {
            fprintf(stderr, "quiche_conn_send: %zd\n", n);
            return;
        }
        /* GSO would batch quiche_conn_send_quantum() bytes here. */
        sent = sendto(sock, out, n, 0, (struct sockaddr *) &si.to, si.to_len);
        if (sent != n) {
            if (errno == EAGAIN) {
                /* FreeUnit: enable EPOLLOUT on the listener and retry. */
                return;
            }
            perror("sendto");
            return;
        }
    }
}

static int
header_cb(uint8_t *name, size_t name_len, uint8_t *value, size_t value_len,
    void *argp)
{
    char *path = argp;

    /* QPACK-decoded, lower-case; -> nxt_http_req_field_add() per field,
     * pseudo-headers -> r->method / r->target / r->host. */
    if (name_len == 5 && memcmp(name, ":path", 5) == 0) {
        snprintf(path, 256, "%.*s", (int) value_len, value);
    }
    return 0;
}

static void
conn_h3_events(h3_conn_t *c)
{
    int64_t          sid;
    quiche_h3_event  *ev;
    char             path[256] = "?";
    char             body[256];
    ssize_t          n;
    uint8_t          buf[4096];

    if (c->h3 == NULL) {
        if (!quiche_conn_is_established(c->q)) {
            return;
        }
        c->h3 = quiche_h3_conn_new_with_transport(c->q, h3config);
        if (c->h3 == NULL) {
            fprintf(stderr, "h3 conn new failed\n");
            return;
        }
    }

    for ( ;; ) {
        sid = quiche_h3_conn_poll(c->h3, c->q, &ev);
        if (sid < 0) {
            break;
        }

        switch (quiche_h3_event_type(ev)) {
        case QUICHE_H3_EVENT_HEADERS:
            quiche_h3_event_for_each_header(ev, header_cb, path);
            fprintf(stderr, "[h3] stream %ld headers, path=%s\n",
                    (long) sid, path);
            break;

        case QUICHE_H3_EVENT_DATA:
            /* request body: -> r->body, mirroring nxt_h1p_request_body_read */
            while ((n = quiche_h3_recv_body(c->h3, c->q, sid, buf, sizeof(buf)))
                   > 0)
            {
                fprintf(stderr, "[h3] stream %ld body %zd bytes\n",
                        (long) sid, n);
            }
            break;

        case QUICHE_H3_EVENT_FINISHED: {
            quiche_h3_header hdrs[3] = {
                { (uint8_t *) ":status", 7, (uint8_t *) "200", 3 },
                { (uint8_t *) "server", 6, (uint8_t *) "freeunit-h3-spike", 17 },
                { (uint8_t *) "content-type", 12, (uint8_t *) "text/plain", 10 },
            };

            fprintf(stderr, "[h3] stream %ld finished -> respond\n", (long) sid);

            n = snprintf(body, sizeof(body), "hello over h3, stream %ld\n",
                         (long) sid);

            /*
             * Both calls fail with QUICHE_ERR_STREAM_BLOCKED / DONE when the
             * stream has no send capacity; FreeUnit would then park the
             * response chain and resume it from the stream-writable event
             * (quiche_conn_stream_writable / nghttp3 unblock), the same
             * "deferred provider" shape as the h2 spike.  The spike only
             * reports it.
             */
            if (quiche_h3_send_response(c->h3, c->q, sid, hdrs, 3, false) < 0) {
                fprintf(stderr, "[h3] stream %ld send_response failed\n",
                        (long) sid);
                break;
            }
            if (quiche_h3_send_body(c->h3, c->q, sid, (uint8_t *) body, n, true)
                != n)
            {
                fprintf(stderr, "[h3] stream %ld send_body short/blocked\n",
                        (long) sid);
            }
            break;
        }

        default:
            break;
        }

        quiche_h3_event_free(ev);
    }
}

static void
listener_readable(void)
{
    static uint8_t   buf[65535];
    static uint8_t   out[MAX_DATAGRAM_SIZE];
    ssize_t          n;
    struct sockaddr_storage peer;
    socklen_t        peer_len;
    uint32_t         version;
    uint8_t          type;
    uint8_t          scid[QUICHE_MAX_CONN_ID_LEN], dcid[QUICHE_MAX_CONN_ID_LEN];
    uint8_t          token[256];
    size_t           scid_len, dcid_len, token_len;
    h3_conn_t        *c;
    quiche_recv_info ri;

    for ( ;; ) {
        peer_len = sizeof(peer);
        n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *) &peer,
                     &peer_len);
        if (n < 0) {
            if (errno != EAGAIN) {
                perror("recvfrom");
            }
            return;
        }

        scid_len = dcid_len = sizeof(scid);
        token_len = sizeof(token);

        if (quiche_header_info(buf, n, LOCAL_CONN_ID_LEN, &version, &type,
                               scid, &scid_len, dcid, &dcid_len,
                               token, &token_len) < 0)
        {
            fprintf(stderr, "bad packet header\n");
            continue;
        }

        c = conn_lookup(dcid, dcid_len);

        if (c == NULL) {
            if (!quiche_version_is_supported(version)) {
                ssize_t w = quiche_negotiate_version(scid, scid_len, dcid,
                                                     dcid_len, out, sizeof(out));
                if (w > 0) {
                    sendto(sock, out, w, 0, (struct sockaddr *) &peer, peer_len);
                }
                continue;
            }

            /* No Retry in the spike; FreeUnit would add address validation. */
            c = calloc(1, sizeof(h3_conn_t));
            c->scid_len = LOCAL_CONN_ID_LEN;
            if (getrandom(c->scid, c->scid_len, 0) != (ssize_t) c->scid_len) {
                perror("getrandom");
                free(c);
                continue;
            }
            memcpy(c->odcid, dcid, dcid_len);
            c->odcid_len = dcid_len;
            memcpy(&c->peer, &peer, peer_len);
            c->peer_len = peer_len;

            c->q = quiche_accept(c->scid, c->scid_len, NULL, 0,
                                 (struct sockaddr *) &local_addr, local_len,
                                 (struct sockaddr *) &peer, peer_len, config);
            if (c->q == NULL) {
                fprintf(stderr, "quiche_accept failed\n");
                free(c);
                continue;
            }

            c->next = conns;
            conns = c;
            fprintf(stderr, "[quic] new connection\n");
        }

        ri.from = (struct sockaddr *) &peer;
        ri.from_len = peer_len;
        ri.to = (struct sockaddr *) &local_addr;
        ri.to_len = local_len;

        n = quiche_conn_recv(c->q, buf, n, &ri);
        if (n < 0) {
            fprintf(stderr, "quiche_conn_recv: %zd\n", n);
            continue;
        }

        conn_h3_events(c);
        conn_flush(c);
    }
}

static int
timers_min_ms(void)
{
    h3_conn_t  *c;
    uint64_t   t, min = UINT64_MAX;

    for (c = conns; c != NULL; c = c->next) {
        t = quiche_conn_timeout_as_millis(c->q);
        if (t < min) {
            min = t;
        }
    }
    return min == UINT64_MAX ? -1 : (int) min;
}

static void
timers_expire(void)
{
    h3_conn_t  *c, **prev;
    quiche_stats st;

    for (prev = &conns; (c = *prev) != NULL; ) {
        if (quiche_conn_timeout_as_millis(c->q) == 0) {
            quiche_conn_on_timeout(c->q);   /* loss detection / idle */
            conn_flush(c);
        }

        if (quiche_conn_is_closed(c->q)) {
            quiche_conn_stats(c->q, &st);
            fprintf(stderr, "[quic] connection closed, recv=%zu sent=%zu lost=%zu\n",
                    st.recv, st.sent, st.lost);
            *prev = c->next;
            if (c->h3 != NULL) {
                quiche_h3_conn_free(c->h3);
            }
            quiche_conn_free(c->q);
            free(c);
            continue;
        }
        prev = &c->next;
    }
}

int
main(int argc, char **argv)
{
    int                 epfd, nev, port, i;
    struct sockaddr_in  *sin;
    struct epoll_event  ev, events[8];
    const uint8_t       alpn[] = "\x02h3";

    if (argc < 4) {
        fprintf(stderr, "usage: %s cert.pem key.pem port\n", argv[0]);
        return 1;
    }
    port = atoi(argv[3]);

    if (getenv("QUICHE_DEBUG") != NULL) {
        quiche_enable_debug_logging(debug_log, NULL);
    }

    config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    quiche_config_load_cert_chain_from_pem_file(config, argv[1]);
    quiche_config_load_priv_key_from_pem_file(config, argv[2]);
    quiche_config_set_application_protos(config, alpn, sizeof(alpn) - 1);
    quiche_config_set_max_idle_timeout(config, 5000);
    quiche_config_set_max_recv_udp_payload_size(config, MAX_DATAGRAM_SIZE);
    quiche_config_set_max_send_udp_payload_size(config, MAX_DATAGRAM_SIZE);
    quiche_config_set_initial_max_data(config, 10000000);
    quiche_config_set_initial_max_stream_data_bidi_local(config, 1000000);
    quiche_config_set_initial_max_stream_data_bidi_remote(config, 1000000);
    quiche_config_set_initial_max_stream_data_uni(config, 1000000);
    quiche_config_set_initial_max_streams_bidi(config, 100);
    quiche_config_set_initial_max_streams_uni(config, 100);
    quiche_config_set_disable_active_migration(config, true);

    h3config = quiche_h3_config_new();

    sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sin = (struct sockaddr_in *) &local_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin->sin_port = htons(port);
    local_len = sizeof(struct sockaddr_in);
    if (bind(sock, (struct sockaddr *) &local_addr, local_len) != 0) {
        perror("bind");
        return 1;
    }

    epfd = epoll_create1(0);
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = sock;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);

    fprintf(stderr, "h3 spike listening on udp 127.0.0.1:%d\n", port);

    for ( ;; ) {
        nev = epoll_wait(epfd, events, 8, timers_min_ms());
        if (nev < 0 && errno != EINTR) {
            perror("epoll_wait");
            return 1;
        }
        for (i = 0; i < nev; i++) {
            if (events[i].data.fd == sock) {
                listener_readable();
            }
        }
        timers_expire();
    }
}
