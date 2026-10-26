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
    uint32_t sr = ram_run(cr, 0, 0);
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
        uint32_t sr = ram_run(FLASH_CR_PG | FLASH_CR_PSIZE_1, (volatile uint32_t *)(addr + i), w);
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
