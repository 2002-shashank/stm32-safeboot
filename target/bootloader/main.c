/*
 * safeboot bootloader: reset reason and crash report, journal scan, boot
 * decision with full SHA-256 + Ed25519 verification, trial watchdog, and
 * handoff. DWT checkpoints go into the boot info in backup SRAM; the app
 * reads them and prints the breakdown.
 */
#include <string.h>

#include "board.h"
#include "rtt.h"
#include "sb/boot.h"
#include "sb/crash.h"
#include "sb/crc32.h"
#include "sb/image.h"
#include "sb/journal.h"
#include "sb/layout.h"

#define SB_TRIAL_WDG_MS 4000u

static sb_shared_t *sh;

static const char *const reason_str[] = {
    "?", "confirmed", "trial", "rollback", "adopted", "promoted", "recovery", "nothing",
    "journal-error",
};
static const char *const state_str[] = {
    "?", "EMPTY", "VALID", "PENDING", "TRIAL", "CONFIRMED", "INVALID", "ROLLBACK",
};

static const char *st(uint8_t s)
{
    return s < sizeof state_str / sizeof *state_str ? state_str[s] : "?";
}

static void mark(int m)
{
    uint32_t now = sb_cyc();
    if (m == SB_MARK_SHA_START)
        sh->info.sha_start = now;
    else if (m == SB_MARK_SHA_END)
        sh->info.cyc[SB_CP_SHA] = now;
    else if (m == SB_MARK_SIG_END)
        sh->info.cyc[SB_CP_SIG] = now;
}

static void print_reset(uint32_t rr)
{
    static const char *const names[7] = {"BOR", "PIN", "POR", "SFT", "IWDG", "WWDG", "LPWR"};
    for (int i = 0; i < 7; i++)
        if (rr >> i & 1u)
            sb_printf(" %s", names[i]);
}

static void report_crash(uint32_t rr)
{
    sb_crash_t *c = &sh->crash;
    if (!sb_crash_valid(c) || c->consumed)
        return;
    static char buf[640];
    c->reset_reason = rr;
    sb_crash_format(c, buf, sizeof buf);
    sb_printf("[sb] previous run crashed:\r\n%s", buf);
    c->consumed = 1;   /* outside the CRC: the record stays valid for the app */
}

static __attribute__((noreturn)) void update_mode(const char *why)
{
    sb_printf("[sb] staying in bootloader after %u us: %s\r\n",
              (uint32_t)((uint64_t)sb_cyc() * 1000000u / SB_SYSCLK_HZ), why);
    sb_printf("[sb] UART update mode not available yet (waiting for USB-serial adapter)\r\n");
    for (;;) {
        sb_led(LED_RED, 1);
        sb_delay_cycles(SB_SYSCLK_HZ / 10);
        sb_led(LED_RED, 0);
        sb_delay_cycles(SB_SYSCLK_HZ / 10 * 9);
    }
}

static __attribute__((noreturn)) void jump(uint32_t vt)
{
    const uint32_t *v = (const uint32_t *)vt;
    __disable_irq();
    SysTick->CTRL = 0;
    for (unsigned i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    SCB->VTOR = vt;
    __DSB();
    __ISB();
    /* All IRQs are disabled at the NVIC; the app starts with PRIMASK clear,
     * as after a real reset. */
    __enable_irq();
    __asm volatile("msr msp, %0\n"
                   "bx %1\n" ::"r"(v[0]),
                   "r"(v[1]));
    __builtin_unreachable();
}

int main(void)
{
    uint32_t t_main = sb_cyc();
    sh = sb_bkpsram_enable();
    uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;
    uint32_t rr = sb_reset_reason_from_csr(csr);
    int hse = sb_clock_init();
    uint32_t t_clock = sb_cyc();

    sb_rtt_init();
    sb_leds_init();

    sb_bootinfo_t *bi = &sh->info;
    uint32_t boots = bi->magic == SB_BOOTINFO_MAGIC ? bi->boot_count + 1 : 1;
    memset(bi, 0, sizeof *bi);
    bi->magic = SB_BOOTINFO_MAGIC;
    bi->boot_count = boots;
    bi->reset_reason = rr;
    bi->cyc[SB_CP_MAIN] = t_main;
    bi->hz[SB_CP_MAIN] = SB_HSI_HZ;
    bi->cyc[SB_CP_CLOCK] = t_clock;
    bi->hz[SB_CP_CLOCK] = SB_HSI_HZ;

    sb_printf("\r\n[sb] safeboot  boot #%u  reset:", boots);
    print_reset(rr);
    sb_printf("  clock: %s PLL 168 MHz\r\n", hse ? "HSE" : "HSI (HSE failed)");
    report_crash(rr);

    const sb_prov_t *prov = sb_prov_get();
    if (!prov)
        update_mode("device not provisioned (no public key in S4)");

    sb_journal_t j;
    sb_journal_scan(&j);
    bi->cyc[SB_CP_JOURNAL] = sb_cyc();
    bi->hz[SB_CP_JOURNAL] = SB_SYSCLK_HZ;
    if (j.have_cur)
        sb_printf("[sb] journal seq %u: A=%s v%u  B=%s v%u  active=%d cand=%d att=%u/%u min_ver=%u\r\n",
                  j.cur.seq, st(j.cur.state[0]), j.cur.version[0], st(j.cur.state[1]),
                  j.cur.version[1], j.cur.active == SB_SLOT_NONE ? -1 : (int)j.cur.active,
                  j.cur.candidate == SB_SLOT_NONE ? -1 : (int)j.cur.candidate, j.cur.attempts,
                  j.cur.max_attempts, j.cur.min_version);
    else
        sb_printf("[sb] journal empty\r\n");

    sb_boot_decision_t d;
    sb_boot_decide(&j, prov, mark, &d);
    bi->hz[SB_CP_SHA] = bi->hz[SB_CP_SIG] = SB_SYSCLK_HZ;
    bi->target = d.target;
    bi->reason = d.reason;
    bi->trial = d.trial;
    bi->attempts = d.attempts;
    bi->version = d.version;
    if (d.target == SB_BOOT_UPDATE_MODE)
        update_mode(d.reason == SB_R_JOURNAL_ERROR ? "journal write failed" : "no bootable image");

    sb_printf("[sb] boot %s v%u (", d.target == SB_BOOT_RECOVERY_IMAGE ? "recovery" :
                                    d.target == SB_BOOT_SLOT_A ? "slot A" : "slot B",
              d.version);
    if (d.reason != SB_R_TRIAL)
        sb_printf("%s%s", d.reason < sizeof reason_str / sizeof *reason_str ? reason_str[d.reason] : "?",
                  d.trial ? ", " : "");
    if (d.trial)
        sb_printf("trial %u/%u, IWDG %u ms", d.attempts, j.cur.max_attempts, SB_TRIAL_WDG_MS);
    sb_printf(")\r\n");

    if (d.trial)
        sb_iwdg_start(SB_TRIAL_WDG_MS);
    bi->hz[SB_CP_HANDOFF] = bi->hz[SB_CP_APP] = SB_SYSCLK_HZ;
    bi->cyc[SB_CP_HANDOFF] = sb_cyc();
    bi->crc = sb_crc32(0, bi, offsetof(sb_bootinfo_t, crc));
    jump(d.region_addr + SB_HDR_SIZE);
}
