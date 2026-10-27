/*
 * USART2 on PA2 (TX) / PA3 (RX), wired to an external 3.3 V USB-serial
 * bridge (the DISC1's ST-LINK VCP is not connected to the target).
 *
 * RX runs on DMA1 Stream5 (channel 4) into a circular buffer, so reception
 * continues while the CPU is stalled on a flash erase or program. TX is
 * polled and must not be called while flash is busy.
 */
#ifndef SB_UART_H
#define SB_UART_H

#include <stddef.h>
#include <stdint.h>

/* rx_buf/rx_size: DMA ring (any size <= 65535). Requires sb_clock_init(). */
void sb_uart_init(uint32_t baud, uint8_t *rx_buf, uint32_t rx_size);

/* Change the baud rate (waits for TX to drain). RX data in the ring is kept. */
void sb_uart_set_baud(uint32_t baud);

/* Actual baud rate after integer/fraction rounding of the divider. */
uint32_t sb_uart_actual_baud(uint32_t baud);

void sb_uart_write(const void *data, size_t len);

/* Bytes received and not yet read. */
uint32_t sb_uart_available(void);

/* Read up to cap bytes from the ring. Returns bytes read. */
size_t sb_uart_read(void *buf, size_t cap);

/* Highest ring fill level seen by sb_uart_available() (diagnostics). */
uint32_t sb_uart_max_fill(void);

/* Framing/noise/overrun errors seen (reading clears the USART flags). */
uint32_t sb_uart_errors(void);

#endif
