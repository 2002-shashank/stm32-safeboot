# Flash and RAM layout (STM32F407VG, 1 MB)

The source of truth is `core/include/sb/layout.h`. The linker scripts
(`target/bootloader/bootloader.ld`, `target/app/app.ld`) and the Python tools
(`tools/sbimage.py` parses the header) mirror it. `host/test_core.c` checks
that every region starts and ends on a sector boundary.

## Flash properties the design depends on

| Property | F407 behavior | Measured on this board (MB997E, 2.90 V, 168 MHz) | Consequence |
|---|---|---|---|
| Sector map | S0–S3 16 KB, S4 64 KB, S5–S11 128 KB | — | The erase unit is a whole sector. Regions never share one. |
| Banks | Single bank | — | Any flash fetch stalls during an erase or program. The wait loops run from RAM (`.ramfunc`, 52 B). |
| Write granularity | x32 programming at 2.7–3.6 V, word-aligned | 14.3 µs per word, 272 KB/s | Records and images are written in 32-bit words. |
| Program semantics | Bits can only be cleared. No ECC on the F4. | — | Never program into non-erased space. Torn records are skipped, not repaired. |
| Erase time | Datasheet typical: 250 / 550 / 1000 ms | 16 KB: 195 ms, 64 KB: 548 ms, 128 KB: 1.02 s | The journal uses 16 KB sectors, so a compaction costs about 196 ms. A full slot erase is about 3 s. |
| Hardware hash | None (HASH exists only on F415/F417) | SHA-256 in software: 66.5 cycles/B, 2.5 MB/s | A 384 KB slot hashes in 156 ms. |
| Reset during erase/program | The operation aborts and the contents are undefined | Exercised with `bootloader_fi` mid-op resets | This is the torn-write model of the journal. |

## Regions

| Sectors | Address | Size | Region | Written by |
|---|---|---|---|---|
| S0–S1 | 0x0800_0000 | 32 KB | Bootloader, with the API table in its last 256 B (0x0800_7F00) | Programmer only |
| S2 | 0x0800_8000 | 16 KB | Journal page 0 | Bootloader code only |
| S3 | 0x0800_C000 | 16 KB | Journal page 1 | Bootloader code only |
| S4 | 0x0801_0000 | 64 KB | Provisioning: public key, key ID, device ID (`sb_prov_t`) | Programmer only |
| S5–S7 | 0x0802_0000 | 384 KB | Slot A | Bootloader |
| S8–S10 | 0x0808_0000 | 384 KB | Slot B | Bootloader |
| S11 | 0x080E_0000 | 128 KB | Recovery (golden) image, flagged `SB_IMG_F_RECOVERY` | Programmer only |

- **Ownership.** "Bootloader code only" includes `boot_confirm()`. The app
  calls it through the API table, but the code that writes the journal lives
  in the bootloader. The app never links a flash driver.
- **Bootloader budget.** 19.1 KB is used out of 32 KB (31.7 KB usable after
  the API table). Ed25519 verify with SHA-512 is about 10.5 KB of that. Its
  fault handlers record a crash (slot 0xFF) and reset rather than hang.
- **Image layout.** Each slot holds a 512 B header followed by the payload,
  which starts with the vector table at `slot + 0x200`. The F407 has 98
  vectors (392 B), so VTOR needs 512 B alignment.
- **Max image size.** 384 KB − 512 B = **392,704 B** per slot, and
  130,560 B for the recovery image.
- **Execute in place.** Each image is linked for one slot (`--defsym
  SB_APP_ORIGIN`). The header's signed `load_addr` must match the region it
  sits in. `sbtool.py sign` refuses an image whose reset vector points
  outside the slot it is signed for.
- **Hardening option, not enabled yet.** Write protection (WRP) on S0, S1
  and S4. It stops only software and accidental writes; see the threat
  model.

## RAM

| Address | Size | Use |
|---|---|---|
| 0x2000_0000 | 124 KB | Bootloader data/stack, then the app's (the stack top is 0x2001_F000) |
| 0x2001_F000 | 3 KB | RTT control block and log ring at a fixed address, shared by bootloader and app |
| 0x2001_FC00 | 1 KB | Bootloader RAM functions. `boot_confirm()` copies them here again on each call. |
| 0x1000_0000 | 64 KB CCM | Unused. CCM is data-only and cannot execute RAM functions. |
| 0x4002_4000 | 4 KB backup SRAM | Boot info at +0x000, crash record after it, fault-injection block at +0xC00 |

## What survives a reset (measured, `metrics/board_retention.json`)

| Reset | RCC_CSR flags | Backup SRAM | `.noinit` SRAM |
|---|---|---|---|
| `NVIC_SystemReset` | PIN, SFT | kept | kept |
| IWDG timeout | PIN, IWDG | kept | kept |
| Debugger reset (`reset run`) | PIN, SFT | kept | kept |
| Power cycle (USB unplugged 15 s, cold boot with no debugger) | BOR, PIN, POR | **lost** | lost |

The F4 sets PINRSTF on *every* reset, because an internal reset also drives
the NRST pin. Only a reset with PIN alone means an external reset.

Consequence: the crash record and boot info survive every reset (including
the watchdog reset that ends a hung trial), but **not a power loss**. On this
board VBAT has no separate supply, so the backup domain is unpowered when USB
is removed. A crash followed by a power cut loses the record. The trial
attempt counter is unaffected, because it lives in the flash journal.
