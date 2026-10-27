/*
 * safeboot bootloader: reset reason and crash report, journal scan, boot
 * decision with full SHA-256 + Ed25519 verification, trial watchdog, and
 * handoff. DWT checkpoints go into the boot info in backup SRAM; the app
 * reads them and prints the breakdown.
 *
 * UART update mode (USART2 PA2/PA3, 115200 baud until the host sends
 * SET_BAUD) is entered when nothing is bootable, when the app requested it
 * (sb_api boot_request_update), or when the user button (PA0) is held at
 * reset. Entered by request or button, it resets after 30 s without traffic.
 */
#include <string.h>

#include "board.h"
#include "fault.h"
#include "rtt.h"
#include "sb/boot.h"
#include "sb/crash.h"
#include "sb/crc32.h"
#include "sb/image.h"
#include "sb/journal.h"
#include "sb/frame.h"
#include "sb/layout.h"
#include "sb/proto.h"
#include "uart.h"
#ifdef SB_FAULT_INJECT
#include "sb/fault_inject.h"
#define FI ((volatile sb_fi_t *)SB_FI_ADDR)
#endif

#define SB_TRIAL_WDG_MS 4000u
#define SB_UPDATE_BAUD 115200u
#define SB_UPDATE_IDLE_S 30u
#define SB_FRAME_GAP_MS 100u   /* inter-byte gap that abandons a partial frame */
#define SB_BAUD_REVERT_MS 3000u /* idle time after which the link returns to SB_UPDATE_BAUD */

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

static uint8_t uart_ring[4096];
static sb_frame_parser_t parser;
static sb_proto_t proto;
static uint32_t cur_baud = SB_UPDATE_BAUD;

static void send_frame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t out[SB_RESP_LEN + SB_FRAME_OVERHEAD];
    uint32_t n = sb_frame_encode(out, type, seq, payload, len);
    sb_uart_write(out, n);
}

static void handle_frame(const sb_frame_t *f)
{
    uint8_t resp[SB_RESP_LEN];
    uint8_t type = f->type | SB_RESP_FLAG;
    if (f->type == SB_CMD_SET_BAUD) {
        /* Transport-level: reply at the old rate, then switch. */
        memset(resp, 0, sizeof resp);
        uint32_t baud = f->len == 4 ? sb_get_le32(f->payload) : 0;
        int ok = baud >= 9600u && baud <= 1000000u;
        resp[0] = ok ? SB_S_OK : SB_S_ERR_LEN;
        sb_put_le32(resp + 8, ok ? sb_uart_actual_baud(baud) : 0);
        send_frame(type, f->seq, resp, sizeof resp);
        if (ok) {
            sb_uart_set_baud(baud);
            cur_baud = baud;
            sb_printf("[sb] uart %u baud\r\n", sb_uart_actual_baud(baud));
        }
        return;
    }
    sb_proto_handle(&proto, f->type, f->seq, f->payload, f->len, resp);
    send_frame(type, f->seq, resp, sizeof resp);
    if (f->type != SB_CMD_DATA && f->type != SB_CMD_QUERY_PROGRESS)
        sb_printf("[sb] cmd 0x%02x -> status %u next %u\r\n", f->type, resp[0], sb_get_le32(resp + 4));
    if (proto.reboot) {
        sb_printf("[sb] reboot requested\r\n");
        sb_delay_cycles(SB_SYSCLK_HZ / 100);
        NVIC_SystemReset();
    }
}

