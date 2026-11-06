/* savewind: things the PS5 title's libraries do not offer (GPL-3.0-or-later).
 * The title only has libkernel sockets (no getaddrinfo, arc4random or gmtime_r), so:
 *   sw_resolve  - dotted IPv4, else a tiny DNS A query over UDP
 *   sw_entropy  - RDRAND (x86) mixed with clocks; getentropy on the host
 *   sw_utc      - UTC timestamp formatting */
#include "sw.h"
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ IPv4 / DNS */

static int parse_ipv4(const char *s, uint32_t *out) {   /* network order */
    unsigned a[4]; char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a[0], &a[1], &a[2], &a[3], &tail) != 4) return -1;
    for (int i = 0; i < 4; i++) if (a[i] > 255) return -1;
    *out = htonl(a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3]);
    return 0;
}

static int skip_name(const uint8_t *m, size_t n, size_t *pos) {
    while (*pos < n) {
        uint8_t l = m[*pos];
        if (l == 0) { (*pos)++; return 0; }
        if ((l & 0xC0) == 0xC0) { *pos += 2; return *pos <= n ? 0 : -1; }
        *pos += 1u + l;
    }
    return -1;
}

static int dns_query(uint32_t server, const char *host, int timeout_s, uint32_t *out) {
    uint8_t q[512], r[1500];
    size_t n = 12;
    uint16_t id = (uint16_t)(time(NULL) ^ (uintptr_t)&q);
    memset(q, 0, 12);
    q[0] = id >> 8, q[1] = id & 0xff, q[2] = 0x01;     /* recursion desired */
    q[5] = 1;                                           /* one question */
    for (const char *p = host; *p;) {
        size_t l = strcspn(p, ".");
        if (!l || l > 63 || n + l + 6 > sizeof q) return -1;
        q[n++] = (uint8_t)l;
        memcpy(q + n, p, l), n += l, p += l;
        if (*p == '.') p++;
    }
    q[n++] = 0;
    q[n++] = 0, q[n++] = 1;                             /* type A */
    q[n++] = 0, q[n++] = 1;                             /* class IN */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { timeout_s, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET, sa.sin_port = htons(53), sa.sin_addr.s_addr = server;
    int rc = -1;
    if (sendto(fd, q, n, 0, (struct sockaddr *)&sa, sizeof sa) == (ssize_t)n) {
        ssize_t got = recv(fd, r, sizeof r, 0);
        if (got >= 12 && r[0] == q[0] && r[1] == q[1] && (r[3] & 0x0f) == 0) {
            size_t pos = 12, an = (size_t)(r[6] << 8 | r[7]);
            for (unsigned qd = (unsigned)(r[4] << 8 | r[5]); qd--;)
                if (skip_name(r, (size_t)got, &pos) || (pos += 4) > (size_t)got) goto out;
            while (an--) {                               /* first A record (CNAMEs are skipped) */
                if (skip_name(r, (size_t)got, &pos) || pos + 10 > (size_t)got) break;
                uint16_t type = (uint16_t)(r[pos] << 8 | r[pos + 1]);
                uint16_t len = (uint16_t)(r[pos + 8] << 8 | r[pos + 9]);
                pos += 10;
                if (pos + len > (size_t)got) break;
                if (type == 1 && len == 4) { memcpy(out, r + pos, 4); rc = 0; break; }
                pos += len;
            }
        }
    }
out:
    close(fd);
    return rc;
}

int sw_resolve(const sw_config *cfg, const char *host, uint32_t *addr, char *err, size_t ec) {
    if (cfg->ip[0] && !parse_ipv4(cfg->ip, addr)) return 0;      /* fixed address from the ini */
    if (!parse_ipv4(host, addr)) return 0;
    char list[64];
    snprintf(list, sizeof list, "%s", cfg->dns[0] ? cfg->dns : "1.1.1.1 8.8.8.8");
    for (char *save = NULL, *s = strtok_r(list, " ,", &save); s; s = strtok_r(NULL, " ,", &save)) {
        uint32_t server;
        if (!parse_ipv4(s, &server) && !dns_query(server, host, 3, addr)) return 0;
    }
    snprintf(err, ec, "DNS: cannot resolve %s", host);
    return -1;
}

/* ------------------------------------------------------------------ entropy */

#if defined(__x86_64__)
__attribute__((target("rdrnd"))) static int rdrand64(unsigned long long *v) {
    for (int i = 0; i < 32; i++) if (__builtin_ia32_rdrand64_step(v)) return 1;
    return 0;
}
#endif

int sw_entropy(uint8_t *buf, size_t len) {
#if defined(__PROSPERO__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    for (size_t i = 0; i < len; i += 8) {
        unsigned long long v;
        if (!rdrand64(&v)) return -1;
        v ^= (unsigned long long)ts.tv_nsec * 0x9E3779B97F4A7C15ull;
        memcpy(buf + i, &v, len - i < 8 ? len - i : 8);
    }
    return 0;
#else
    while (len) {
        size_t n = len > 256 ? 256 : len;
        if (getentropy(buf, n)) return -1;
        buf += n, len -= n;
    }
    return 0;
#endif
}

/* ------------------------------------------------------------------ time */

void sw_utc(long long t, int compact, char out[48]) {
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) rem += 86400, days--;
    /* civil_from_days (H. Hinnant) */
    days += 719468;
    long long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    int H = (int)(rem / 3600), M = (int)(rem / 60 % 60), S = (int)(rem % 60);
    if (compact) snprintf(out, 48, "%04lld%02u%02uT%02d%02d%02d", y, m, d, H, M, S);
    else snprintf(out, 48, "%04lld-%02u-%02uT%02d:%02d:%02dZ", y, m, d, H, M, S);
}
