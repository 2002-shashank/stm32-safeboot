/*
 * Bootloader services for the app, through a versioned table at the fixed
 * address SB_API_ADDR (last 256 B of the bootloader region). The functions
 * run in the app's context: on the app's stack, using no bootloader RAM
 * except the reserved RAM-function area.
 */
#ifndef SB_API_H
#define SB_API_H

#include <stdint.h>

#include "sb/crash.h"
#include "sb/layout.h"

#define SB_API_MAGIC 0x49504153u   /* "SAPI" */
#define SB_API_VERSION 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    /* Mark the running slot CONFIRMED (ends the trial). 0 on success. */
    int (*boot_confirm)(void);
    /* Boot decision, reset reason and DWT checkpoints of this boot. */
    const sb_bootinfo_t *(*boot_info)(void);
    /* v2: reset into the bootloader's UART update mode. Does not return. */
    void (*boot_request_update)(void);
} sb_api_t;

/* Returns the table if the bootloader provides at least API version `min`. */
static inline const sb_api_t *sb_api_v(uint32_t min)
{
    const sb_api_t *a = (const sb_api_t *)SB_API_ADDR;
    return a->magic == SB_API_MAGIC && a->version >= min ? a : 0;
}

static inline const sb_api_t *sb_api(void)
{
    return sb_api_v(1);
}

#endif
