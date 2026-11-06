/* savewind: one sync round, state, backups, log (GPL-3.0-or-later).
 * Mirrors core/savewind_client.py sync(); the hub tests define the behaviour. */
#include "sw.h"
#include "json.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KEEP_BACKUPS 3

void sw_resp_free(sw_resp *r) { free(r->body); memset(r, 0, sizeof *r); }

static void path2(char *out, size_t cap, const char *a, const char *b) { snprintf(out, cap, "%s/%s", a, b); }

/* ------------------------------------------------------------------ log */

static char g_errbuf[2048];   /* ERR lines waiting to be shipped to the hub */

void sw_log(sw_store *st, const sw_config *cfg, const char *fmt, ...) {
    char line[700], path[200], ts[48];
    sw_utc((long long)time(NULL), 0, ts);
    int n = snprintf(line, sizeof line, "ts=%s node=ps5 ", ts);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    path2(path, sizeof path, cfg->state_dir, "sync.log");
    st->append(st, path, (const uint8_t *)line, strlen(line));
    if (strstr(line, "lvl=ERR") && strlen(g_errbuf) + strlen(line) < sizeof g_errbuf) strcat(g_errbuf, line);
}

static void ship_errors(sw_store *st, const sw_config *cfg) {
    if (!g_errbuf[0]) return;
    sw_resp r = {0};
    char err[160];
    if (!sw_http(cfg, "POST", "/api/log", "text/plain", (const uint8_t *)g_errbuf, strlen(g_errbuf), &r, err, sizeof err)
        && r.status == 200)
        g_errbuf[0] = 0;
    sw_resp_free(&r);
    (void)st;
}

/* ------------------------------------------------------------------ state + backups */

static void load_state(sw_store *st, const sw_config *cfg, sw_state *s) {
    memset(s, 0, sizeof *s);
    char path[200];
    uint8_t *d; size_t n;
    path2(path, sizeof path, cfg->state_dir, "state.json");
    if (st->read(st, path, &d, &n)) return;
    char *txt = malloc(n + 1);
    if (txt) {
        memcpy(txt, d, n); txt[n] = 0;
        jnode *j = json_parse(txt);
        if (j) {
            s->last_id = (long long)json_num(json_get(j, "last_id"), 0);
            snprintf(s->last_hash, sizeof s->last_hash, "%s", json_str(json_get(j, "last_hash"), ""));
            json_free(j);
        }
        free(txt);
    }
    free(d);
}

static int save_state(sw_store *st, const sw_config *cfg, long long id, const char *hash) {
    char path[200], txt[160];
    path2(path, sizeof path, cfg->state_dir, "state.json");
    int n = snprintf(txt, sizeof txt, "{\"last_id\": %lld, \"last_hash\": \"%s\"}", id, hash);
    return st->write(st, path, (const uint8_t *)txt, (size_t)n);
}

struct bk_ctx { char names[64][200]; int n; };
static void collect_bk(void *vctx, const char *path) {
    struct bk_ctx *b = vctx;
    size_t l = strlen(path);
    if (b->n < 64 && l > 4 && !strcmp(path + l - 4, ".tar")) snprintf(b->names[b->n++], 200, "%s", path);
}
static int by_name(const void *a, const void *b) { return strcmp(a, b); }

static void backup(sw_store *st, const sw_config *cfg, const sw_files *local, const char *hash) {
    if (!local->n) return;
    uint8_t *tar; size_t len;
    if (sw_tar_pack(local, &tar, &len)) return;
    char dir[200], path[320], stamp[48];
    sw_utc((long long)time(NULL), 1, stamp);
    path2(dir, sizeof dir, cfg->state_dir, "backups");
    snprintf(path, sizeof path, "%s/%s-%.8s.tar", dir, stamp, hash);
    st->write(st, path, tar, len);
    free(tar);
    struct bk_ctx *b = calloc(1, sizeof *b);
    if (!b) return;
    st->walk(st, dir, collect_bk, b);
    qsort(b->names, (size_t)b->n, sizeof b->names[0], by_name);
    for (int i = 0; i < b->n - KEEP_BACKUPS; i++) st->unlink(st, b->names[i]);
    free(b);
}

