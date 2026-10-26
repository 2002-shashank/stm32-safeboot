/* Helpers shared by host tests, the fault harness and the device emulator. */
#ifndef TESTUTIL_H
#define TESTUTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sb/image.h"

#define TU_KEY_ID 0x4B455931u

extern int tu_failures;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);    \
            tu_failures++;                                                              \
        }                                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        long long _a = (long long)(a), _b = (long long)(b);                             \
        if (_a != _b) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n",        \
                    __FILE__, __LINE__, #a, #b, _a, _b);                                \
            tu_failures++;                                                              \
        }                                                                               \
    } while (0)

/* Deterministic test key pair (seed fixed). */
void tu_keys(uint8_t sk[64], uint8_t pk[32]);

/* Build a signed image (header + payload) for region_addr into buf
 * (>= SB_HDR_SIZE + payload_size). Payload is a fake vector table followed
 * by pseudo-random bytes derived from version. Returns total size. */
uint32_t tu_make_image(uint8_t *buf, uint32_t region_addr, uint32_t version,
                       uint32_t payload_size, uint32_t flags);

/* Write the provisioning block and erase everything else (factory state). */
void tu_factory_reset(void);

/* Program an image directly into flash (like the SWD programmer would). */
void tu_flash_image(uint32_t region_addr, uint32_t version, uint32_t payload_size);

#endif
