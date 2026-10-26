/*
 * Records shared between bootloader and app in backup SRAM (never flash):
 *  - sb_crash_t: written by the app's fault handlers, printed by the
 *    bootloader on the next boot.
 *  - sb_bootinfo_t: written by the bootloader before handoff (reset reason,
 *    boot decision, DWT checkpoints), read by the app.
 */
#ifndef SB_CRASH_H
#define SB_CRASH_H

#include <stddef.h>
#include <stdint.h>

#include "sb/layout.h"

#define SB_CRASH_MAGIC 0x48535243u    /* "CRSH" */
#define SB_BOOTINFO_MAGIC 0x4F464E49u /* "INFO" */

enum sb_fault_type { SB_FAULT_HARD = 3, SB_FAULT_MEM = 4, SB_FAULT_BUS = 5, SB_FAULT_USAGE = 6,
                     SB_FAULT_WDG = 0x10, SB_FAULT_APP = 0x20 };

/* RCC_CSR reset flags, normalized (bit n = RCC_CSR bit 25+n). */
#define SB_RST_BOR  0x01u
#define SB_RST_PIN  0x02u
#define SB_RST_POR  0x04u
#define SB_RST_SFT  0x08u
#define SB_RST_IWDG 0x10u
#define SB_RST_WWDG 0x20u
#define SB_RST_LPWR 0x40u

typedef struct {
    uint32_t magic;
    uint32_t count;          /* crashes recorded since the record was cleared */
    uint32_t fault_type;     /* enum sb_fault_type (IPSR exception number) */
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
    uint32_t exc_return;
    uint32_t sp;             /* stack pointer at exception entry */
    uint32_t image_version;
    uint32_t slot;
    uint32_t reset_reason;   /* filled by the bootloader on the next boot */
    uint32_t consumed;       /* nonzero once the bootloader reported it */
    uint32_t crc;            /* CRC32 of the preceding fields except consumed/reset_reason */
} sb_crash_t;

#define SB_CHECKPOINTS 8
enum sb_checkpoint {
    SB_CP_RESET = 0,      /* first instruction of Reset_Handler (CYCCNT ~ 0) */
    SB_CP_MAIN,           /* bootloader main() */
    SB_CP_CLOCK,          /* PLL at 168 MHz */
    SB_CP_JOURNAL,        /* journal scanned */
    SB_CP_SHA,            /* SHA-256 done (last verified image) */
    SB_CP_SIG,            /* Ed25519 done */
    SB_CP_HANDOFF,        /* just before the jump */
    SB_CP_APP,            /* app's first line of main (written by the app) */
};

typedef struct {
    uint32_t magic;
    uint32_t reset_reason;   /* SB_RST_* */
    uint32_t boot_count;
    uint8_t target, reason, trial, attempts;
    uint32_t version;
    uint32_t cyc[SB_CHECKPOINTS];  /* DWT CYCCNT at each checkpoint */
    uint32_t hz[SB_CHECKPOINTS];   /* core clock while the segment ending here ran */
    uint32_t sha_start;            /* CYCCNT when the last SHA-256 started */
    uint32_t crc;
} sb_bootinfo_t;

typedef struct {
    sb_bootinfo_t info;
    sb_crash_t crash;
} sb_shared_t;

static inline uint32_t sb_crash_crc_len(void)
{
    return offsetof(sb_crash_t, reset_reason);
}

void sb_crash_seal(sb_crash_t *c);
int sb_crash_valid(const sb_crash_t *c);

/* Decode a crash record into text (CFSR/HFSR bits named). Returns length. */
size_t sb_crash_format(const sb_crash_t *c, char *out, size_t cap);

/* Convert RCC_CSR to SB_RST_* flags. */
static inline uint32_t sb_reset_reason_from_csr(uint32_t csr)
{
    return (csr >> 25) & 0x7Fu;
}

#endif
