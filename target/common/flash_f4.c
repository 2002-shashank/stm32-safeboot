/*
 * STM32F407 flash driver implementing sb/flash.h.
 *
 * The F407 has a single flash bank: any instruction fetch or data read from
 * flash stalls while an erase/program runs. The start-and-wait sequences
 * therefore live in .ramfunc (copied to SB_RAMFUNC_ADDR), and kick the IWDG
 * while waiting so a 2 s sector erase cannot trip the watchdog.
 *
 * Stateless by design: boot_confirm() calls into this code from app context,
 * where the bootloader's .data/.bss do not exist.
 */
#include "sb/flash.h"

#include "sb/layout.h"
#include "stm32f407xx.h"

#define RAMFUNC __attribute__((section(".ramfunc"), noinline, long_call))

/* SOP is the F407 header's name for the operation error flag (OPERR). */
#define FLASH_ERRORS (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR | \
                      FLASH_SR_SOP)

static void unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

static void lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* Starts the operation encoded in cr (with STRT for erase) and waits. For a
 * program, *dst = value is the write that starts it. Runs from RAM. */
RAMFUNC static uint32_t ram_run(uint32_t cr, volatile uint32_t *dst, uint32_t value)
{
    FLASH->CR = cr;
    if (dst)
        *dst = value;
    else
        FLASH->CR = cr | FLASH_CR_STRT;
    while (FLASH->SR & FLASH_SR_BSY)
        IWDG->KR = 0xAAAAu;
    FLASH->CR = 0;
    return FLASH->SR;
}

#ifdef SB_FAULT_INJECT
/*
 * Hardware fault injection (bootloader_fi build only). The host arms it over
 * SWD; the op counter lives in backup SRAM so it counts across resets, in
 * bootloader and app (boot_confirm) context alike. At op k the chip resets:
 *   mode 0: before the op, 1: mid-op (delay cycles after start), 2: after.
 * A reset during an erase/program aborts it, so mode 1 produces real torn
 * words and partially erased sectors on silicon.
 */
#include "rtt.h"
#include "sb/fault_inject.h"

#define FI ((volatile sb_fi_t *)SB_FI_ADDR)

RAMFUNC static void ram_run_and_reset(uint32_t cr, volatile uint32_t *dst, uint32_t value,
                                      uint32_t delay)
{
    FLASH->CR = cr;
    if (dst)
        *dst = value;
    else
        FLASH->CR = cr | FLASH_CR_STRT;
    uint32_t t0 = DWT->CYCCNT;
    while (DWT->CYCCNT - t0 < delay)
        ;
    SCB->AIRCR = (0x5FAu << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
    for (;;)
        ;
}

/* Returns the mode to apply to this op, or -1 for a normal op. */
static int fi_check(uint32_t addr)
{
    if (FI->magic != SB_FI_MAGIC || FI->fired)
        return -1;
    uint32_t idx = FI->count++;
    if (idx != FI->armed_k)
        return -1;
    FI->fired = 1;
    FI->fired_addr = addr;
    sb_printf("[fi] reset at flash op %u (0x%08x) mode %u delay %u\r\n", idx, addr, FI->mode,
              FI->delay_cyc);
    if (FI->mode == SB_FI_BEFORE)
        NVIC_SystemReset();
    return (int)FI->mode;
}
#endif

/* ART caches may hold stale flash contents after erase/program (RM0090
 * 3.5.2): reset them. */
static void flush_caches(void)
{
    uint32_t acr = FLASH->ACR;
    FLASH->ACR = acr & ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR = (acr & ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN)) | FLASH_ACR_ICRST | FLASH_ACR_DCRST;
    FLASH->ACR = acr & ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
}

static int finish(uint32_t sr)
{
    FLASH->SR = FLASH_ERRORS | FLASH_SR_EOP;   /* write-1-to-clear */
    return (sr & FLASH_ERRORS) ? -1 : 0;
}

int sb_flash_erase_sector(unsigned sector)
{
    if (sector >= SB_NUM_SECTORS)
        return -1;
    unlock();
    FLASH->SR = FLASH_ERRORS | FLASH_SR_EOP;
    uint32_t cr = FLASH_CR_SER | (sector << FLASH_CR_SNB_Pos) | FLASH_CR_PSIZE_1;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
#ifdef SB_FAULT_INJECT
    int fi = fi_check(sb_sector_addr(sector));
    if (fi == SB_FI_MID)
        ram_run_and_reset(cr, 0, 0, FI->delay_cyc);
#endif
    uint32_t sr = ram_run(cr, 0, 0);
#ifdef SB_FAULT_INJECT
    if (fi == SB_FI_AFTER)
        NVIC_SystemReset();
#endif
    flush_caches();
    __set_PRIMASK(primask);
    lock();
    return finish(sr);
}

int sb_flash_program(uint32_t addr, const void *src, uint32_t len)
{
    if ((addr & 3u) || (len & 3u) || addr < SB_FLASH_BASE ||
        addr + len > SB_FLASH_BASE + SB_FLASH_SIZE)
        return -1;
    const uint8_t *s = src;
    int rc = 0;
    unlock();
    FLASH->SR = FLASH_ERRORS | FLASH_SR_EOP;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    for (uint32_t i = 0; i < len && rc == 0; i += 4) {
        uint32_t w = (uint32_t)s[i] | (uint32_t)s[i + 1] << 8 | (uint32_t)s[i + 2] << 16 |
                     (uint32_t)s[i + 3] << 24;
#ifdef SB_FAULT_INJECT
        int fi = fi_check(addr + i);
        if (fi == SB_FI_MID)
            ram_run_and_reset(FLASH_CR_PG | FLASH_CR_PSIZE_1, (volatile uint32_t *)(addr + i), w,
                              FI->delay_cyc);
#endif
        uint32_t sr = ram_run(FLASH_CR_PG | FLASH_CR_PSIZE_1, (volatile uint32_t *)(addr + i), w);
#ifdef SB_FAULT_INJECT
        if (fi == SB_FI_AFTER)
            NVIC_SystemReset();
#endif
        rc = finish(sr);
    }
    flush_caches();
    __set_PRIMASK(primask);
    lock();
    return rc;
}

const uint8_t *sb_flash_ptr(uint32_t addr)
{
    return (const uint8_t *)addr;
}
