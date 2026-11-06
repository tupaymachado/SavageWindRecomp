/* sha256.h — SHA-256 puro (FIPS 180-4), sem dependências. */
#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
/* conveniência: hash de um buffer inteiro → hex (out precisa de 65 bytes) */
void sha256_hex(const void *data, size_t n, char out[65]);
/* hash incremental de um arquivo (stdio, funciona no PS5 e no host) — 0 ok */
int sha256_file_hex(const char *path, char out[65]);

#endif
