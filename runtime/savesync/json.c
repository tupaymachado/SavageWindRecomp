/* json.c — parser JSON mínimo, recursivo, sem dependências.
 * Escapes suportados: \" \\ \/ \b \f \n \r \t \uXXXX (BMP → UTF-8). */
#include "json.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const char *s; size_t pos; } jparser;

static void jskip_ws(jparser *p) {
    while (p->s[p->pos] == ' ' || p->s[p->pos] == '\t' ||
           p->s[p->pos] == '\n' || p->s[p->pos] == '\r')
        p->pos++;
}

static jnode *jvalue(jparser *p);

static jnode *jnew(jtype t) {
    jnode *j = calloc(1, sizeof(jnode));
    if (!j) return NULL;
    j->t = t;
    return j;
}

void json_free(jnode *j) {
    if (!j) return;
    free(j->str);
    for (size_t i = 0; i < j->n; i++) {
        json_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j);
}

/* decodifica string JSON a partir de p->s[p->pos] (que é '"') */
static char *jstring_raw(jparser *p) {
    if (p->s[p->pos] != '"') return NULL;
    p->pos++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    unsigned cp;
    while (1) {
        char c = p->s[p->pos];
        if (!c) { free(out); return NULL; }
        if (c == '"') { p->pos++; break; }
        if (c == '\\') {
            p->pos++;
            char e = p->s[p->pos++];
            switch (e) {
            case '"':  cp = '"';  break;
            case '\\': cp = '\\'; break;
            case '/':  cp = '/';  break;
            case 'b':  cp = '\b'; break;
            case 'f':  cp = '\f'; break;
            case 'n':  cp = '\n'; break;
            case 'r':  cp = '\r'; break;
            case 't':  cp = '\t'; break;
            case 'u': {
                if (p->s[p->pos] == 0) { free(out); return NULL; }
                cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = p->s[p->pos++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else { free(out); return NULL; }
                }
                break;
            }
            default: free(out); return NULL;
            }
        } else {
            cp = (unsigned char)c;
            p->pos++;
        }
        /* cp → UTF-8 */
        char enc[4];
        size_t en;
        if (cp < 0x80) { enc[0] = (char)cp; en = 1; }
        else if (cp < 0x800) {
            enc[0] = (char)(0xC0 | (cp >> 6));
            enc[1] = (char)(0x80 | (cp & 0x3F));
            en = 2;
        } else {
            enc[0] = (char)(0xE0 | (cp >> 12));
            enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            enc[2] = (char)(0x80 | (cp & 0x3F));
            en = 3;
        }
        if (len + en + 1 > cap) {
            cap *= 2;
            char *r = realloc(out, cap);
            if (!r) { free(out); return NULL; }
            out = r;
        }
        memcpy(out + len, enc, en);
        len += en;
    }
    out[len] = 0;
    return out;
}

static int jpush(jnode *j, char *key, jnode *child) {
    jnode **ri = realloc(j->items, (j->n + 1) * sizeof *ri);
    if (!ri) return -1;
    j->items = ri;
    if (j->t == JOBJ) {
        char **rk = realloc(j->keys, (j->n + 1) * sizeof *rk);
        if (!rk) return -1;
        j->keys = rk;
        j->keys[j->n] = key;
    }
    j->items[j->n] = child;
    j->n++;
    return 0;
}

static jnode *jvalue(jparser *p) {
    jskip_ws(p);
    char c = p->s[p->pos];
    if (c == '{') {
        p->pos++;
        jnode *o = jnew(JOBJ);
        if (!o) return NULL;
        jskip_ws(p);
        if (p->s[p->pos] == '}') { p->pos++; return o; }
        while (1) {
            jskip_ws(p);
            char *key = jstring_raw(p);
            if (!key) { json_free(o); return NULL; }
            jskip_ws(p);
            if (p->s[p->pos] != ':') { free(key); json_free(o); return NULL; }
            p->pos++;
            jnode *v = jvalue(p);
            if (!v) { free(key); json_free(o); return NULL; }
            if (jpush(o, key, v)) { free(key); json_free(v); json_free(o); return NULL; }
            jskip_ws(p);
            if (p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->s[p->pos] == '}') { p->pos++; return o; }
            json_free(o);
            return NULL;
        }
    }
    if (c == '[') {
        p->pos++;
        jnode *a = jnew(JARR);
        if (!a) return NULL;
        jskip_ws(p);
        if (p->s[p->pos] == ']') { p->pos++; return a; }
        while (1) {
            jnode *v = jvalue(p);
            if (!v) { json_free(a); return NULL; }
            if (jpush(a, NULL, v)) { json_free(v); json_free(a); return NULL; }
            jskip_ws(p);
            if (p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->s[p->pos] == ']') { p->pos++; return a; }
            json_free(a);
            return NULL;
        }
    }
    if (c == '"') {
        char *s = jstring_raw(p);
        if (!s) return NULL;
        jnode *j = jnew(JSTR);
        if (!j) { free(s); return NULL; }
        j->str = s;
        return j;
    }
    if (!strncmp(p->s + p->pos, "true", 4)) {
        p->pos += 4;
        jnode *j = jnew(JBOOL);
        if (j) j->num = 1;
        return j;
    }
    if (!strncmp(p->s + p->pos, "false", 5)) {
        p->pos += 5;
        return jnew(JBOOL);
    }
    if (!strncmp(p->s + p->pos, "null", 4)) {
        p->pos += 4;
        return jnew(JNULL);
    }
    /* número */
    {
        char *end;
        double v = strtod(p->s + p->pos, &end);
        if (end == p->s + p->pos) return NULL;
        p->pos = (size_t)(end - p->s);
        jnode *j = jnew(JNUM);
        if (j) j->num = v;
        return j;
    }
}

jnode *json_parse(const char *s) {
    jparser p = {s, 0};
    jnode *j = jvalue(&p);
    if (!j) return NULL;
    jskip_ws(&p);
    if (p.s[p.pos] != 0) { /* lixo depois do valor */
        json_free(j);
        return NULL;
    }
    return j;
}

jnode *json_get(const jnode *obj, const char *key) {
    if (!obj || obj->t != JOBJ) return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

jnode *json_at(const jnode *arr, size_t i) {
    if (!arr || arr->t != JARR || i >= arr->n) return NULL;
    return arr->items[i];
}

const char *json_str(const jnode *j, const char *def) {
    return (j && j->t == JSTR) ? j->str : def;
}

double json_num(const jnode *j, double def) {
    return (j && j->t == JNUM) ? j->num : def;
}

int json_bool(const jnode *j, int def) {
    if (j && j->t == JBOOL) return (int)j->num;
    if (j && j->t == JNUM) return (int)j->num;
    return def;
}
