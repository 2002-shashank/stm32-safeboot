/*
 * Fault handlers: capture CFSR/HFSR/MMFAR/BFAR and the stacked exception
 * frame into the crash record in backup SRAM, then reset. The next boot's
 * bootloader validates and prints it. Linking this file overrides the weak
 * HardFault/MemManage/BusFault/UsageFault handlers.
 */
#include "fault.h"

#include "board.h"
#include "sb/crash.h"

static uint32_t g_version, g_slot;

void sb_fault_init(uint32_t image_version, uint32_t slot)
{
    g_version = image_version;
    g_slot = slot;
    /* Route MemManage/BusFault/UsageFault to their own handlers and trap
     * division by zero (unaligned access stays legal for normal code). */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
    SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;
}

/* type 0: take the exception number from IPSR. */
__attribute__((used, noreturn)) void sb_fault_record(uint32_t *frame, uint32_t exc_return,
                                                     uint32_t type)
{
    sb_shared_t *sh = sb_bkpsram_enable();
    sb_crash_t *c = &sh->crash;
    uint32_t count = sb_crash_valid(c) ? c->count + 1 : 1;
    c->count = count;
    c->fault_type = type ? type : __get_IPSR() & 0x1FFu;
    c->cfsr = SCB->CFSR;
    c->hfsr = SCB->HFSR;
    c->mmfar = SCB->MMFAR;
    c->bfar = SCB->BFAR;
    c->r0 = frame[0];
    c->r1 = frame[1];
    c->r2 = frame[2];
    c->r3 = frame[3];
    c->r12 = frame[4];
    c->lr = frame[5];
    c->pc = frame[6];
    c->xpsr = frame[7];
    c->exc_return = exc_return;
    /* Stack pointer before the 8-word frame (plus the alignment word). */
    c->sp = (uint32_t)frame + 32u + ((frame[7] & (1u << 9)) ? 4u : 0u);
    c->image_version = g_version;
    c->slot = g_slot;
    c->reset_reason = 0;
    c->consumed = 0;
    sb_crash_seal(c);
    __DSB();
    NVIC_SystemReset();
}

void sb_fault_app_assert(uint32_t code)
{
    /* Synthesize a frame so asserts share the record format: R0 = code,
     * PC = caller. */
    uint32_t frame[8] = {code, 0, 0, 0, 0, 0, (uint32_t)__builtin_return_address(0), 0};
    sb_fault_record(frame, 0, SB_FAULT_APP);
}

/* Choose MSP or PSP from EXC_RETURN bit 2 and pass the frame along. A stack
 * overflow into an invalid region would fault again on stacking; that case
 * escalates to a lockup-reset and leaves no record (documented limit). */
#define FAULT_TRAMPOLINE(name)                         \
    __attribute__((naked)) void name(void)             \
    {                                                  \
        __asm volatile("tst lr, #4\n"                  \
                       "ite eq\n"                      \
                       "mrseq r0, msp\n"               \
                       "mrsne r0, psp\n"               \
                       "mov r1, lr\n"                  \
                       "movs r2, #0\n"                 \
                       "b sb_fault_record\n");         \
    }

FAULT_TRAMPOLINE(HardFault_Handler)
FAULT_TRAMPOLINE(MemManage_Handler)
FAULT_TRAMPOLINE(BusFault_Handler)
FAULT_TRAMPOLINE(UsageFault_Handler)
