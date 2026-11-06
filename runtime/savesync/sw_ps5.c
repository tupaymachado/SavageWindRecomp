/* savewind: PS5 title glue (GPL-3.0-or-later).
 * sw_store over the title's storage broker (rt_storage_*, paths relative to /data/savagewind)
 * and toasts via sceKernelSendNotificationRequest. Built only inside the recomp fork. */
#ifdef __PROSPERO__
#include "sw.h"
#include "../storage_ps5.h"
#include "sw_title.h"
#include "../../ps5/storagebroker/protocol.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ store */

static int list_dir(const char *dir, uint8_t **buf, int *len) {
    uint32_t cap = 4096;
    *buf = NULL;
    for (;;) {
        uint8_t *nb = realloc(*buf, cap);
        if (!nb) { free(*buf); return -ENOMEM; }
        *buf = nb;
        int r = rt_storage_list(dir, *buf, cap);
        if (r != -ENOSPC || cap >= SB_MAX_TRANSFER) {
            if (r < 0) { free(*buf); *buf = NULL; }
            *len = r;
            return r < 0 ? r : 0;
        }
        cap *= 2;
    }
}

static int walk_rec(const char *dir, void (*fn)(void *, const char *), void *ctx, int depth) {
    if (depth > 16) return -ELOOP;
    uint8_t *buf; int len;
    int r = list_dir(dir, &buf, &len);
    if (r) return r == -ENOENT ? 0 : r;
    size_t pos = 0;
    while (!r && pos + sizeof(struct sb_list_entry) <= (size_t)len) {
        struct sb_list_entry e;
        memcpy(&e, buf + pos, sizeof e);
        pos += sizeof e;
        if (!e.name_length || e.name_length > (size_t)len - pos || e.name_length > 255) { r = -EPROTO; break; }
        char child[SB_MAX_PATH];
        snprintf(child, sizeof child, "%s/%.*s", dir, (int)e.name_length, (const char *)buf + pos);
        pos += e.name_length;
        if (e.type == SB_ENTRY_DIRECTORY) r = walk_rec(child, fn, ctx, depth + 1);
        else if (e.type == SB_ENTRY_REGULAR) fn(ctx, child);
    }
    free(buf);
    return r;
}

static int s_walk(sw_store *s, const char *dir, void (*fn)(void *, const char *), void *ctx) {
    (void)s;
    return walk_rec(dir, fn, ctx, 0);
}

static int s_read(sw_store *s, const char *path, uint8_t **data, size_t *len) {
    (void)s;
    uint64_t size;
    int r = rt_storage_stat(path, &size);
    if (r) return r;
    if (size > 64u * 1024u * 1024u) return -EFBIG;
    *data = malloc(size ? size : 1);
    if (!*data) return -ENOMEM;
    size_t got = 0;
    while (got < size) {
        ssize_t n = rt_storage_read(path, *data + got, size - got, got);
        if (n <= 0) { free(*data); *data = NULL; return n < 0 ? (int)n : -EIO; }
        got += (size_t)n;
    }
    *len = got;
    return 0;
}

static int parent_dirs(const char *path) {
    char dir[SB_MAX_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = 0;
    int r = rt_storage_mkdirs(dir);
    return r == -EEXIST ? 0 : r;
}

static int s_write(sw_store *s, const char *path, const uint8_t *data, size_t len) {
    (void)s;
    int r = parent_dirs(path);
    return r ? r : rt_storage_replace(path, data, len);   /* atomic: temp + rename + fsync */
}

static int s_append(sw_store *s, const char *path, const uint8_t *data, size_t len) {
    (void)s;
    uint64_t size;
    if (rt_storage_stat(path, &size)) {
        int r = parent_dirs(path);
        if (r || ((r = rt_storage_create(path)) && r != -EEXIST)) return r;
    } else if (size > 512 * 1024) {                       /* rotate the log */
        char old[SB_MAX_PATH];
        snprintf(old, sizeof old, "%s.1", path);
        rt_storage_rename(path, old);
        rt_storage_create(path);
    }
    return rt_storage_append(path, data, len);
}

static int s_unlink(sw_store *s, const char *path) { (void)s; return rt_storage_unlink(path); }
static int s_rmdir(sw_store *s, const char *path) { (void)s; return rt_storage_rmdir(path); }

static sw_store g_title_store = { s_walk, s_read, s_write, s_append, s_unlink, s_rmdir, NULL };

/* ------------------------------------------------------------------ toasts */

typedef struct { char header[45]; char message[3075]; } notify_request;  /* SDK notify sample layout */
int sceKernelSendNotificationRequest(int, notify_request *, size_t, int);

static pthread_mutex_t toast_mx = PTHREAD_MUTEX_INITIALIZER;   /* mount thread + push worker */

static void toast(sw_note_kind kind, const char *msg) {
    pthread_mutex_lock(&toast_mx);
    static char last[256];
    static time_t last_at;
    time_t now = time(NULL);
    if (!strcmp(msg, last) && now - last_at < 30) { pthread_mutex_unlock(&toast_mx); return; }  /* drop repeats */
    snprintf(last, sizeof last, "%s", msg);
    last_at = now;
    static notify_request req;                               /* 3 KB: keep it off the caller's stack */
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s%s", kind == SW_NOTE_CONFLICT || kind == SW_NOTE_ERROR ? "⚠ " : "", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    pthread_mutex_unlock(&toast_mx);
}

/* ------------------------------------------------------------------ entry points for fs.c */

void sw_title_before_mount(void) {
    sw_init(&g_title_store, toast);   /* first call reads savesync.ini; later calls return */
    sw_before_mount();
}

void sw_title_after_commit(void) { sw_after_commit(); }
#endif
