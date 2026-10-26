#ifndef SB_SHA256_H
#define SB_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    uint32_t fill;
} sb_sha256_ctx;

void sb_sha256_init(sb_sha256_ctx *c);
void sb_sha256_update(sb_sha256_ctx *c, const void *data, size_t len);
void sb_sha256_final(sb_sha256_ctx *c, uint8_t out[32]);

#endif
