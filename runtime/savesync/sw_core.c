/* savewind: file sets, content hash, ustar, decision table, config (GPL-3.0-or-later). */
#include "sw.h"
#include "sha256.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW_MAX_FILES 1000
#define SW_MAX_TOTAL (64u * 1024u * 1024u)

/* ------------------------------------------------------------------ file sets */

void sw_files_free(sw_files *fs) {
    for (size_t i = 0; i < fs->n; i++) { free(fs->v[i].path); free(fs->v[i].data); }
    free(fs->v);
    memset(fs, 0, sizeof *fs);
}

int sw_files_add(sw_files *fs, const char *path, const uint8_t *data, size_t len) {
    if (fs->n == fs->cap) {
        size_t cap = fs->cap ? fs->cap * 2 : 32;
        sw_file *v = realloc(fs->v, cap * sizeof *v);
        if (!v) return -ENOMEM;
        fs->v = v, fs->cap = cap;
    }
    sw_file *f = &fs->v[fs->n];
    f->path = strdup(path);
    f->data = malloc(len ? len : 1);
    if (!f->path || !f->data) { free(f->path); free(f->data); return -ENOMEM; }
    if (len) memcpy(f->data, data, len);
    f->len = len;
    fs->n++;
    return 0;
}

static int by_path(const void *a, const void *b) {
    return strcmp(((const sw_file *)a)->path, ((const sw_file *)b)->path);
}
void sw_files_sort(sw_files *fs) { if (fs->n > 1) qsort(fs->v, fs->n, sizeof *fs->v, by_path); }

int sw_ignored(const char *rel) {
    const char *p = rel;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && p[0] == '.') return 1;
        if (n >= 5 && !memcmp(p + n - 5, ".part", 5)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int sw_safe_rel(const char *rel) {
    if (!*rel || rel[0] == '/' || strlen(rel) > 512) return 0;
    for (const char *c = rel; *c; c++)
        if (*c == '\\' || *c == ':' || (unsigned char)*c < 0x20) return 0;
    const char *p = rel;
    for (;;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!e) return 1;
        p = e + 1;
    }
}

void sw_content_hash(const sw_files *fs_in, char out[65]) {
    sw_files *fs = (sw_files *)fs_in;
    sw_files_sort(fs);
    sha256_ctx c;
    sha256_init(&c);
    for (size_t i = 0; i < fs->n; i++) {
        char hex[65];
        sha256_hex(fs->v[i].data, fs->v[i].len, hex);
        sha256_update(&c, fs->v[i].path, strlen(fs->v[i].path) + 1);  /* path + NUL */
        sha256_update(&c, hex, 64);
        sha256_update(&c, "\n", 1);
    }
    uint8_t d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
    out[64] = 0;
}

/* ------------------------------------------------------------------ directories */

struct walk_ctx { sw_store *st; size_t skip; sw_files *fs; int err; };

static void add_from_store(void *vctx, const char *path) {
    struct walk_ctx *w = vctx;
    if (w->err) return;
    const char *rel = path + w->skip;
    if (sw_ignored(rel) || !sw_safe_rel(rel)) return;
    uint8_t *data; size_t len;
    int r = w->st->read(w->st, path, &data, &len);
    if (r) { w->err = r; return; }
    w->err = sw_files_add(w->fs, rel, data, len);
    free(data);
}

int sw_read_dir(sw_store *st, const char *dir, sw_files *fs) {
    struct walk_ctx w = { st, strlen(dir) + 1, fs, 0 };
    int r = st->walk(st, dir, add_from_store, &w);
    if (!r) r = w.err;
    if (r) sw_files_free(fs);
    else sw_files_sort(fs);
    return r;
}

struct del_ctx { size_t skip; const sw_files *keep; char (*extra)[600]; size_t n, cap; };

static void collect_extra(void *vctx, const char *path) {
    struct del_ctx *d = vctx;
    const char *rel = path + d->skip;
    if (sw_ignored(rel)) return;
    for (size_t i = 0; i < d->keep->n; i++)
        if (!strcmp(d->keep->v[i].path, rel)) return;
    if (d->n < d->cap) snprintf(d->extra[d->n++], 600, "%s", path);
}

