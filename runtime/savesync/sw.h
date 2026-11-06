/* savewind save sync, in-title module (SPDX-License-Identifier: GPL-3.0-or-later).
 *
 * The title calls two hooks:
 *   sw_before_mount()  in OpenSaveDataFileSystem, before the game reads its save: pushes a
 *                      pending local change, pulls a newer hub save. Blocks up to the
 *                      configured timeout; on any failure the game just uses the local save.
 *   sw_after_commit()  in IFile::Commit: wakes a background thread that waits for writes to
 *                      settle and pushes. Never blocks the game.
 * Without a config file (url + token) both hooks do nothing.
 *
 * Same protocol and decision table as core/savewind_core.py (the hub's reference).
 */
#ifndef SAVEWIND_SW_H
#define SAVEWIND_SW_H
#include <stddef.h>
#include <stdint.h>

/* ---- storage: all paths relative to the storage root (title: /data/savagewind) ---- */
typedef struct sw_store {
    /* list regular files under dir, recursively; calls fn(rel path from root) for each.
     * Missing dir = 0 files. Returns 0 or -errno. */
    int (*walk)(struct sw_store *, const char *dir, void (*fn)(void *, const char *), void *ctx);
    /* whole file into a malloc'd buffer */
    int (*read)(struct sw_store *, const char *path, uint8_t **data, size_t *len);
    /* create/replace a whole file atomically, creating parent dirs */
    int (*write)(struct sw_store *, const char *path, const uint8_t *data, size_t len);
    int (*append)(struct sw_store *, const char *path, const uint8_t *data, size_t len);
    int (*unlink)(struct sw_store *, const char *path);
    int (*rmdir)(struct sw_store *, const char *path);  /* only if empty; errors ignored */
    void *impl;
} sw_store;

/* ---- in-memory file set (snapshot contents) ---- */
typedef struct { char *path; uint8_t *data; size_t len; } sw_file;
typedef struct { sw_file *v; size_t n, cap; } sw_files;

void sw_files_free(sw_files *fs);
int  sw_files_add(sw_files *fs, const char *path, const uint8_t *data, size_t len); /* copies */
void sw_files_sort(sw_files *fs);
int  sw_ignored(const char *rel);   /* dotfile or *.part component */
int  sw_safe_rel(const char *rel);  /* 1 if a safe relative path */
void sw_content_hash(const sw_files *fs, char out[65]);  /* sorts fs */

/* read every syncable file under dir; paths in fs are relative to dir */
int  sw_read_dir(sw_store *st, const char *dir, sw_files *fs);
/* make dir mirror fs (write all, delete syncable extras) */
int  sw_write_dir(sw_store *st, const char *dir, const sw_files *fs);

/* ---- ustar ---- */
int  sw_tar_pack(const sw_files *fs, uint8_t **out, size_t *len);
/* 0 ok, -1 malformed/unsafe (msg in err) */
int  sw_tar_unpack(const uint8_t *data, size_t len, sw_files *fs, char *err, size_t errcap);

/* ---- HTTP ---- */
typedef struct { int status; uint8_t *body; size_t len; } sw_resp;
void sw_resp_free(sw_resp *r);

/* ---- decision (same table as savewind_core.decide) ---- */
typedef enum { SW_NOOP, SW_ADOPT, SW_PULL, SW_PUSH, SW_CONFLICT } sw_action;
typedef struct { long long last_id; char last_hash[65]; } sw_state;   /* last_id 0 = never */
typedef struct { long long id; char hash[65]; char client[33]; } sw_head; /* id 0 = hub empty */
sw_action sw_decide(const sw_state *st, const char *local_hash, const sw_head *head);
const char *sw_action_name(sw_action a);

/* ---- the engine ---- */
typedef struct {
    char url[256];            /* https://sync.tupay.dev.br (http:// for tests) */
    char token[128];
    char ip[16];              /* optional fixed IPv4 of the hub (skips DNS) */
    char dns[64];             /* DNS servers, default "1.1.1.1 8.8.8.8" */
    char save_dir[128];       /* relative to the store root: "save" */
    char state_dir[128];      /* "savesync": state.json, backups/, sync.log */
    int  timeout_s;           /* per network operation; mount waits at most ~2x this */
    int  settle_s;            /* after the last commit, before pushing */
    int  mount_wait_s;        /* max time the first mount may wait for the sync */
    long long deadline;       /* runtime: epoch seconds; past it, network stops and nothing is written */
} sw_config;

typedef enum { SW_NOTE_INFO, SW_NOTE_PUSH, SW_NOTE_PULL, SW_NOTE_CONFLICT, SW_NOTE_ERROR } sw_note_kind;
typedef void (*sw_notify_fn)(sw_note_kind kind, const char *msg);

typedef struct {
    sw_action action;         /* what was decided (SW_NOOP on error) */
    int error;                /* 0 ok; nonzero = failed (msg in detail) */
    long long snapshot;
    char detail[200];
} sw_result;

/* parse "key = value" lines (url, token, timeout, settle, mount_wait); 0 ok, -1 missing url/token */
int  sw_config_parse(const char *text, sw_config *cfg);

/* one sync round (the Python client's sync()). allow_pull=0 while the game holds the save */
sw_result sw_sync(const sw_config *cfg, sw_store *st, int allow_pull, sw_notify_fn notify);

/* engine log: one logfmt line, appended to <state_dir>/sync.log (and errors shipped to the hub) */
void sw_log(sw_store *st, const sw_config *cfg, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ---- title integration (sw_hooks.c) ---- */
void sw_init(sw_store *st, sw_notify_fn notify);  /* reads <root>/savesync.ini; safe to call once */
void sw_before_mount(void);
void sw_after_commit(void);

/* ---- platform helpers (sw_net.c) ---- */
int  sw_resolve(const sw_config *cfg, const char *host, uint32_t *addr_be, char *err, size_t ec);
int  sw_entropy(uint8_t *buf, size_t len);
void sw_utc(long long t, int compact, char out[48]);  /* ISO-8601 Z, or YYYYMMDDTHHMMSS */

/* ---- transport (sw_http.c); TLS via BearSSL when built with SW_TLS ---- */
int sw_http(const sw_config *cfg, const char *method, const char *path, const char *ctype,
            const uint8_t *body, size_t len, sw_resp *resp, char *err, size_t errcap);
#endif