static __attribute__((noreturn)) void update_mode(const char *why, sb_journal_t *j,
                                                  const sb_prov_t *prov, int idle_reset)
{
#ifdef SB_FAULT_INJECT
    if (FI->magic == SB_FI_MAGIC)
        FI->recovery++;
#endif
    /* 32-bit divide: a 64-bit one would pull in 784 B of libgcc. */
    sb_printf("[sb] staying in bootloader after %u us: %s\r\n",
              sb_cyc() / (SB_SYSCLK_HZ / 1000000u), why);
    sb_uart_init(SB_UPDATE_BAUD, uart_ring, sizeof uart_ring);
    sb_proto_init(&proto, j, prov, mark);
    sb_printf("[sb] UART update mode on USART2 (PA2/PA3), %u baud\r\n", SB_UPDATE_BAUD);

    const uint32_t gap = SB_SYSCLK_HZ / 1000u * SB_FRAME_GAP_MS;
    const uint64_t idle_limit = (uint64_t)SB_SYSCLK_HZ * SB_UPDATE_IDLE_S;
    uint32_t prev = sb_cyc(), since_rx = 0;
    uint64_t idle = 0;   /* accumulated, so it survives CYCCNT wrap (25.6 s) */
    int partial = 0;
    for (uint32_t n = 0;; n++) {
        sb_iwdg_kick();   /* harmless if the IWDG is not running */
        uint32_t now = sb_cyc(), d = now - prev;
        prev = now;
        uint8_t buf[64];
        size_t got = sb_uart_read(buf, sizeof buf);
        if (got) {
            idle = 0;
            since_rx = 0;
        } else {
            idle += d;
            since_rx = since_rx + d > since_rx ? since_rx + d : 0xFFFFFFFFu;
        }
        if (partial && since_rx > gap) {
            sb_frame_reset(&parser);   /* host gave up mid-frame */
            partial = 0;
        }
        if (cur_baud != SB_UPDATE_BAUD && since_rx > SB_SYSCLK_HZ / 1000u * SB_BAUD_REVERT_MS) {
            /* Host went away at a higher rate: let a new session start at the default. */
            sb_uart_set_baud(SB_UPDATE_BAUD);
            cur_baud = SB_UPDATE_BAUD;
            sb_printf("[sb] link idle: back to %u baud\r\n", SB_UPDATE_BAUD);
        }
        for (size_t i = 0; i < got; i++) {
            partial = 1;
            if (sb_frame_feed(&parser, buf[i])) {
                partial = 0;
                handle_frame(&parser.f);
            }
        }
        if (idle_reset && idle > idle_limit) {
            sb_printf("[sb] update mode idle for %u s: rebooting\r\n", SB_UPDATE_IDLE_S);
            sb_delay_cycles(SB_SYSCLK_HZ / 100);
            NVIC_SystemReset();
        }
        sb_led(LED_RED, (n >> 17) & 1u);
    }
}

static int button_held(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    (void)RCC->AHB1ENR;
    /* B1 on PA0: external pull-down, reads 1 while pressed. Debounce. */
    for (int i = 0; i < 5; i++) {
        if (!(GPIOA->IDR & 1u))
            return 0;
        sb_delay_cycles(SB_SYSCLK_HZ / 1000);
    }
    return 1;
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
    /* A fault in the bootloader records a crash (slot 0xFF) and resets
     * instead of spinning in the default handler forever. */
    sb_fault_init(0, 0xFFu);

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
    static sb_journal_t j;   /* static: update mode keeps using it */
    sb_journal_scan(&j);
    bi->cyc[SB_CP_JOURNAL] = sb_cyc();
#ifdef SB_FAULT_INJECT
    if (FI->magic == SB_FI_MAGIC && FI->fired && !FI->post_valid) {
        FI->post_have = (uint32_t)j.have_cur;
        memcpy((void *)FI->post_rec, &j.cur, sizeof FI->post_rec);
        FI->post_valid = 1;
    }
#endif
    bi->hz[SB_CP_JOURNAL] = SB_SYSCLK_HZ;
    if (j.have_cur)
        sb_printf("[sb] journal seq %u: A=%s v%u  B=%s v%u  active=%d cand=%d att=%u/%u min_ver=%u\r\n",
                  j.cur.seq, st(j.cur.state[0]), j.cur.version[0], st(j.cur.state[1]),
                  j.cur.version[1], j.cur.active == SB_SLOT_NONE ? -1 : (int)j.cur.active,
                  j.cur.candidate == SB_SLOT_NONE ? -1 : (int)j.cur.candidate, j.cur.attempts,
                  j.cur.max_attempts, j.cur.min_version);
    else
        sb_printf("[sb] journal empty\r\n");

    if (!prov)
        update_mode("device not provisioned (no public key in S4)", &j, NULL, 0);
    int requested = sh->request == SB_REQ_UPDATE;
    sh->request = 0;
    if (requested)
        update_mode("update requested by the app", &j, prov, 1);
    if (button_held())
        update_mode("user button held at reset", &j, prov, 1);

    sb_boot_decision_t d;
    sb_boot_decide(&j, prov, mark, &d);
    bi->hz[SB_CP_SHA] = bi->hz[SB_CP_SIG] = SB_SYSCLK_HZ;
    bi->target = d.target;
    bi->reason = d.reason;
    bi->trial = d.trial;
    bi->attempts = d.attempts;
    bi->version = d.version;
    if (d.target == SB_BOOT_UPDATE_MODE)
        update_mode(d.reason == SB_R_JOURNAL_ERROR ? "journal write failed" : "no bootable image",
                    &j, prov, 0);

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
