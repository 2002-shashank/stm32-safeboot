#include "board.h"

static int wait_flag(volatile uint32_t *reg, uint32_t mask, uint32_t cycles)
{
    uint32_t t0 = sb_cyc();
    while (!(*reg & mask))
        if (sb_cyc() - t0 > cycles)
            return 0;
    return 1;
}

int sb_clock_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR |= PWR_CR_VOS;   /* scale 1, required for 168 MHz */

    RCC->CR |= RCC_CR_HSEON;
    int hse = wait_flag(&RCC->CR, RCC_CR_HSERDY, SB_HSI_HZ / 20);   /* 50 ms */

    /* VCO input 1 MHz, VCO 336 MHz, SYSCLK = 336/2 = 168 MHz, 48 MHz = 336/7. */
    uint32_t m = hse ? 8u : 16u;
    RCC->PLLCFGR = m << RCC_PLLCFGR_PLLM_Pos | 336u << RCC_PLLCFGR_PLLN_Pos |
                   0u << RCC_PLLCFGR_PLLP_Pos | 7u << RCC_PLLCFGR_PLLQ_Pos |
                   (hse ? RCC_PLLCFGR_PLLSRC_HSE : RCC_PLLCFGR_PLLSRC_HSI);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;

    FLASH->ACR = FLASH_ACR_LATENCY_5WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    RCC->CFGR = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2 | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;
    if (!hse)
        RCC->CR &= ~RCC_CR_HSEON;
    return hse;
}

void sb_leds_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    (void)RCC->AHB1ENR;
    GPIOD->MODER = (GPIOD->MODER & ~(0xFFu << 24)) | (0x55u << 24);
}

void sb_led(int pin, int on)
{
    GPIOD->BSRR = on ? 1u << pin : 1u << (pin + 16);
}

void sb_iwdg_start(uint32_t timeout_ms)
{
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
    /* LSI 32 kHz / 64 = 500 Hz -> 2 ms per tick, 12-bit reload. */
    uint32_t reload = timeout_ms / 2u;
    if (reload > 0xFFFu)
        reload = 0xFFFu;
    if (reload == 0)
        reload = 1;
    IWDG->KR = 0xCCCCu;   /* start (also starts the LSI) */
    IWDG->KR = 0x5555u;   /* unlock PR/RLR */
    IWDG->PR = 4u;        /* /64 */
    IWDG->RLR = reload;
    while (IWDG->SR)
        ;
    IWDG->KR = 0xAAAAu;
}

sb_shared_t *sb_bkpsram_enable(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    RCC->AHB1ENR |= RCC_AHB1ENR_BKPSRAMEN;
    (void)RCC->AHB1ENR;
    PWR->CR |= PWR_CR_DBP;
    PWR->CSR |= PWR_CSR_BRE;
    uint32_t t0 = sb_cyc();
    while (!(PWR->CSR & PWR_CSR_BRR) && sb_cyc() - t0 < SB_SYSCLK_HZ / 100)
        ;
    return (sb_shared_t *)SB_BKPSRAM_ADDR;
}

void sb_delay_cycles(uint32_t cycles)
{
    uint32_t t0 = sb_cyc();
    while (sb_cyc() - t0 < cycles)
        ;
}
