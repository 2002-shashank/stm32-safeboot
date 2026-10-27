/* Hardware fault-injection control block (bootloader_fi builds), in backup
 * SRAM past the shared crash/boot-info area. Written by the host over SWD. */
#ifndef SB_FAULT_INJECT_H
#define SB_FAULT_INJECT_H

#include <stdint.h>

#include "sb/layout.h"

#define SB_FI_ADDR (SB_BKPSRAM_ADDR + 0xC00u)
#define SB_FI_MAGIC 0x494A4E46u   /* "FNJI" */

enum { SB_FI_BEFORE = 0, SB_FI_MID = 1, SB_FI_AFTER = 2 };

typedef struct {
    uint32_t magic;
    uint32_t armed_k;     /* op index to interrupt (0xFFFFFFFF: count only) */
    uint32_t mode;
    uint32_t delay_cyc;   /* mode 1: cycles between start and reset */
    uint32_t count;       /* ops seen since arming */
    uint32_t fired;
    uint32_t fired_addr;
    /* Evidence recorded by the bootloader itself, so checks never depend on
     * a log line surviving the reset: */
    uint32_t post_valid;      /* set at the first boot after the injected reset */
    uint32_t post_have;       /* that boot's scan found a valid record */
    uint32_t post_rec[16];    /* the sb_record_t it scanned */
    uint32_t recovery;        /* boots that ended in bootloader recovery mode */
} sb_fi_t;

#endif
