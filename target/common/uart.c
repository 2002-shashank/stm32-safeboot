#include "uart.h"

#include "stm32f407xx.h"

#define APB1_HZ 42000000u

static uint8_t *ring;
static uint32_t ring_size, rd, max_fill, errors;

static uint32_t brr_for(uint32_t baud)
{
    /* Oversampling by 16: BRR = APB1 / baud, as 12.4 fixed point. */
    return (APB1_HZ + baud / 2u) / baud;
}

uint32_t sb_uart_actual_baud(uint32_t baud)
{
    return APB1_HZ / brr_for(baud);
}

void sb_uart_init(uint32_t baud, uint8_t *rx_buf, uint32_t rx_size)
{
    ring = rx_buf;
    ring_size = rx_size;
    rd = max_fill = errors = 0;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_DMA1EN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;

    /* PA2/PA3: AF7, pull-up on RX so a disconnected line idles high. */
    GPIOA->MODER = (GPIOA->MODER & ~(0xFu << 4)) | (0xAu << 4);
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFFu << 8)) | (0x77u << 8);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 6)) | (1u << 6);
    GPIOA->OSPEEDR |= 3u << 4;

    USART2->CR1 = 0;
    USART2->BRR = brr_for(baud);
    USART2->CR3 = USART_CR3_DMAR;

    DMA1_Stream5->CR = 0;
    while (DMA1_Stream5->CR & DMA_SxCR_EN)
        ;
    DMA1->HIFCR = 0x3Du << 6;   /* clear all stream 5 flags */
    DMA1_Stream5->PAR = (uint32_t)&USART2->DR;
    DMA1_Stream5->M0AR = (uint32_t)rx_buf;
    DMA1_Stream5->NDTR = rx_size;
    DMA1_Stream5->CR = (4u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC | DMA_SxCR_CIRC |
                       (2u << DMA_SxCR_PL_Pos) | DMA_SxCR_EN;

    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void sb_uart_set_baud(uint32_t baud)
{
    while (!(USART2->SR & USART_SR_TC))
        ;
    USART2->CR1 &= ~USART_CR1_UE;
    USART2->BRR = brr_for(baud);
    USART2->CR1 |= USART_CR1_UE;
}

void sb_uart_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        while (!(USART2->SR & USART_SR_TXE))
            ;
        USART2->DR = *p++;
    }
    while (!(USART2->SR & USART_SR_TC))
        ;
}

static void check_errors(void)
{
    uint32_t sr = USART2->SR;
    if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
        errors++;
        /* The flags clear on an SR read followed by a DR read. If a byte is
         * pending (RXNE), the DMA's own DR read completes the sequence;
         * reading DR here would steal that byte from the ring. */
        if (!(sr & USART_SR_RXNE))
            (void)USART2->DR;
    }
}

uint32_t sb_uart_available(void)
{
    check_errors();
    uint32_t wr = ring_size - DMA1_Stream5->NDTR;
    if (wr == ring_size)
        wr = 0;
    uint32_t n = wr >= rd ? wr - rd : ring_size - rd + wr;
    if (n > max_fill)
        max_fill = n;
    return n;
}

size_t sb_uart_read(void *buf, size_t cap)
{
    uint8_t *p = buf;
    size_t n = sb_uart_available();
    if (n > cap)
        n = cap;
    for (size_t i = 0; i < n; i++) {
        p[i] = ring[rd];
        if (++rd == ring_size)
            rd = 0;
    }
    return n;
}

uint32_t sb_uart_max_fill(void)
{
    return max_fill;
}

uint32_t sb_uart_errors(void)
{
    return errors;
}
