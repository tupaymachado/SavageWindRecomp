/* savewind: minimal HTTP/1.1 client over BSD sockets (GPL-3.0-or-later).
 * One request per connection (Connection: close); the whole response is read to EOF and
 * then parsed (Content-Length or chunked). No Sony HTTP/SSL libraries: https:// goes
 * through BearSSL (sw_tls.c) when built with SW_TLS. */
#include "sw.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RESP_MAX (80u * 1024u * 1024u)
#define USER_AGENT "savewind-ps5/1"

/* transport: plain TCP, or TLS on top of it */
typedef struct sw_conn {
    int fd;
    void *tls;   /* sw_tls state when https */
} sw_conn;

#ifdef SW_TLS
int  sw_tls_open(sw_conn *c, const char *host, char *err, size_t ec);
int  sw_tls_write(sw_conn *c, const void *buf, size_t len);   /* 0 ok */
long sw_tls_read(sw_conn *c, void *buf, size_t len);          /* bytes, 0 EOF, <0 error */
void sw_tls_close(sw_conn *c);
int  sw_tls_error(sw_conn *c);
#endif

static void io_err(sw_conn *c, const char *what, char *err, size_t ec) {
#ifdef SW_TLS
    int e = sw_tls_error(c);
    if (e) {
        snprintf(err, ec, "%s: TLS error %d%s", what, e, e >= 32 ? " (certificate not trusted)" : "");
        return;
    }
#endif
    (void)c;
    snprintf(err, ec, "%s failed (errno %d)", what, errno);
}

static int conn_write(sw_conn *c, const void *buf, size_t len) {
#ifdef SW_TLS
    if (c->tls) return sw_tls_write(c, buf, len);
#endif
    const char *p = buf;
    while (len) {
        ssize_t n = send(c->fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n, len -= (size_t)n;
    }
    return 0;
}

static long conn_read(sw_conn *c, void *buf, size_t len) {
#ifdef SW_TLS
    if (c->tls) return sw_tls_read(c, buf, len);
#endif
    for (;;) {
        ssize_t n = recv(c->fd, buf, len, 0);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

static int split_url(const char *url, int *https, char *host, size_t hc, char *port, size_t pc) {
    const char *p;
    if (!strncmp(url, "https://", 8)) *https = 1, p = url + 8;
    else if (!strncmp(url, "http://", 7)) *https = 0, p = url + 7;
    else return -1;
    size_t hl = strcspn(p, ":/");
    if (!hl || hl >= hc) return -1;
    memcpy(host, p, hl); host[hl] = 0;
    if (p[hl] == ':') snprintf(port, pc, "%.*s", (int)strcspn(p + hl + 1, "/"), p + hl + 1);
    else snprintf(port, pc, "%s", *https ? "443" : "80");
    return 0;
}

static int tcp_connect(const sw_config *cfg, const char *host, const char *port, int timeout_s, char *err, size_t ec) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)atoi(port));
    if (sw_resolve(cfg, host, &sa.sin_addr.s_addr, err, ec)) return -1;
    int r;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, ec, "socket: %d", errno); return -1; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (r && errno != EINPROGRESS) { snprintf(err, ec, "connect: %d", errno); close(fd); return -1; }
    if (r) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
        struct timeval tv = { timeout_s, 0 };
        int soerr = 0; socklen_t sl = sizeof soerr;
        if (select(fd + 1, NULL, &w, NULL, &tv) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) || soerr) {
            snprintf(err, ec, "connect %s:%s: %s", host, port, soerr ? "refused/unreachable" : "timeout");
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl);
    struct timeval tv = { timeout_s, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    return fd;
}

/* memmem is missing from the PS5 libc */
static uint8_t *find(const uint8_t *hay, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; k <= n && i <= n - k; i++)
        if (!memcmp(hay + i, needle, k)) return (uint8_t *)hay + i;
    return NULL;
}

/* decode a chunked body in place; returns new length or -1 */
static long dechunk(uint8_t *b, size_t len) {
    size_t in = 0, out = 0;
    for (;;) {
        char *end;
        if (in >= len) return -1;
        unsigned long n = strtoul((const char *)b + in, &end, 16);
        uint8_t *crlf = find(b + in, len - in, "\r\n");
        if (!crlf || (uint8_t *)end > crlf) return -1;
        in = (size_t)(crlf - b) + 2;
        if (!n) return (long)out;
        if (n > len - in) return -1;
        memmove(b + out, b + in, n);
        out += n, in += n + 2;
    }
}

