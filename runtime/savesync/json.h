/* json.h — parser JSON mínimo (árvore em memória). Suficiente para o
 * protocolo do savesync: config, manifest, state, resposta do batch. */
#ifndef JSON_H
#define JSON_H
#include <stddef.h>

typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } jtype;

typedef struct jnode jnode;
struct jnode {
    jtype    t;
    double   num;      /* JNUM (e JBOOL: 0/1) */
    char    *str;      /* JSTR */
    jnode  **items;    /* JARR/JOBJ: filhos */
    char   **keys;     /* JOBJ: chaves alinhadas com items */
    size_t   n;
};

/* parse; NULL em erro. libera com json_free. */
jnode *json_parse(const char *s);
void   json_free(jnode *j);

/* JOBJ: filho pela chave (NULL se não for obj ou não achar) */
jnode *json_get(const jnode *obj, const char *key);
/* JARR: filho pelo índice */
jnode *json_at(const jnode *arr, size_t i);
/* convenções seguras: NULL/erro → default */
const char *json_str(const jnode *j, const char *def);
double      json_num(const jnode *j, double def);
int         json_bool(const jnode *j, int def);

#endif
