/*
 * Phase 0 on-board measurements (runs from 0x08000000, linked like the
 * bootloader). Results go into `bench` (read over SWD by tools/board.py) and
 * are printed over RTT.
 *
 * WARNING: erases S2, S3 (journal), S4 (provisioning) and S11 (recovery).
 *
 * Commands (host writes `bench_cmd` over SWD):
 *   1 = software reset, 2 = IWDG reset, 3 = rerun benchmarks.
 * Every boot also records whether backup SRAM and .noinit RAM kept their
 * pattern across the previous reset, and whether a debugger was attached.
 */
#include <string.h>

#include "bench_vectors.h"
#include "board.h"
#include "monocypher-ed25519.h"
#include "rtt.h"
#include "sb/crc32.h"
#include "sb/flash.h"
#include "sb/journal.h"
#include "sb/layout.h"
#include "sb/sha256.h"

#define PATTERN 0x5AFEB00Bu

typedef struct {
    uint32_t magic;            /* 0xBE7C0001 when results are complete */
    uint32_t hz;
    uint32_t sha_64k_cyc, sha_384k_cyc;
    uint32_t crc32_1k_cyc;
    uint32_t ed_verify_cyc;
    int32_t ed_ok, ed_tampered_rejected;
    uint32_t ed_stack_bytes;
    uint32_t erase16_cyc, erase64_cyc, erase128_cyc;
    uint32_t prog16k_cyc;
    int32_t prog_ok, erase_ok;
    uint32_t jscan_empty_cyc, jappend_cyc, jscan_full_cyc, jcompact_cyc;
    int32_t journal_ok;
    /* reset/retention, filled on every boot */
    uint32_t boot_count;
    uint32_t rcc_csr;
    uint32_t bkpsram_kept;     /* 1 if backup SRAM pattern survived */
    uint32_t noinit_kept;      /* 1 if .noinit SRAM pattern survived */
    uint32_t debugger_attached;/* DHCSR.C_DEBUGEN at boot */
    uint32_t cyccnt_counts;    /* 1 if CYCCNT advanced across a 1 ms SysTick wait */
} bench_t;

volatile bench_t bench;
volatile uint32_t bench_cmd;
__attribute__((section(".noinit"))) volatile uint32_t noinit_pattern[2];

typedef struct {
    uint32_t pattern;
    uint32_t boots;
} bkp_probe_t;
/* Past the shared crash/bootinfo area. */
#define BKP_PROBE ((volatile bkp_probe_t *)(SB_BKPSRAM_ADDR + 0x800u))

static uint32_t cyc_since(uint32_t t0)
{
    return sb_cyc() - t0;
}

static void retention_check(void)
{
    sb_bkpsram_enable();
    bench.rcc_csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;
    bench.bkpsram_kept = BKP_PROBE->pattern == PATTERN;
    bench.boot_count = bench.bkpsram_kept ? BKP_PROBE->boots + 1 : 1;
    bench.noinit_kept = noinit_pattern[0] == PATTERN && noinit_pattern[1] == ~PATTERN;
    bench.debugger_attached = CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk ? 1 : 0;
    BKP_PROBE->pattern = PATTERN;
    BKP_PROBE->boots = bench.boot_count;
    noinit_pattern[0] = PATTERN;
    noinit_pattern[1] = ~PATTERN;

    /* CYCCNT vs SysTick (both core clock, but SysTick is not a debug
     * component): proves DWT counts with or without a probe. */
    SysTick->LOAD = SB_SYSCLK_HZ / 1000u - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
    while (!(SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk))
        ;
    uint32_t t0 = sb_cyc();
    while (!(SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk))
        ;
    uint32_t d = cyc_since(t0);
    SysTick->CTRL = 0;
    bench.cyccnt_counts = d > SB_SYSCLK_HZ / 1000u * 9 / 10 && d < SB_SYSCLK_HZ / 1000u * 11 / 10;
}

static uint32_t time_sha(uint32_t addr, uint32_t len)
{
    uint8_t out[32];
    sb_sha256_ctx c;
    uint32_t t0 = sb_cyc();
    sb_sha256_init(&c);
    sb_sha256_update(&c, (const void *)addr, len);
    sb_sha256_final(&c, out);
    return cyc_since(t0);
}

/* Paint the stack below SP, run fn, and report how deep it reached. */
static uint32_t stack_depth(void (*fn)(void), uint32_t span)
{
    uint32_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    volatile uint32_t *lo = (volatile uint32_t *)((sp - 64u - span) & ~3u);
    for (volatile uint32_t *p = lo; p < (volatile uint32_t *)(sp - 64u); p++)
        *p = 0xDEADBEEFu;
    fn();
    volatile uint32_t *p = lo;
    while (p < (volatile uint32_t *)(sp - 64u) && *p == 0xDEADBEEFu)
        p++;
    return (sp - 64u) - (uint32_t)p + 64u;
}

static uint8_t msg[BV_MSG_LEN];
static int ed_result;

static void run_verify(void)
{
    ed_result = crypto_ed25519_check(bv_sig, bv_pub, msg, BV_MSG_LEN);
}