int sw_write_dir(sw_store *st, const char *dir, const sw_files *fs) {
    char path[700];
    for (size_t i = 0; i < fs->n; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, fs->v[i].path);
        int r = st->write(st, path, fs->v[i].data, fs->v[i].len);
        if (r) return r;
    }
    struct del_ctx d = { strlen(dir) + 1, fs, calloc(SW_MAX_FILES, 600), 0, SW_MAX_FILES };
    if (!d.extra) return -ENOMEM;
    int r = st->walk(st, dir, collect_extra, &d);
    for (size_t i = 0; !r && i < d.n; i++) {
        r = st->unlink(st, d.extra[i]);
        /* drop now-empty parent dirs (best effort) */
        for (char *s = strrchr(d.extra[i], '/'); s && (size_t)(s - d.extra[i]) > strlen(dir); s = strrchr(d.extra[i], '/')) {
            *s = 0;
            if (st->rmdir(st, d.extra[i])) break;
        }
    }
    free(d.extra);
    return r;
}

/* ------------------------------------------------------------------ ustar */

static void oct(char *dst, size_t width, uint64_t v) {   /* NUL-terminated octal, zero padded */
    snprintf(dst, width, "%0*llo", (int)width - 1, (unsigned long long)v);
}

int sw_tar_pack(const sw_files *fs, uint8_t **out, size_t *len) {
    size_t total = 1024;
    for (size_t i = 0; i < fs->n; i++) total += 512 + ((fs->v[i].len + 511) & ~(size_t)511);
    uint8_t *buf = calloc(1, total);
    if (!buf) return -ENOMEM;
    size_t off = 0;
    for (size_t i = 0; i < fs->n; i++) {
        const char *p = fs->v[i].path;
        size_t pl = strlen(p);
        uint8_t *h = buf + off;
        if (pl <= 100) memcpy(h, p, pl);
        else {  /* split into prefix (155) / name (100) at a '/' */
            const char *cut = NULL;  /* first '/' that leaves name <= 100 and prefix <= 155 */
            for (const char *s = p; *s && !cut; s++)
                if (*s == '/' && (size_t)(s - p) <= 155 && pl - (size_t)(s - p) - 1 <= 100) cut = s;
            if (!cut) { free(buf); return -ENAMETOOLONG; }
            memcpy(h, cut + 1, pl - (size_t)(cut - p) - 1);
            memcpy(h + 345, p, (size_t)(cut - p));
        }
        oct((char *)h + 100, 8, 0644);
        oct((char *)h + 108, 8, 0);
        oct((char *)h + 116, 8, 0);
        oct((char *)h + 124, 12, fs->v[i].len);
        oct((char *)h + 136, 12, 0);
        h[156] = '0';
        memcpy(h + 257, "ustar\0" "00", 8);
        memset(h + 148, ' ', 8);
        unsigned sum = 0;
        for (int k = 0; k < 512; k++) sum += h[k];
        snprintf((char *)h + 148, 8, "%06o", sum);
        h[155] = ' ';
        off += 512;
        memcpy(buf + off, fs->v[i].data, fs->v[i].len);
        off += (fs->v[i].len + 511) & ~(size_t)511;
    }
    *out = buf;
    *len = off + 1024;
    return 0;
}

static int parse_oct(const uint8_t *s, size_t n, uint64_t *v) {
    uint64_t r = 0; size_t i = 0;
    while (i < n && s[i] == ' ') i++;
    if (i == n || s[i] < '0' || s[i] > '7') return -1;
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) r = r * 8 + (s[i] - '0');
    *v = r;
    return 0;
}

