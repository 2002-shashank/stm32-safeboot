/*
 * Flash/RAM layout for STM32F407VG (1 MB). Single source of truth: the linker
 * scripts, the host simulator and the Python tools all mirror these values.
 * See docs/flash-layout.md for the rationale.
 */
#ifndef SB_LAYOUT_H
#define SB_LAYOUT_H

#include <stdint.h>

#define SB_FLASH_BASE        0x08000000u
#define SB_FLASH_SIZE        0x00100000u
#define SB_NUM_SECTORS       12u
#define SB_FLASH_ERASED_WORD 0xFFFFFFFFu

/* Region map (sector indices are inclusive). */
#define SB_BOOT_ADDR         0x08000000u   /* S0-S1, 32 KB */
#define SB_BOOT_SIZE         0x00008000u
#define SB_JOURNAL_SECTOR0   2u            /* S2, 16 KB */
#define SB_JOURNAL_SECTOR1   3u            /* S3, 16 KB */
#define SB_JOURNAL_PAGE_SIZE 0x00004000u
#define SB_PROV_ADDR         0x08010000u   /* S4, 64 KB provisioning */
#define SB_PROV_SECTOR       4u
#define SB_SLOT_A_ADDR       0x08020000u   /* S5-S7, 384 KB */
#define SB_SLOT_B_ADDR       0x08080000u   /* S8-S10, 384 KB */
#define SB_SLOT_SIZE         0x00060000u
#define SB_RECOVERY_ADDR     0x080E0000u   /* S11, 128 KB golden image */
#define SB_RECOVERY_SIZE     0x00020000u

/* Image header precedes the vector table; 512 B satisfies VTOR alignment
 * for the F407's 98-entry vector table. */
#define SB_HDR_SIZE          512u
#define SB_SLOT_MAX_PAYLOAD  (SB_SLOT_SIZE - SB_HDR_SIZE)          /* 392704 */
#define SB_RECOVERY_MAX_PAYLOAD (SB_RECOVERY_SIZE - SB_HDR_SIZE)   /* 130560 */

/* Bootloader API table lives in the last 256 B of the bootloader region. */
#define SB_API_ADDR          (SB_BOOT_ADDR + SB_BOOT_SIZE - 0x100u)

/* RAM. The top 4 KB of SRAM is shared by bootloader and app and never used
 * for ordinary data by either:
 *   0x2001F000  3 KB  RTT control block + log buffer (fixed address so the
 *                     debugger finds the live block, not a stale copy)
 *   0x2001FC00  1 KB  bootloader RAM-resident flash routines, so
 *                     boot_confirm() can program flash from app context */
#define SB_SRAM_BASE         0x20000000u
#define SB_SRAM_SIZE         0x00020000u
#define SB_RESERVED_TOP_SIZE 0x00001000u
#define SB_RTT_ADDR          (SB_SRAM_BASE + SB_SRAM_SIZE - SB_RESERVED_TOP_SIZE)
#define SB_RTT_SIZE          0x00000C00u
#define SB_RAMFUNC_ADDR      (SB_RTT_ADDR + SB_RTT_SIZE)
#define SB_RAMFUNC_SIZE      0x00000400u
#define SB_APP_RAM_SIZE      (SB_SRAM_SIZE - SB_RESERVED_TOP_SIZE)
#define SB_BKPSRAM_ADDR      0x40024000u   /* 4 KB backup SRAM */

#define SB_SLOT_NONE         0xFFu
#define SB_NUM_SLOTS         2u

static inline uint32_t sb_slot_addr(unsigned slot)
{
    return slot == 0 ? SB_SLOT_A_ADDR : SB_SLOT_B_ADDR;
}

/* F407 sector geometry: 4 x 16 KB, 1 x 64 KB, 7 x 128 KB. */
static inline uint32_t sb_sector_size(unsigned sector)
{
    return sector < 4 ? 0x4000u : sector == 4 ? 0x10000u : 0x20000u;
}

static inline uint32_t sb_sector_addr(unsigned sector)
{
    if (sector < 4)
        return SB_FLASH_BASE + sector * 0x4000u;
    if (sector == 4)
        return SB_FLASH_BASE + 0x10000u;
    return SB_FLASH_BASE + 0x20000u + (sector - 5u) * 0x20000u;
}

/* Returns the sector containing addr, or -1 if outside flash. */
static inline int sb_sector_of(uint32_t addr)
{
    if (addr < SB_FLASH_BASE || addr >= SB_FLASH_BASE + SB_FLASH_SIZE)
        return -1;
    uint32_t off = addr - SB_FLASH_BASE;
    if (off < 0x10000u)
        return (int)(off / 0x4000u);
    if (off < 0x20000u)
        return 4;
    return (int)(5u + (off - 0x20000u) / 0x20000u);
}

#endif
