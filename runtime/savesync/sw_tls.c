/* savewind: TLS client over BearSSL (GPL-3.0-or-later; BearSSL itself is MIT).
 * Full certificate validation against the embedded Let's Encrypt roots (sw_tls_anchors.h).
 * No Sony SSL/HTTP libraries are involved. */
#ifdef SW_TLS
#include "sw.h"
#include "bearssl.h"
#include "sw_tls_anchors.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct sw_conn { int fd; void *tls; } sw_conn;   /* same layout as in sw_http.c */

typedef struct {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    int fd;
    unsigned char buf[BR_SSL_BUFSIZE_BIDI];
} tls_state;

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n < 0 && errno == EINTR) continue;
        return n <= 0 ? -1 : (int)n;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0 && errno == EINTR) continue;
        return n <= 0 ? -1 : (int)n;
    }
}

int sw_tls_open(sw_conn *c, const char *host, char *err, size_t ec) {
    tls_state *t = calloc(1, sizeof *t);
    if (!t) { snprintf(err, ec, "TLS: out of memory"); return -1; }
    br_ssl_client_init_full(&t->sc, &t->xc, TAs, TAs_NUM);
    br_ssl_engine_set_buffer(&t->sc.eng, t->buf, sizeof t->buf, 1);
    unsigned char seed[32];
    if (sw_entropy(seed, sizeof seed)) { free(t); snprintf(err, ec, "TLS: no entropy"); return -1; }
    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof seed);
    if (!br_ssl_client_reset(&t->sc, host, 0)) {
        snprintf(err, ec, "TLS reset: error %d", br_ssl_engine_last_error(&t->sc.eng));
        free(t);
        return -1;
    }
    t->fd = c->fd;
    br_sslio_init(&t->io, &t->sc.eng, sock_read, &t->fd, sock_write, &t->fd);
    c->tls = t;
    return 0;   /* the handshake runs on the first write; failures show up there */
}

/* BearSSL error after a failed read/write: 0 none; >= 32 certificate (BR_ERR_X509_*) */
int sw_tls_error(sw_conn *c) {
    return c->tls ? br_ssl_engine_last_error(&((tls_state *)c->tls)->sc.eng) : 0;
}

int sw_tls_write(sw_conn *c, const void *buf, size_t len) {
    tls_state *t = c->tls;
    if (br_sslio_write_all(&t->io, buf, len) < 0) return -1;
    return br_sslio_flush(&t->io) < 0 ? -1 : 0;
}

long sw_tls_read(sw_conn *c, void *buf, size_t len) {
    tls_state *t = c->tls;
    int n = br_sslio_read(&t->io, buf, len);
    if (n >= 0) return n;
    /* closed: clean EOF, or the peer dropped TCP after a complete response
       (common with Connection: close); any real error is reported */
    int e = br_ssl_engine_last_error(&t->sc.eng);
    return (e == BR_ERR_OK || e == BR_ERR_IO) ? 0 : -1;
}

void sw_tls_close(sw_conn *c) {
    free(c->tls);
    c->tls = NULL;
}
#endif
