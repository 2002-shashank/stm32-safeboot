/*
 * Demo/test application. Built per slot and per behavior:
 *   APP_BEHAVIOR 0: normal; confirms after init.
 *   APP_BEHAVIOR 1: faults during init, before confirming (rollback test).
 *   APP_BEHAVIOR 2: hangs during init without kicking the IWDG (rollback test).
 * Commands over the RTT down channel (or the SWD mailbox `app_cmd`):
 *   b = precise bus fault (load from unmapped memory; BFAR valid),
 *   w = imprecise bus fault (buffered store; BFAR not valid),
 *   d = divide by zero, u = undefined instruction,
 *   a = app assert, h = hang (stop kicking IWDG), r = soft reset, i = info,
 *   U = reboot into the bootloader's UART update mode.
 * On USART2 (PA2/PA3, 115200) the app listens for "SBUPDATE", which does the
 * same as U, so a host can start an update over the serial link alone.
 */
#include <string.h>

#include "board.h"
#include "fault.h"
#include "rtt.h"
#include "sb/crash.h"
#include "sb/crc32.h"
#include "sb/layout.h"
#include "sb_api.h"
#include "uart.h"

#ifndef APP_VERSION
#define APP_VERSION 1
#endif
#ifndef APP_BEHAVIOR
#define APP_BEHAVIOR 0
#endif

#ifdef APP_FILLER_BYTES
/* Padding to bring the image near the 392,704 B slot limit for worst-case
 * boot timing (SHA-256 time depends only on length, not content). */
static const uint8_t filler[APP_FILLER_BYTES] = {1};
#endif

volatile uint32_t app_cmd;          /* SWD mailbox: host writes a command char */
volatile int32_t app_confirm_rc = 0x7FFFFFFF;
volatile uint32_t app_alive;

static uint32_t us(uint32_t cycles, uint32_t hz)
{
    return (uint32_t)(((uint64_t)cycles * 1000000u + hz / 2) / hz);
}

static void print_boot_timing(const sb_bootinfo_t *bi)
{
    if (bi->magic != SB_BOOTINFO_MAGIC) {
        sb_printf("[app] no boot info (started without the bootloader?)\r\n");
        return;
    }
    const uint32_t *c = bi->cyc;
    uint32_t sha = c[SB_CP_SHA] - bi->sha_start;
    uint32_t sig = c[SB_CP_SIG] - c[SB_CP_SHA];
    sb_printf("[app] boot timing (DWT; reset->Reset_Handler not visible to DWT):\r\n");
    sb_printf("  Reset_Handler->main  %6u us  (HSI 16 MHz)\r\n", us(c[SB_CP_MAIN], SB_HSI_HZ));
    sb_printf("  clock init           %6u us\r\n", us(c[SB_CP_CLOCK] - c[SB_CP_MAIN], SB_HSI_HZ));
    sb_printf("  journal scan         %6u us\r\n", us(c[SB_CP_JOURNAL] - c[SB_CP_CLOCK], SB_SYSCLK_HZ));
    sb_printf("  SHA-256 (last image) %6u us\r\n", us(sha, SB_SYSCLK_HZ));
    sb_printf("  Ed25519 verify       %6u us\r\n", us(sig, SB_SYSCLK_HZ));
    sb_printf("  handoff              %6u us\r\n", us(c[SB_CP_APP] - c[SB_CP_HANDOFF], SB_SYSCLK_HZ));
    uint32_t total = us(c[SB_CP_MAIN] + (c[SB_CP_CLOCK] - c[SB_CP_MAIN]), SB_HSI_HZ) +
                     us(c[SB_CP_APP] - c[SB_CP_CLOCK], SB_SYSCLK_HZ);
    sb_printf("  total to app main    %6u us\r\n", total);
}

