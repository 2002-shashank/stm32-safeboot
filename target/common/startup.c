/*
 * Startup and full F407 vector table (98 entries), shared by all images.
 *
 * SB_STARTUP_RESET_CYCCNT (bootloader only): zero DWT CYCCNT as the first
 * action, so boot checkpoints are measured from reset. The app must not
 * reset it: the bootloader leaves it running across the handoff.
 */
#include <stdint.h>

#include "stm32f407xx.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern uint32_t _siramfunc, _sramfunc, _eramfunc;

int main(void);

__attribute__((naked, noreturn)) void Reset_Handler(void)
{
    /* Naked: no prologue touches the stack before it is known good. */
    __asm volatile(
        "ldr r0, =_estack\n"
        "msr msp, r0\n"
        "b sb_reset_c\n");
}

__attribute__((used, noreturn)) void sb_reset_c(void)
{
#ifdef SB_STARTUP_RESET_CYCCNT
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
    for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;)
        *d++ = *s++;
    for (uint32_t *d = &_sbss; d < &_ebss;)
        *d++ = 0;
    for (uint32_t *s = &_siramfunc, *d = &_sramfunc; d < &_eramfunc;)
        *d++ = *s++;
    main();
    for (;;)
        ;
}

__attribute__((weak)) void Default_Handler(void)
{
    for (;;)
        ;
}

#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK_ALIAS;
void HardFault_Handler(void) WEAK_ALIAS;
void MemManage_Handler(void) WEAK_ALIAS;
void BusFault_Handler(void) WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void) WEAK_ALIAS;
void DebugMon_Handler(void) WEAK_ALIAS;
void PendSV_Handler(void) WEAK_ALIAS;
void SysTick_Handler(void) WEAK_ALIAS;
void WWDG_IRQHandler(void) WEAK_ALIAS;
void USART2_IRQHandler(void) WEAK_ALIAS;
void DMA1_Stream5_IRQHandler(void) WEAK_ALIAS;

typedef void (*vec_t)(void);

/* 16 system exceptions + 82 IRQs (RM0090 table 61). Unused IRQs share the
 * default handler; the few ever used get named weak symbols (they override
 * entries of the range initializer, hence the pragma). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used)) const vec_t sb_vectors[16 + 82] = {
    (vec_t)&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    [16 + WWDG_IRQn] = WWDG_IRQHandler,
    [16 + 1 ... 16 + 81] = Default_Handler,
    [16 + DMA1_Stream5_IRQn] = DMA1_Stream5_IRQHandler,
    [16 + USART2_IRQn] = USART2_IRQHandler,
};
#pragma GCC diagnostic pop