int sw_tar_unpack(const uint8_t *data, size_t len, sw_files *fs, char *err, size_t errcap) {
    size_t off = 0, total = 0;
    memset(fs, 0, sizeof *fs);
#define FAIL(...) do { snprintf(err, errcap, __VA_ARGS__); sw_files_free(fs); return -1; } while (0)
    while (off + 512 <= len) {
        const uint8_t *h = data + off;
        int zero = 1;
        for (int k = 0; k < 512 && zero; k++) zero = !h[k];
        if (zero) break;
        unsigned sum = 0; uint64_t want, size;
        for (int k = 0; k < 512; k++) sum += (k >= 148 && k < 156) ? ' ' : h[k];
        if (parse_oct(h + 148, 8, &want) || want != sum) FAIL("bad tar checksum at %zu", off);
        if (parse_oct(h + 124, 12, &size)) FAIL("bad tar size at %zu", off);
        char name[260] = {0};
        if (!memcmp(h + 257, "ustar", 5) && h[345]) {
            memcpy(name, h + 345, strnlen((const char *)h + 345, 155));
            strcat(name, "/");
        }
        strncat(name, (const char *)h, strnlen((const char *)h, 100));
        size_t nl = strlen(name);
        while (nl && name[nl - 1] == '/') name[--nl] = 0;
        char type = (char)h[156];
        off += 512;
        if (size > len - off) FAIL("truncated tar");
        if (type == '5') { off += (size + 511) & ~(uint64_t)511; continue; }
        if (type != '0' && type != 0) FAIL("not a regular file: %s", name);
        if (!sw_safe_rel(name)) FAIL("bad path: %s", name);
        total += size;
        if (fs->n >= SW_MAX_FILES || total > SW_MAX_TOTAL) FAIL("snapshot too large");
        if (!sw_ignored(name)) {
            for (size_t i = 0; i < fs->n; i++)
                if (!strcmp(fs->v[i].path, name)) FAIL("duplicate path: %s", name);
            if (sw_files_add(fs, name, data + off, size)) FAIL("out of memory");
        }
        off += (size + 511) & ~(uint64_t)511;
    }
#undef FAIL
    sw_files_sort(fs);
    return 0;
}

/* ------------------------------------------------------------------ decision */

static const char *EMPTY_HASH = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

sw_action sw_decide(const sw_state *st, const char *local, const sw_head *head) {
    if (!head->id) return strcmp(local, EMPTY_HASH) ? SW_PUSH : SW_NOOP;
    if (!strcmp(head->hash, local))
        return (st->last_id == head->id && !strcmp(st->last_hash, local)) ? SW_NOOP : SW_ADOPT;
    if (!st->last_id) return strcmp(local, EMPTY_HASH) ? SW_CONFLICT : SW_PULL;
    int local_changed = strcmp(local, st->last_hash) != 0;
    int head_changed = head->id != st->last_id;
    if (head_changed && !local_changed) return SW_PULL;
    if (local_changed && !head_changed) return SW_PUSH;
    return SW_CONFLICT;
}

const char *sw_action_name(sw_action a) {
    static const char *n[] = { "noop", "adopt", "pull", "push", "conflict" };
    return a <= SW_CONFLICT ? n[a] : "?";
}

/* ------------------------------------------------------------------ config */

int sw_config_parse(const char *text, sw_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->save_dir, sizeof cfg->save_dir, "save");
    snprintf(cfg->state_dir, sizeof cfg->state_dir, "savesync");
    cfg->timeout_s = 10;
    cfg->settle_s = 5;
    cfg->mount_wait_s = 20;
    char line[512];
    for (const char *p = text; *p;) {
        size_t n = strcspn(p, "\r\n");
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p += n;
        while (*p == '\r' || *p == '\n') p++;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == ';' || !eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (isspace((unsigned char)*k)) k++;
        for (char *e = eq - 1; e >= k && isspace((unsigned char)*e); e--) *e = 0;
        while (isspace((unsigned char)*v)) v++;
        for (char *e = v + strlen(v) - 1; e >= v && isspace((unsigned char)*e); e--) *e = 0;
        if (!strcmp(k, "url")) snprintf(cfg->url, sizeof cfg->url, "%s", v);
        else if (!strcmp(k, "token")) snprintf(cfg->token, sizeof cfg->token, "%s", v);
        else if (!strcmp(k, "ip")) snprintf(cfg->ip, sizeof cfg->ip, "%s", v);
        else if (!strcmp(k, "dns")) snprintf(cfg->dns, sizeof cfg->dns, "%s", v);
        else if (!strcmp(k, "timeout")) cfg->timeout_s = atoi(v) > 0 ? atoi(v) : cfg->timeout_s;
        else if (!strcmp(k, "settle")) cfg->settle_s = atoi(v) >= 0 ? atoi(v) : cfg->settle_s;
        else if (!strcmp(k, "mount_wait")) cfg->mount_wait_s = atoi(v) > 0 ? atoi(v) : cfg->mount_wait_s;
    }
    size_t ul = strlen(cfg->url);
    while (ul && cfg->url[ul - 1] == '/') cfg->url[--ul] = 0;
    return cfg->url[0] && cfg->token[0] ? 0 : -1;
}