/* ------------------------------------------------------------------ hub calls */

static int get_head(const sw_config *cfg, sw_head *h, char *err, size_t ec) {
    sw_resp r = {0};
    memset(h, 0, sizeof *h);
    if (sw_http(cfg, "GET", "/api/head", NULL, NULL, 0, &r, err, ec)) return -1;
    int rc = 0;
    if (r.status == 404) rc = 0;  /* hub empty: id 0 */
    else if (r.status != 200) { snprintf(err, ec, "GET /api/head: HTTP %d", r.status); rc = -1; }
    else {
        char *txt = malloc(r.len + 1);
        jnode *j = NULL;
        if (txt) { memcpy(txt, r.body, r.len); txt[r.len] = 0; j = json_parse(txt); free(txt); }
        if (!j) { snprintf(err, ec, "GET /api/head: bad JSON"); rc = -1; }
        else {
            h->id = (long long)json_num(json_get(j, "id"), 0);
            snprintf(h->hash, sizeof h->hash, "%s", json_str(json_get(j, "hash"), ""));
            snprintf(h->client, sizeof h->client, "%s", json_str(json_get(j, "client"), "?"));
            if (!h->id || strlen(h->hash) != 64) { snprintf(err, ec, "GET /api/head: missing fields"); rc = -1; }
            json_free(j);
        }
    }
    sw_resp_free(&r);
    return rc;
}

/* ------------------------------------------------------------------ the round */

static sw_result pull(const sw_config *cfg, sw_store *st, const sw_head *head, const sw_files *local,
                      const char *local_hash, sw_notify_fn notify) {
    sw_result res = { SW_PULL, 0, head->id, "" };
    char msg[160], path[64], err[160];
    snprintf(msg, sizeof msg, "Baixando save #%lld (%s)...", head->id, head->client);
    notify(SW_NOTE_INFO, msg);
    snprintf(path, sizeof path, "/api/snapshots/%lld/tar", head->id);
    sw_resp r = {0};
    sw_files files = {0};
    char got[65];
    if (sw_http(cfg, "GET", path, NULL, NULL, 0, &r, err, sizeof err)) goto fail;
    if (r.status != 200) { snprintf(err, sizeof err, "GET tar: HTTP %d", r.status); goto fail; }
    if (sw_tar_unpack(r.body, r.len, &files, err, sizeof err)) goto fail;
    sw_content_hash(&files, got);
    if (strcmp(got, head->hash)) { snprintf(err, sizeof err, "downloaded snapshot does not match the head hash"); goto fail; }
    if (cfg->deadline && time(NULL) > cfg->deadline) {  /* the game is about to read the save: never write now */
        snprintf(err, sizeof err, "deadline passed before applying; local save kept");
        goto fail;
    }
    backup(st, cfg, local, local_hash);
    if (sw_write_dir(st, cfg->save_dir, &files)) { snprintf(err, sizeof err, "writing the save failed"); goto fail; }
    save_state(st, cfg, head->id, head->hash);
    snprintf(msg, sizeof msg, "Save atualizado: #%lld de %s", head->id, head->client);
    notify(SW_NOTE_PULL, msg);
    snprintf(res.detail, sizeof res.detail, "files=%zu", files.n);
    sw_resp_free(&r); sw_files_free(&files);
    return res;
fail:
    sw_resp_free(&r); sw_files_free(&files);
    res.error = 1;
    snprintf(res.detail, sizeof res.detail, "%s", err);
    return res;
}

