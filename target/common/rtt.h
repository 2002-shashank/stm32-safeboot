/*
 * Minimal SEGGER-RTT-compatible logging over SWD (no UART needed).
 * One up channel (target -> host log) and one down channel (host -> target
 * commands), at the fixed address SB_RTT_ADDR shared by bootloader and app.
 * Host: openocd ... -c "rtt setup 0x2001F000 0x100 \"SEGGER RTT\"; rtt start;
 *                       rtt server start 9090 0"
 */
#ifndef SB_RTT_H
#define SB_RTT_H

#include <stddef.h>
#include <stdint.h>

/* Initialize, or adopt an intact block left by the previous stage so the
 * host's read position carries over from bootloader to app. */
void sb_rtt_init(void);

/* Non-blocking: data that does not fit is dropped (never stalls without a
 * debugger). Returns bytes written. */
size_t sb_rtt_write(const void *data, size_t len);

/* Read up to cap bytes sent by the host. Returns bytes read. */
size_t sb_rtt_read(void *buf, size_t cap);

/* printf subset: %s %c %u %d %x %X, with optional 0-padding width (%08x).
 * Every numeric argument is read as 32 bits (uint32_t/int32_t/int), so no
 * format attribute: GCC would insist on %lu for uint32_t on this target. */
void sb_printf(const char *fmt, ...);

#endif
