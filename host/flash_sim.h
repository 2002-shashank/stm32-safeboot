/*
 * Host model of the F407 flash with power-cut injection.
 * Erase sets a sector to 0xFF; program ANDs bits in (it can only clear bits).
 * Every sector erase and every 32-bit word program counts as one flash op.
 */
#ifndef FLASH_SIM_H
#define FLASH_SIM_H

#include <setjmp.h>
#include <stdint.h>

enum fsim_variant {
    FSIM_CUT_BEFORE = 0,  /* op k never happened */
    FSIM_CUT_AFTER = 1,   /* op k completed, power lost right after */
    FSIM_CUT_TORN = 2,    /* op k partially applied (random, seeded) */
    /* Erase only: 2 = random bits set, 3 = random prefix erased,
     * 4 = sparse (1 bit in ~64 set): most data survives, so magic/commit
     * often stay intact while seq/body change. Program: 2..4 = torn words. */
};
#define FSIM_NUM_VARIANTS 5

typedef struct {
    long ops;              /* flash ops executed since fsim_reset_counters */
    long erases, programs;
    long overwrite_violations; /* programs into non-erased words */
} fsim_stats_t;

void fsim_erase_all(void);
uint8_t *fsim_mem(void);   /* SB_FLASH_SIZE bytes backing 0x08000000 */
void fsim_reset_counters(void);
const fsim_stats_t *fsim_stats(void);

/* Arm a power cut at op index k (0-based, counted from the last counter
 * reset). When reached, the op is applied per variant and then control
 * longjmps to env with value 1. k < 0 disarms. */
void fsim_arm_cut(long k, int variant, uint32_t seed, jmp_buf *env);

/* Optional callback instead of longjmp (used by the device emulator). */
void fsim_set_cut_handler(void (*fn)(void));

/* Called after every completed flash op (not on a cut). kind: 'E' or 'P'. */
void fsim_set_op_hook(void (*fn)(long idx, uint32_t addr, char kind));

#endif