sw_result sw_sync(const sw_config *cfg, sw_store *st, int allow_pull, sw_notify_fn notify) {
    sw_result res = { SW_NOOP, 0, 0, "" };
    sw_state s;
    sw_head head;
    sw_files local = {0};
    char lh[65], err[200] = "";
    uint8_t *tar = NULL;
    sw_resp r = {0};
    load_state(st, cfg, &s);
    res.snapshot = s.last_id;

    if (sw_read_dir(st, cfg->save_dir, &local)) { snprintf(err, sizeof err, "reading the save failed"); goto fail; }
    sw_content_hash(&local, lh);
    if (get_head(cfg, &head, err, sizeof err)) goto fail;
    res.action = sw_decide(&s, lh, &head);

    switch (res.action) {
    case SW_NOOP:
        break;
    case SW_ADOPT:
        save_state(st, cfg, head.id, lh);
        res.snapshot = head.id;
        break;
    case SW_PULL:
        if (!allow_pull) { res.action = SW_NOOP; snprintf(res.detail, sizeof res.detail, "hub newer; pull deferred"); break; }
        res = pull(cfg, st, &head, &local, lh, notify);
        if (res.error) { snprintf(err, sizeof err, "%s", res.detail); goto fail; }
        break;
    case SW_PUSH:
    case SW_CONFLICT: {
        size_t len;
        char path[64];
        notify(SW_NOTE_INFO, "Enviando save...");
        if (sw_tar_pack(&local, &tar, &len)) { snprintf(err, sizeof err, "tar failed"); goto fail; }
        snprintf(path, sizeof path, "/api/snapshots?parent=%lld", s.last_id);
        if (sw_http(cfg, "POST", path, "application/x-tar", tar, len, &r, err, sizeof err)) goto fail;
        char *txt = malloc(r.len + 1);
        jnode *j = NULL;
        if (txt) { memcpy(txt, r.body, r.len); txt[r.len] = 0; j = json_parse(txt); free(txt); }
        long long id = j ? (long long)json_num(json_get(j, "id"), 0) : 0;
        long long head_id = j ? (long long)json_num(json_get(j, "head_id"), 0) : 0;
        int dismissed = j ? json_bool(json_get(j, "dismissed"), 0) : 0;
        json_free(j);
        res.snapshot = id;
        if ((r.status == 200 || r.status == 201) && id) {
            res.action = SW_PUSH;
            save_state(st, cfg, id, lh);
            if (r.status == 201) {
                char msg[80];
                snprintf(msg, sizeof msg, "Save enviado: #%lld", id);
                notify(SW_NOTE_PUSH, msg);
            }
            snprintf(res.detail, sizeof res.detail, r.status == 201 ? "new head" : "unchanged");
        } else if (r.status == 409 && id) {
            if (dismissed) {  /* conflict dismissed in the hub UI: the head wins */
                if (!allow_pull) { res.action = SW_NOOP; snprintf(res.detail, sizeof res.detail, "conflict dismissed; pull deferred"); break; }
                notify(SW_NOTE_INFO, "Conflito descartado na web UI: usando o save do hub");
                res = pull(cfg, st, &head, &local, lh, notify);
                if (res.error) { snprintf(err, sizeof err, "%s", res.detail); goto fail; }
                break;
            }
            res.action = SW_CONFLICT;
            char msg[200];
            snprintf(msg, sizeof msg, "Conflito: save local guardado como #%lld; o hub segue no #%lld. Resolva na web UI.", id, head_id);
            notify(SW_NOTE_CONFLICT, msg);
            snprintf(res.detail, sizeof res.detail, "head=%lld", head_id);
        } else {
            snprintf(err, sizeof err, "POST snapshot: HTTP %d", r.status);
            goto fail;
        }
        break;
    }
    }
    sw_log(st, cfg, "lvl=%s op=sync action=%s snapshot=%lld local=%.12s detail=\"%s\"",
           res.action == SW_CONFLICT ? "ERR" : "INFO", sw_action_name(res.action), res.snapshot, lh, res.detail);
    if (res.action != SW_NOOP) ship_errors(st, cfg);
    free(tar); sw_resp_free(&r); sw_files_free(&local);
    return res;
fail:
    res.error = 1;
    snprintf(res.detail, sizeof res.detail, "%s", err);
    sw_log(st, cfg, "lvl=ERR op=sync action=%s error=\"%s\"", sw_action_name(res.action), err);
    {
        char msg[240];
        snprintf(msg, sizeof msg, "Erro no sync: %s", err);
        notify(SW_NOTE_ERROR, msg);
    }
    free(tar); sw_resp_free(&r); sw_files_free(&local);
    return res;
}