static void do_command(char ch)
{
    switch (ch) {
    case 'b':
        sb_printf("[app] precise bus fault: load from unmapped 0xCCCCCCC0\r\n");
        (void)*(volatile uint32_t *)0xCCCCCCC0u;
        break;
    case 'w':
        sb_printf("[app] imprecise bus fault: store to unmapped 0xCCCCCCC0\r\n");
        *(volatile uint32_t *)0xCCCCCCC0u = 1;
        break;
    case 'd': {
        sb_printf("[app] divide by zero\r\n");
        volatile uint32_t z = 0;
        volatile uint32_t q = 10u / z;
        (void)q;
        break;
    }
    case 'u':
        sb_printf("[app] undefined instruction\r\n");
        __asm volatile("udf #0");
        break;
    case 'a':
        sb_printf("[app] assert\r\n");
        sb_fault_app_assert(0xA55E7);
    case 'h':
        sb_printf("[app] hanging (no IWDG kicks)\r\n");
        __disable_irq();
        for (;;)
            ;
    case 'r':
        NVIC_SystemReset();
    case 'U': {
        const sb_api_t *api = sb_api_v(2);
        sb_printf("[app] update requested: rebooting into the bootloader\r\n");
        if (api)
            api->boot_request_update();
        sb_printf("[app] bootloader has no update API (v1)\r\n");
        break;
    }
    case 'i': {
        const sb_api_t *api = sb_api();
        if (api)
            print_boot_timing(api->boot_info());
        break;
    }
    default:
        break;
    }
}

int main(void)
{
    uint32_t t_app = sb_cyc();
    sb_shared_t *sh = sb_bkpsram_enable();
    sb_bootinfo_t *bi = &sh->info;
    if (bi->magic == SB_BOOTINFO_MAGIC) {
        bi->cyc[SB_CP_APP] = t_app;
        bi->crc = sb_crc32(0, bi, offsetof(sb_bootinfo_t, crc));
    }
    sb_rtt_init();
    sb_leds_init();

    uint32_t slot = SCB->VTOR == SB_SLOT_B_ADDR + SB_HDR_SIZE ? 1 : 0;
    sb_fault_init(APP_VERSION, slot);
    int led = slot ? LED_BLUE : LED_GREEN;
    sb_printf("[app] v%u running from slot %c (behavior %u)\r\n", APP_VERSION, 'A' + (int)slot,
              APP_BEHAVIOR);
    print_boot_timing(bi);
#ifdef APP_FILLER_BYTES
    /* Runtime reference keeps --gc-sections from dropping the filler. */
    volatile uint32_t last = APP_FILLER_BYTES - 1;
    sb_printf("[app] image carries %u B of filler (last byte %u)\r\n", APP_FILLER_BYTES,
              filler[last]);
#endif

#if APP_BEHAVIOR == 1
    sb_printf("[app] simulated init failure: faulting before boot_confirm()\r\n");
    (void)*(volatile uint32_t *)0xCCCCCCC0u;
#elif APP_BEHAVIOR == 2
    sb_printf("[app] simulated init hang: not kicking IWDG\r\n");
    __disable_irq();
    for (;;)
        ;
#endif

    /* Init is done and healthy: end the trial. */
    const sb_api_t *api = sb_api();
    app_confirm_rc = api ? api->boot_confirm() : -100;
    sb_printf("[app] boot_confirm() = %d\r\n", (int)app_confirm_rc);

    static uint8_t uart_ring[256];
    static const char magic[] = "SBUPDATE";
    unsigned matched = 0;
    sb_uart_init(115200, uart_ring, sizeof uart_ring);

    for (uint32_t n = 0;; n++) {
        uint8_t b;
        while (sb_uart_read(&b, 1)) {
            matched = b == (uint8_t)magic[matched] ? matched + 1 : b == (uint8_t)magic[0];
            if (matched == sizeof magic - 1)
                do_command('U');
        }
        sb_iwdg_kick();
        sb_led(led, (n & 1u) == 0);
        app_alive = n;
        char ch;
        if (sb_rtt_read(&ch, 1))
            do_command(ch);
        if (app_cmd) {
            char c2 = (char)app_cmd;
            app_cmd = 0;
            do_command(c2);
        }
        sb_delay_cycles(SB_SYSCLK_HZ / 4);
    }
}
