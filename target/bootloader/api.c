/*
 * API table at SB_API_ADDR. These functions are called from the app: they
 * must not touch the bootloader's .data/.bss (the app owns that RAM). The
 * journal and flash code they use is stateless; the RAM-resident flash
 * routines are re-copied into the reserved area on every call.
 */
#include <string.h>

#include "sb/boot.h"
#include "sb/journal.h"
#include "sb/layout.h"
#include "sb_api.h"
#include "stm32f407xx.h"

extern uint32_t _siramfunc, _sramfunc, _eramfunc;

static void ramfunc_install(void)
{
    memcpy(&_sramfunc, &_siramfunc, (size_t)((uint8_t *)&_eramfunc - (uint8_t *)&_sramfunc));
    __DSB();
    __ISB();
}

static int api_boot_confirm(void)
{
    unsigned slot;
    if (SCB->VTOR == SB_SLOT_A_ADDR + SB_HDR_SIZE)
        slot = 0;
    else if (SCB->VTOR == SB_SLOT_B_ADDR + SB_HDR_SIZE)
        slot = 1;
    else
        return -1;
    ramfunc_install();
    sb_journal_t j;
    return sb_boot_confirm(&j, slot);
}

static const sb_bootinfo_t *api_boot_info(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_BKPSRAMEN;
    (void)RCC->AHB1ENR;
    return &((const sb_shared_t *)SB_BKPSRAM_ADDR)->info;
}

static void api_boot_request_update(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    RCC->AHB1ENR |= RCC_AHB1ENR_BKPSRAMEN;
    (void)RCC->AHB1ENR;
    PWR->CR |= PWR_CR_DBP;
    ((sb_shared_t *)SB_BKPSRAM_ADDR)->request = SB_REQ_UPDATE;
    __DSB();
    NVIC_SystemReset();
}

__attribute__((section(".sb_api"), used)) const sb_api_t sb_api_table = {
    .magic = SB_API_MAGIC,
    .version = SB_API_VERSION,
    .boot_confirm = api_boot_confirm,
    .boot_info = api_boot_info,
    .boot_request_update = api_boot_request_update,
};
