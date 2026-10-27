/*
 * Phase 0 UART checks over the external USB-serial bridge on PA2/PA3.
 * Binary commands (little-endian u32 arguments), text replies:
 *   'B' baud       -> "OK <actual>\n" at the old rate, then switch
 *   'E' n          -> echo the next n bytes, then "ED <uart_errors>\n"
 *   'S' n sector   -> "SR\n"; on the first byte of the host's n-byte burst,
 *                     erase `sector` (CPU stalled, DMA keeps receiving), then
 *                     collect the rest and reply
 *                     "SD <erase_us> <during_erase> <received> <crc32> <errors> <max_fill>\n"
 * WARNING: 'S' erases the given flash sector (default test: S11).
 */
#include <string.h>

#include "board.h"
#include "sb/crc32.h"
#include "sb/flash.h"
#include "uart.h"

#define RING 49152u
static uint8_t ring[RING];

static void reply_u32s(const char *tag, const uint32_t *v, int n)
{
    char buf[96];
    size_t k = 0;
    for (const char *t = tag; *t;)
        buf[k++] = *t++;
    for (int i = 0; i < n; i++) {
        char tmp[11];
        int m = 0;
        uint32_t x = v[i];
        do {
            tmp[m++] = (char)('0' + x % 10);
            x /= 10;
        } while (x);
        buf[k++] = ' ';
        while (m)
            buf[k++] = tmp[--m];
    }
    buf[k++] = '\n';
    sb_uart_write(buf, k);
}

static uint8_t get_byte(void)
{
    uint8_t b;
    while (!sb_uart_read(&b, 1))
        ;
    return b;
}

static uint32_t get_u32(void)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)get_byte() << (8 * i);
    return v;
}

int main(void)
{
    sb_dwt_init();
    sb_clock_init();
    sb_leds_init();
    sb_uart_init(115200, ring, RING);
    sb_uart_write("UARTCHECK ready\n", 16);
    for (;;) {
        sb_led(LED_GREEN, 1);
        uint8_t cmd = get_byte();
        sb_led(LED_GREEN, 0);
        if (cmd == 'B') {
            uint32_t baud = get_u32();
            uint32_t actual = sb_uart_actual_baud(baud);
            reply_u32s("OK", &actual, 1);
            sb_uart_set_baud(baud);
        } else if (cmd == 'E') {
            uint32_t n = get_u32();
            uint8_t chunk[256];
            while (n) {
                size_t got = sb_uart_read(chunk, n < sizeof chunk ? n : sizeof chunk);
                sb_uart_write(chunk, got);
                n -= (uint32_t)got;
            }
            uint32_t e = sb_uart_errors();
            reply_u32s("ED", &e, 1);
        } else if (cmd == 'S') {
            uint32_t n = get_u32();
            uint32_t sector = get_u32();
            sb_uart_write("SR\n", 3);
            while (!sb_uart_available())
                ;
            sb_led(LED_ORANGE, 1);
            uint32_t t0 = sb_cyc();
            int rc = sb_flash_erase_sector(sector);
            uint32_t erase_cyc = sb_cyc() - t0;
            uint32_t during = sb_uart_available();
            sb_led(LED_ORANGE, 0);
            uint32_t crc = 0, received = 0, last = sb_cyc();
            uint8_t chunk[256];
            while (received < n && sb_cyc() - last < SB_SYSCLK_HZ * 2u) {
                size_t got = sb_uart_read(chunk, sizeof chunk);
                if (got) {
                    crc = sb_crc32(crc, chunk, got);
                    received += (uint32_t)got;
                    last = sb_cyc();
                }
            }
            uint32_t v[6] = {rc ? 0xFFFFFFFFu : erase_cyc / (SB_SYSCLK_HZ / 1000000u), during,
                             received, crc, sb_uart_errors(), sb_uart_max_fill()};
            reply_u32s("SD", v, 6);
        }
    }
}