static void run_benchmarks(void)
{
    memset((void *)&bench.sha_64k_cyc, 0, offsetof(bench_t, boot_count) - offsetof(bench_t, sha_64k_cyc));
    bench.hz = SB_SYSCLK_HZ;

    bench.sha_64k_cyc = time_sha(SB_SLOT_A_ADDR, 64u * 1024u);
    bench.sha_384k_cyc = time_sha(SB_SLOT_A_ADDR, SB_SLOT_SIZE);
    uint32_t t0 = sb_cyc();
    (void)sb_crc32(0, (const void *)SB_SLOT_A_ADDR, 1024);
    bench.crc32_1k_cyc = cyc_since(t0);

    for (uint32_t i = 0; i < BV_MSG_LEN; i++)
        msg[i] = BV_MSG_BYTE(i);
    t0 = sb_cyc();
    run_verify();
    bench.ed_verify_cyc = cyc_since(t0);
    bench.ed_ok = ed_result == 0;
    bench.ed_stack_bytes = stack_depth(run_verify, 16u * 1024u);
    msg[100] ^= 1;
    run_verify();
    bench.ed_tampered_rejected = ed_result != 0;
    msg[100] ^= 1;

    int ok = 1;
    t0 = sb_cyc();
    ok &= sb_flash_erase_sector(3) == 0;
    bench.erase16_cyc = cyc_since(t0);
    t0 = sb_cyc();
    ok &= sb_flash_erase_sector(4) == 0;
    bench.erase64_cyc = cyc_since(t0);
    t0 = sb_cyc();
    ok &= sb_flash_erase_sector(11) == 0;
    bench.erase128_cyc = cyc_since(t0);
    for (uint32_t a = SB_RECOVERY_ADDR; a < SB_RECOVERY_ADDR + SB_RECOVERY_SIZE; a += 4)
        ok &= *(volatile uint32_t *)a == 0xFFFFFFFFu;
    bench.erase_ok = ok;

    static uint32_t buf[4096];
    for (uint32_t i = 0; i < 4096; i++)
        buf[i] = i * 2654435761u;
    t0 = sb_cyc();
    int rc = sb_flash_program(SB_RECOVERY_ADDR, buf, sizeof buf);
    bench.prog16k_cyc = cyc_since(t0);
    bench.prog_ok = rc == 0 && memcmp((const void *)SB_RECOVERY_ADDR, buf, sizeof buf) == 0;

    /* Journal on real flash: empty scan, one append, full-page scan, and the
     * append that compacts (includes a 16 KB erase). */
    ok = sb_flash_erase_sector(SB_JOURNAL_SECTOR0) == 0 && sb_flash_erase_sector(SB_JOURNAL_SECTOR1) == 0;
    sb_journal_t j;
    sb_record_t r;
    t0 = sb_cyc();
    sb_journal_scan(&j);
    bench.jscan_empty_cyc = cyc_since(t0);
    sb_record_init(&r);
    t0 = sb_cyc();
    ok &= sb_journal_append(&j, &r) == 0;
    bench.jappend_cyc = cyc_since(t0);
    while (j.next_off + SB_REC_SIZE <= SB_JOURNAL_PAGE_SIZE) {
        r.min_version++;
        ok &= sb_journal_append(&j, &r) == 0;
    }
    t0 = sb_cyc();
    sb_journal_scan(&j);
    bench.jscan_full_cyc = cyc_since(t0);
    r.min_version++;
    t0 = sb_cyc();
    ok &= sb_journal_append(&j, &r) == 0;
    bench.jcompact_cyc = cyc_since(t0);
    sb_journal_scan(&j);
    ok &= j.have_cur && j.cur.min_version == r.min_version && j.page == 1;
    bench.journal_ok = ok;
    bench.magic = 0xBE7C0001u;
}

static uint32_t us(uint32_t c)
{
    return (uint32_t)(((uint64_t)c * 1000000u + SB_SYSCLK_HZ / 2) / SB_SYSCLK_HZ);
}

int main(void)
{
    bench.magic = 0;
    sb_clock_init();
    sb_rtt_init();
    sb_leds_init();
    retention_check();
    sb_printf("\r\n[bench] boot %u  csr=0x%08x bkpsram_kept=%u noinit_kept=%u debugger=%u cyccnt=%u\r\n",
              bench.boot_count, bench.rcc_csr, bench.bkpsram_kept, bench.noinit_kept,
              bench.debugger_attached, bench.cyccnt_counts);
    sb_led(LED_ORANGE, 1);
    run_benchmarks();
    sb_led(LED_ORANGE, 0);
    sb_printf("[bench] sha256 64K %u us, 384K %u us; ed25519 verify %u us (ok=%d tamper_rej=%d stack=%u B)\r\n",
              us(bench.sha_64k_cyc), us(bench.sha_384k_cyc), us(bench.ed_verify_cyc),
              (int)bench.ed_ok, (int)bench.ed_tampered_rejected, bench.ed_stack_bytes);
    sb_printf("[bench] erase 16K %u us 64K %u us 128K %u us; program 16K %u us (ok=%d)\r\n",
              us(bench.erase16_cyc), us(bench.erase64_cyc), us(bench.erase128_cyc),
              us(bench.prog16k_cyc), (int)bench.prog_ok);
    sb_printf("[bench] journal scan empty %u us, full %u us; append %u us, compacting append %u us (ok=%d)\r\n",
              us(bench.jscan_empty_cyc), us(bench.jscan_full_cyc), us(bench.jappend_cyc),
              us(bench.jcompact_cyc), (int)bench.journal_ok);

    for (uint32_t n = 0;; n++) {
        sb_led(LED_GREEN, n & 1u);
        uint32_t cmd = bench_cmd;
        if (cmd) {
            bench_cmd = 0;
            if (cmd == 1)
                NVIC_SystemReset();
            if (cmd == 2) {
                sb_iwdg_start(100);
                for (;;)
                    ;
            }
            if (cmd == 3)
                run_benchmarks();
        }
        sb_delay_cycles(SB_SYSCLK_HZ / 8);
    }
}
