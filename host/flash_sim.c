#include "flash_sim.h"

#include <stdlib.h>
#include <string.h>

#include "sb/flash.h"
#include "sb/layout.h"

static uint8_t mem[SB_FLASH_SIZE];
static fsim_stats_t st;
static long cut_at = -1;
static int cut_variant;
static uint32_t rng;
static jmp_buf *cut_env;
static void (*cut_fn)(void);
static void (*op_hook)(long idx, uint32_t addr, char kind);

static uint32_t xorshift(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

void fsim_erase_all(void)
{
    memset(mem, 0xFF, sizeof mem);
}

uint8_t *fsim_mem(void)
{
    return mem;
}

void fsim_reset_counters(void)
{
    memset(&st, 0, sizeof st);
}

const fsim_stats_t *fsim_stats(void)
{
    return &st;
}

void fsim_arm_cut(long k, int variant, uint32_t seed, jmp_buf *env)
{
    cut_at = k;
    cut_variant = variant;
    rng = seed ? seed : 0x9E3779B9u;
    cut_env = env;
}

void fsim_set_cut_handler(void (*fn)(void))
{
    cut_fn = fn;
}

void fsim_set_op_hook(void (*fn)(long idx, uint32_t addr, char kind))
{
    op_hook = fn;
}

static void power_lost(void)
{
    cut_at = -1;
    if (cut_fn)
        cut_fn();
    if (cut_env)
        longjmp(*cut_env, 1);
    abort();
}

/* Returns 1 if this op is the armed cut (in which case the caller applies
 * the variant and calls power_lost). */
static int count_op(void)
{
    long idx = st.ops++;
    return cut_at >= 0 && idx == cut_at;
}

int sb_flash_erase_sector(unsigned sector)
{
    if (sector >= SB_NUM_SECTORS)
        return -1;
    uint8_t *p = mem + (sb_sector_addr(sector) - SB_FLASH_BASE);
    uint32_t n = sb_sector_size(sector);
    st.erases++;
    if (count_op()) {
        if (cut_variant == FSIM_CUT_AFTER) {
            memset(p, 0xFF, n);
        } else if (cut_variant >= FSIM_CUT_TORN) {
            /* Partially erased: some cells reach 1, others don't. */
            if (cut_variant == 3) {
                uint32_t k = xorshift() % n;
                memset(p, 0xFF, k);
            } else if (cut_variant == 4) {
                for (uint32_t i = 0; i < n; i++)
                    if ((xorshift() & 7u) == 0)
                        p[i] |= (uint8_t)(1u << (xorshift() & 7u));
            } else {
                for (uint32_t i = 0; i < n; i++)
                    p[i] |= (uint8_t)xorshift();
            }
        }
        power_lost();
    }
    memset(p, 0xFF, n);
    if (op_hook)
        op_hook(st.ops - 1, sb_sector_addr(sector), 'E');
    return 0;
}

static void program_word(uint32_t addr, uint32_t w)
{
    uint8_t *p = mem + (addr - SB_FLASH_BASE);
    uint32_t old;
    memcpy(&old, p, 4);
    if (old != SB_FLASH_ERASED_WORD)
        st.overwrite_violations++;
    st.programs++;
    uint32_t v = old & w;
    if (count_op()) {
        if (cut_variant == FSIM_CUT_BEFORE)
            v = old;
        else if (cut_variant >= FSIM_CUT_TORN)
            v = old & (w | xorshift());  /* only some 0-bits got programmed */
        memcpy(p, &v, 4);
        power_lost();
    }
    memcpy(p, &v, 4);
    if (op_hook)
        op_hook(st.ops - 1, addr, 'P');
}

int sb_flash_program(uint32_t addr, const void *src, uint32_t len)
{
    if ((addr & 3u) || (len & 3u) || addr < SB_FLASH_BASE ||
        addr + len > SB_FLASH_BASE + SB_FLASH_SIZE)
        return -1;
    const uint8_t *s = src;
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w;
        memcpy(&w, s + i, 4);
        program_word(addr + i, w);
    }
    return 0;
}

const uint8_t *sb_flash_ptr(uint32_t addr)
{
    return mem + (addr - SB_FLASH_BASE);
}
