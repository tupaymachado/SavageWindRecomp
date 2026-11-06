/* savewind: title integration (GPL-3.0-or-later).
 *
 *   sw_init(store, notify)  once at startup: reads <root>/savesync.ini, starts the push worker
 *   sw_before_mount()       first save mount of the run: sync with pull allowed, bounded by
 *                           mount_wait; later mounts return at once (the game owns the save)
 *   sw_after_commit()       marks the save dirty; the worker pushes once writes settle
 *
 * Everything is a no-op without a valid config. Sync rounds never overlap (one mutex). */
#include "sw.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static sw_store *g_st;
static sw_notify_fn g_notify;
static sw_config g_cfg;
static int g_enabled, g_mounted;
static pthread_mutex_t g_sync = PTHREAD_MUTEX_INITIALIZER;   /* one sync round at a time */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;     /* guards g_dirty_at */
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static time_t g_dirty_at;                                     /* last commit not pushed yet */

static void quiet(sw_note_kind k, const char *m) { (void)k; (void)m; }
/* background pushes follow every autosave: only results and problems reach the user */
static void no_info(sw_note_kind k, const char *m) { if (k != SW_NOTE_INFO) g_notify(k, m); }

static sw_result run_sync(int allow_pull, int wait_s, sw_notify_fn notify) {
    sw_config cfg = g_cfg;
    cfg.deadline = wait_s ? (long long)time(NULL) + wait_s : 0;
    pthread_mutex_lock(&g_sync);
    sw_result r = sw_sync(&cfg, g_st, allow_pull, notify);
    pthread_mutex_unlock(&g_sync);
    return r;
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mx);
    for (;;) {
        while (!g_dirty_at) pthread_cond_wait(&g_cv, &g_mx);
        time_t now = time(NULL);
        if (now - g_dirty_at < g_cfg.settle_s) {          /* more writes may come: wait */
            pthread_mutex_unlock(&g_mx);
            sleep((unsigned)(g_cfg.settle_s - (now - g_dirty_at)));
            pthread_mutex_lock(&g_mx);
            continue;
        }
        time_t marked = g_dirty_at;
        pthread_mutex_unlock(&g_mx);
        sw_result r = run_sync(0, 0, no_info);             /* the game holds the save: push only */
        pthread_mutex_lock(&g_mx);
        if (g_dirty_at == marked) {
            if (!r.error) g_dirty_at = 0;
            else {                                         /* hub unreachable: retry in a minute */
                pthread_mutex_unlock(&g_mx);
                sleep(60);
                pthread_mutex_lock(&g_mx);
            }
        }
    }
    return NULL;
}

void sw_init(sw_store *st, sw_notify_fn notify) {
    static int once;
    if (once++) return;
    g_st = st;
    g_notify = notify ? notify : quiet;
    uint8_t *ini; size_t n;
    if (st->read(st, "savesync.ini", &ini, &n)) return;   /* no config: sync disabled */
    char *txt = malloc(n + 1);
    if (!txt) { free(ini); return; }
    memcpy(txt, ini, n); txt[n] = 0;
    free(ini);
    int bad = sw_config_parse(txt, &g_cfg);
    free(txt);
    if (bad) {
        g_notify(SW_NOTE_ERROR, "savesync.ini sem url/token: sync desligado");
        return;
    }
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 * 1024);
    if (pthread_create(&t, &a, worker, NULL)) {
        sw_log(st, &g_cfg, "lvl=ERR op=init error=\"worker thread\"");
        return;
    }
    g_enabled = 1;
    sw_log(st, &g_cfg, "lvl=INFO op=init url=%s", g_cfg.url);
}

void sw_before_mount(void) {
    if (!g_enabled || g_mounted) return;
    g_mounted = 1;
    g_notify(SW_NOTE_INFO, "Sincronizando save...");
    sw_result r = run_sync(1, g_cfg.mount_wait_s, g_notify);
    if (!r.error && (r.action == SW_NOOP || r.action == SW_ADOPT)) g_notify(SW_NOTE_INFO, "Save em dia");
}

void sw_after_commit(void) {
    if (!g_enabled) return;
    pthread_mutex_lock(&g_mx);
    g_dirty_at = time(NULL);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mx);
}
