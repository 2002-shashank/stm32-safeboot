/* STM32F407G-DISC1 board support shared by bootloader, app and bench. */
#ifndef SB_BOARD_H
#define SB_BOARD_H

#include <stdint.h>

#include "sb/crash.h"
#include "stm32f407xx.h"

#define SB_HSI_HZ 16000000u
#define SB_SYSCLK_HZ 168000000u

/* LEDs on PD12..PD15. */
enum { LED_GREEN = 12, LED_ORANGE = 13, LED_RED = 14, LED_BLUE = 15 };

/* Switch to the 8 MHz HSE crystal and PLL at 168 MHz (APB1 42, APB2 84,
 * 5 wait states, ART caches on). Falls back to HSI-based PLL if HSE fails.
 * Returns 1 if HSE is used. */
int sb_clock_init(void);

static inline void sb_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t sb_cyc(void)
{
    return DWT->CYCCNT;
}

void sb_leds_init(void);
void sb_led(int pin, int on);

/* Independent watchdog: timeout in ms (max ~8 s with the LSI's 32 kHz
 * nominal, which varies 17-47 kHz). Frozen while the core is halted by a
 * debugger. Cannot be stopped once started. */
void sb_iwdg_start(uint32_t timeout_ms);
static inline void sb_iwdg_kick(void)
{
    IWDG->KR = 0xAAAAu;
}

/* Enable access to the 4 KB backup SRAM (backup regulator on, so it is kept
 * in VBAT mode if VBAT is powered) and return the shared area. */
sb_shared_t *sb_bkpsram_enable(void);

/* Busy-wait using CYCCNT. */
void sb_delay_cycles(uint32_t cycles);

#endif
