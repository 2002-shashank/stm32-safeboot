/*
 * Flash access used by the portable core. Implemented by target/flash_f4.c on
 * hardware and host/flash_sim.c on the host. All functions are stateless so
 * they can be called through the bootloader API table from app context.
 */
#ifndef SB_FLASH_H
#define SB_FLASH_H

#include <stdint.h>

/* Erase one sector (0..11). Returns 0 on success. */
int sb_flash_erase_sector(unsigned sector);

/* Program len bytes (addr and len word aligned) into erased flash, one 32-bit
 * word per flash operation. Returns 0 on success. Does not verify. */
int sb_flash_program(uint32_t addr, const void *src, uint32_t len);

/* Read-only view of flash at addr. */
const uint8_t *sb_flash_ptr(uint32_t addr);

static inline uint32_t sb_flash_word(uint32_t addr)
{
    const uint8_t *p = sb_flash_ptr(addr);
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#endif
