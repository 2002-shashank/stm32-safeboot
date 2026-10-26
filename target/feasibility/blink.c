/*
 * Phase 0 feasibility firmware (register level, no HAL), runs on HSI 16 MHz.
 *  - F0.2: cycles the four user LEDs (PD12 green, PD13 orange, PD14 red, PD15 blue).
 *  - VCP wiring probe: prints an identifying line on every DISC1-safe USART
 *    TX pin (USART2 PA2, USART3 PD8, USART6 PC6) at 115200 8N1, so whichever
 *    one reaches the ST-LINK virtual COM port names itself.
 *  - F0.5: enables DWT CYCCNT from firmware and exposes it in `probe`.
 */
#include <stdint.h>

#include "stm32f407xx.h"

#define HSI_HZ 16000000u

/* Read back over SWD by the feasibility script. */
volatile struct {
    uint32_t magic;       /* 0xB11E0001 once running */
    uint32_t loops;
    uint32_t cyccnt;      /* DWT->CYCCNT sampled each loop */
    uint32_t cyc_per_loop;
} probe;

static void usart_init(USART_TypeDef *u)
{
    u->BRR = (HSI_HZ + 115200u / 2) / 115200u;   /* 139 -> 115108 baud */
    u->CR1 = USART_CR1_UE | USART_CR1_TE;
}

static void usart_puts(USART_TypeDef *u, const char *s)
{
    for (; *s; s++) {
        while (!(u->SR & USART_SR_TXE))
            ;
        u->DR = (uint8_t)*s;
    }
}

static void put_u32(char *p, uint32_t v)
{
    char t[10];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    *p = '\0';
}

static void say(USART_TypeDef *u, const char *who, uint32_t n)
{
    char num[11];
    put_u32(num, n);
    usart_puts(u, who);
    usart_puts(u, " hello ");
    usart_puts(u, num);
    usart_puts(u, "\r\n");
}

static void delay_cycles(uint32_t c)
{
    uint32_t t0 = DWT->CYCCNT;
    while (DWT->CYCCNT - t0 < c)
        ;
}

int main(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN | RCC_APB1ENR_USART3EN;
    RCC->APB2ENR |= RCC_APB2ENR_USART6EN;
    (void)RCC->APB2ENR;

    /* LEDs PD12-15: general-purpose outputs. */
    GPIOD->MODER = (GPIOD->MODER & ~(0xFFu << 24)) | (0x55u << 24);

    /* PA2 AF7 (USART2_TX), PD8 AF7 (USART3_TX), PC6 AF8 (USART6_TX). */
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 4)) | (2u << 4);
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFu << 8)) | (7u << 8);
    GPIOD->MODER = (GPIOD->MODER & ~(3u << 16)) | (2u << 16);
    GPIOD->AFR[1] = (GPIOD->AFR[1] & ~0xFu) | 7u;
    GPIOC->MODER = (GPIOC->MODER & ~(3u << 12)) | (2u << 12);
    GPIOC->AFR[0] = (GPIOC->AFR[0] & ~(0xFu << 24)) | (8u << 24);

    usart_init(USART2);
    usart_init(USART3);
    usart_init(USART6);

    probe.magic = 0xB11E0001u;
    for (uint32_t n = 0;; n++) {
        uint32_t t0 = DWT->CYCCNT;
        GPIOD->ODR = (GPIOD->ODR & ~(0xFu << 12)) | (1u << (12 + (n & 3u)));
        say(USART2, "USART2/PA2", n);
        say(USART3, "USART3/PD8", n);
        say(USART6, "USART6/PC6", n);
        delay_cycles(HSI_HZ / 4);
        probe.loops = n + 1;
        probe.cyccnt = DWT->CYCCNT;
        probe.cyc_per_loop = DWT->CYCCNT - t0;
    }
}

/* ---- minimal startup ---- */
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

void Reset_Handler(void)
{
    uint32_t *s = &_sidata, *d = &_sdata;
    while (d < &_edata)
        *d++ = *s++;
    for (d = &_sbss; d < &_ebss;)
        *d++ = 0;
    main();
    for (;;)
        ;
}

void Default_Handler(void)
{
    for (;;)
        ;
}

__attribute__((section(".isr_vector"), used)) static void (*const vectors[16])(void) = {
    (void (*)(void))&_estack, Reset_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, 0, 0, 0, 0,
    Default_Handler, Default_Handler, 0, Default_Handler, Default_Handler,
};