int sw_http(const sw_config *cfg, const char *method, const char *path, const char *ctype,
            const uint8_t *body, size_t len, sw_resp *resp, char *err, size_t ec) {
    int https; char host[128], port[8];
    memset(resp, 0, sizeof *resp);
    if (split_url(cfg->url, &https, host, sizeof host, port, sizeof port)) { snprintf(err, ec, "bad url"); return -1; }
#ifndef SW_TLS
    if (https) { snprintf(err, ec, "https not built in"); return -1; }
#endif
    int tmo = cfg->timeout_s;
    if (cfg->deadline) {
        long long left = cfg->deadline - (long long)time(NULL);
        if (left <= 0) { snprintf(err, ec, "deadline passed"); return -1; }
        if (left < tmo) tmo = (int)left;
    }
    sw_conn c = { tcp_connect(cfg, host, port, tmo, err, ec), NULL };
    if (c.fd < 0) return -1;
#ifdef SW_TLS
    if (https && sw_tls_open(&c, host, err, ec)) { close(c.fd); return -1; }
#endif
    char hdr[768];
    int hl = snprintf(hdr, sizeof hdr,
        "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nUser-Agent: " USER_AGENT "\r\n"
        "Connection: close\r\nContent-Length: %zu\r\n%s%s%s\r\n",
        method, path, host, cfg->token, len, ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "");
    uint8_t *buf = NULL;
    size_t got = 0, cap = 0;
    int rc = -1;
    if (conn_write(&c, hdr, (size_t)hl) || (len && conn_write(&c, body, len))) {
        io_err(&c, "send", err, ec);
        goto out;
    }
    for (;;) {
        if (cap - got < 16384) {
            cap = cap ? cap * 2 : 65536;
            if (cap > RESP_MAX) { snprintf(err, ec, "response too large"); goto out; }
            uint8_t *nb = realloc(buf, cap + 1);
            if (!nb) { snprintf(err, ec, "out of memory"); goto out; }
            buf = nb;
        }
        if (cfg->deadline && time(NULL) > cfg->deadline) { snprintf(err, ec, "deadline passed"); goto out; }
        long n = conn_read(&c, buf + got, cap - got);
        if (n == 0) break;
        if (n < 0) { io_err(&c, "recv", err, ec); goto out; }
        got += (size_t)n;
    }
    if (!buf || got < 12 || strncmp((char *)buf, "HTTP/1.", 7)) { snprintf(err, ec, "bad response"); goto out; }
    buf[got] = 0;
    uint8_t *eoh = find(buf, got, "\r\n\r\n");
    if (!eoh) { snprintf(err, ec, "bad response headers"); goto out; }
    *eoh = 0;
    resp->status = atoi((char *)buf + 9);
    size_t blen = got - (size_t)(eoh + 4 - buf);
    const char *h = (const char *)buf;
    int chunked = 0;
    long clen = -1;
    for (const char *l = strstr(h, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        if (!strncasecmp(l + 2, "content-length:", 15)) clen = atol(l + 17);
        if (!strncasecmp(l + 2, "transfer-encoding:", 18) && strstr(l + 2, "chunked")) chunked = 1;
    }
    resp->body = malloc(blen + 1);
    if (!resp->body) { snprintf(err, ec, "out of memory"); goto out; }
    memcpy(resp->body, eoh + 4, blen);
    resp->body[blen] = 0;
    if (chunked) {
        long n = dechunk(resp->body, blen);
        if (n < 0) { snprintf(err, ec, "bad chunked body"); sw_resp_free(resp); goto out; }
        blen = (size_t)n;
    } else if (clen >= 0) {
        if ((size_t)clen > blen) { snprintf(err, ec, "truncated body (%zu/%ld)", blen, clen); sw_resp_free(resp); goto out; }
        blen = (size_t)clen;
    }
    resp->len = blen;
    rc = 0;
out:
#ifdef SW_TLS
    if (c.tls) sw_tls_close(&c);
#endif
    close(c.fd);
    free(buf);
    return rc;
}
