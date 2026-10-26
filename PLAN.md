# stm32-safeboot — Plan

A secure, power-fail-safe A/B bootloader for the STM32F407: signed images (SHA-256 + Ed25519),
anti-rollback, a resumable UART update path, trial boots with automatic rollback, and a persistent
crash record. Every phase records measurements as it is built (see §9).

**Current step: Phase 0 (feasibility check) only.** Nothing past Phase 0 starts until its go/no-go
items are resolved.

---

## 1. Flash layout (design, not an accident of the linker script)

### 1.1 F407 flash facts the design depends on

| Property | Value (RM0090 / DS8626) | Consequence |
|---|---|---|
| Sector map (1 MB part, e.g. F407VG) | S0–S3: 16 KB, S4: 64 KB, S5–S11: 128 KB | Erase unit is the sector; nothing smaller can be erased |
| Banks | Single bank on F407 (dual bank only on F42x/F43x) | CPU **stalls** on any flash fetch while an erase/program runs |
| Program size | x8/x16/x32/x64 by PSIZE; x32 at 2.7–3.6 V | Write granularity = 32-bit word, word-aligned |
| Program semantics | Can only clear bits (1→0); no flash ECC on F4 | "Erased" = 0xFF; every record must be written into erased space only |
| Erase time (x32, typ / max) | 16 KB ≈ 250 / 500 ms, 64 KB ≈ 550 / 1100 ms, 128 KB ≈ 1 / 2 s | Erasing a slot takes seconds; UART must survive the stall |
| Word program time | ≈ 16 µs typ (100 µs max) | ≈ 250 KB/s raw program rate, before erase |
| Hardware hash | None (HASH/CRYP exist only on F415/F417) | SHA-256 and Ed25519 are software |

All timings above are datasheet values to be **measured** on the real board in Phase 2.

### 1.2 Regions (layout A — primary, 1 MB part)

| Sectors | Address range | Size | Region | Owner (only writer) |
|---|---|---|---|---|
| S0–S1 | 0x0800_0000–0x0800_7FFF | 32 KB | Bootloader (WRP option bytes set) | Programmer only |
| S2 | 0x0800_8000–0x0800_BFFF | 16 KB | Journal page 0 | Bootloader |
| S3 | 0x0800_C000–0x0800_FFFF | 16 KB | Journal page 1 | Bootloader |
| S4 | 0x0801_0000–0x0801_FFFF | 64 KB | Provisioning/state: public key(s), key ID, device ID; written once, WRP | Programmer only |
| S5–S7 | 0x0802_0000–0x0807_FFFF | 384 KB | Slot A | Bootloader |
| S8–S10 | 0x0808_0000–0x080D_FFFF | 384 KB | Slot B | Bootloader |
| S11 | 0x080E_0000–0x080F_FFFF | 128 KB | Recovery: optional signed golden image (≤ 128 KB) | Programmer only |

Rules:
- **Sector ownership is exclusive.** No two regions share a sector, so erasing one region can
  never damage another. The application never writes flash itself; it reaches the journal
  only through the bootloader's API table (§3.4).
- **Journal uses two separate sectors** because atomicity needs one copy to stay intact while
  the other is erased. The small 16 KB sectors keep the erase short (~250 ms).
- **Alignment.** Each slot starts with a 512-byte header. The app's vector table sits at
  `slot_base + 0x200`. The F407 has 98 vectors (392 B), and VTOR needs alignment to the next
  power of two (512 B).
- **Max image size** = 384 KB − 512 B header = **392,704 bytes** per slot (golden image:
  128 KB − 512 B = 130,560 bytes).
- **Execute in place means per-slot link addresses.** An image linked for slot A cannot run from
  slot B. The build produces one binary per slot, and the header carries a signed `load_addr`
  that the bootloader checks against the slot it sits in. The update host sends the build for
  the inactive slot.
- The recovery path when both slots are INVALID is the golden image if one is present, or else
  the bootloader's own UART recovery mode.

**Fallback layout** (if Phase 0 shows the bootloader cannot fit in 32 KB): bootloader S0–S3 (64 KB),
journal S4 + S11, slots unchanged at S5–S7 / S8–S10, no golden image.
**512 KB part (F407VE):** only S0–S7 exist. Use slots S5 and S6 (128 KB each) and recovery in S7.
Phase 0 confirms which part is on the board.

---

## 2. Power-fail-atomic journal

### 2.1 Record format (64 bytes, 64-byte aligned, append-only within a page)

| Off | Field | Notes |
|---|---|---|
| 0 | `magic` u32 | Record type/format version |
| 4 | `seq` u32 | Monotonic; highest valid seq wins |
| 8 | `state[A]`, `state[B]` u8 | EMPTY / VALID / PENDING / TRIAL / CONFIRMED / INVALID / ROLLBACK |
| 10 | `active` u8, `candidate` u8 | Slot indices (0xFF = none) |
| 12 | `version[A]`, `version[B]` u32 | From the verified image headers |
| 20 | `min_version` u32 | Anti-rollback floor; never decreases |
| 24 | `attempts` u8, `max_attempts` u8, `flags` u16 | Trial-boot counter |
| 28 | `rx_session` u32, `rx_offset` u32 | Checkpoint for resumable transfer |
| 36–55 | reserved (0xFF) | |
| 56 | `crc32` | Over bytes 0–55 |
| 60 | `commit` u32 = `0x5AFEC0DE` | **Programmed last, as a separate word write** |

### 2.2 Rules that make it atomic
1. **Write order:** program words 0–59, read them back and check, then program `commit`.
   A record is valid only if `commit == 0x5AFEC0DE`, `magic` matches, and the CRC matches.
2. **Never write into non-erased space.** The writer appends only into a 64 B record slot that
   reads as all 0xFF. A torn record is skipped forever, never repaired in place.
3. **Scan:** read both pages and pick the valid record with the highest `seq`. A torn write,
   whether to the body or the commit word, fails validation, so the previous record stays
   current. The state is always the before-state or the after-state, never a mix.
4. **Compaction (page full):** the current record lives in the full page. Erase the *other* page,
   then write the current state with `seq + 1` into it. If power fails during the erase, the full
   page is untouched. If it fails after the new record is written, both pages hold valid records
   and the higher seq wins.
5. **Log intent before acting on a slot:** mark a slot EMPTY in the journal *before* erasing it,
   and VALID only *after* its signature verifies. The bootloader never trusts a slot's contents
   over the journal.
6. Partially erased sectors may look like random data. The CRC and commit marker reject them.
   CRC32 misses a corrupted record with probability about 2⁻³².

### 2.3 Wear
Confirmed boots write nothing. Each update writes a bounded number of records: EMPTY, about 24
checkpoints (one per 16 KB received), VALID, PENDING, up to 3 TRIAL records, then CONFIRMED or
ROLLBACK. That is about 32 records, or 2 KB, so a 16 KB page lasts about 8 updates between
erases. Flash is rated for 10k cycles, which allows roughly 80k updates.

### 2.4 Fault simulation (host-native, exhaustive)
- `flash_sim`: a RAM model of the F407 sector map. Erase sets bytes to 0xFF, and program ANDs
  new bits into old ones. Programming a word that is not erased is an assertion failure.
- **Injector:** each word program and each sector erase is one flash op. For each scenario, run
  once to count N ops, then for every k in 0..N "cut power" at op k:
  - Program: the word becomes the old value, the new value, or a random torn mix (old & partial new).
  - Erase: the sector becomes a random mix of erased and original bits.
  - Then "reboot": run the real bootloader decision code from scratch, then let the interrupted
    operation resume or retry.
- **Invariants checked after every injection:** the scanned state equals the before-state or the
  after-state; the bootloader never jumps to a slot whose signature does not verify;
  `min_version` never decreases; there is always a bootable choice or a clean entry to recovery;
  the system converges once the operation is retried.
- **Scenarios:** fresh provision, full update, resume after a transfer drop, confirm, each failing
  trial attempt, rollback, compaction, and a crash record written during a trial.
- Each scenario runs every k with S random-torn seeds. Pass counts are reported as `passed/total`.
- **On hardware:** the k-th flash op triggers an `NVIC_SystemReset` (k is set via a backup
  register), plus a smaller number of real power cuts from a MOSFET or USB relay at random times.
  A software reset does not tear a flash word the way a power cut does, so only the real power
  cuts test torn writes on silicon. Both are reported separately.

---

## 3. Boot state machine, trial boots, and rollback

### 3.1 Per-slot states
| State | Meaning |
|---|---|
| EMPTY | Erased or being received; never bootable |
| VALID | Fully received, signature and version verified, not scheduled |
| PENDING | Scheduled to be tried on the next boot (`candidate`) |
| TRIAL | Booted at least once and waiting for `boot_confirm()`; `attempts` counts boots |
| CONFIRMED | App confirmed health; this is `active` |
| INVALID | Failed verification or ran out of attempts |
| ROLLBACK | Bootloader is reverting from this slot; persisted so a power cut mid-rollback finishes the rollback |

### 3.2 Boot decision
1. Scan the journal and read the crash record and reset reason (§6).
2. If `candidate` is PENDING or TRIAL:
   - If `attempts < max_attempts` (default 3, configurable 2–3): append TRIAL with `attempts + 1`
     **before** jumping, so a crash or hang still counts. Start the IWDG, verify the image, and jump.
   - Otherwise: append ROLLBACK (candidate → INVALID, active stays the previous CONFIRMED slot),
     then boot the previous slot.
3. Otherwise boot `active` (CONFIRMED) after a full SHA-256 + Ed25519 check.
4. If verification fails: mark the slot INVALID, try the other slot, then the golden image, then
   UART recovery mode.

### 3.3 Watchdog
Trial boots start the IWDG before handoff. Once started, the IWDG cannot be stopped, so the app
must kick it. A hang therefore becomes a reset, and the reset counts as a failed attempt.

### 3.4 `boot_confirm()`
The bootloader exports a versioned API table at a fixed address (the end of S1) with
`boot_confirm()`, `boot_get_info()`, and `boot_get_crash()`. `boot_confirm()` appends CONFIRMED:
`active = candidate`, the old slot drops to VALID as the fallback, and `min_version` is raised to
the confirmed version. The app calls it only after its own initialization and self-test. Only
bootloader code ever writes the journal.

---

## 4. Image format and verification
- **512-byte header:** magic, header version, `image_version`, `image_size`, `load_addr`,
  `slot_id`, `sha256(payload)`, `key_id`, flags, and an Ed25519 signature over all header bytes
  before the signature (which include the digest).
- **Verification:** check bounds (`image_size ≤ 392,704`, `load_addr` matches the slot), compute
  SHA-256 over `image_size` bytes, compare it in constant time, then run Ed25519 verify with the
  key from S4. Ed25519 internally uses SHA-512 over the small header, so both hashes are needed.
- **Version policy:** an update is accepted only if `image_version > min_version`.
- **Signing tool:** Python (`cryptography` or `pynacl`). The private key stays on the host and is
  never in the repo.

---

## 5. Resumable UART transfer

**Frame:** `SOF | type | seq | len | payload | CRC32`. Responses echo `seq`.

| Command | Payload | Response |
|---|---|---|
| BEGIN_UPDATE | session_id | ACK(next_offset). Resumes if the session matches the journal checkpoint, else starts new (journal → EMPTY for the inactive slot) |
| IMAGE_INFO | 512 B header | ACK after bounds, version, and slot checks (the signature is checked at VERIFY) |
| DATA | offset, length (≤ 1 KB), CRC32, bytes | ACK(next_offset) |
| QUERY_PROGRESS | — | next_offset, session_id, state |
| FINALIZE | total length | ACK when all bytes are written and the final checkpoint is persisted |
| VERIFY | — | SHA-256 + Ed25519 result. On pass, journal → VALID (→ PENDING when activation is requested) |

- **Idempotent and duplicate-safe:**
  - `offset < next_offset` (a duplicate or a lost ACK): re-ACK with next_offset and write nothing.
  - `offset > next_offset`: NACK(next_offset).
  - A bad CRC gets a NACK. The host retries with a timeout and backoff, up to N times.
- **Erase stalls:** sectors are erased lazily when the offset first crosses into them. While an
  erase runs, the device withholds the ACK and the host waits (timeout > 2.5 s). UART RX runs on
  circular DMA into SRAM, and the flash routines run from RAM, so no bytes are lost while the
  single-bank flash is busy.
- **Resume across reboot:** the journal stores `rx_offset` every 16 KB. After a power cut the
  device resumes from the last checkpoint and re-erases the partial sector if needed.
- **Host tool** (Python + pyserial) handles retries, resume, a progress bar, and throughput logging.

---

## 6. Persistent crash record
- **Location:** backup SRAM (4 KB at 0x4002_4000, PWR clock + DBP, backup regulator enabled).
  Optionally a `.noinit` SRAM section, which the bootloader's RAM map never touches. Never flash.
- **Contents:** magic, CRC, CFSR, HFSR, MMFAR, BFAR, stacked R0–R3/R12/LR/PC/xPSR, EXC_RETURN,
  image version, slot, reset reason, boot count.
- **Reset reason:** the bootloader reads `RCC_CSR` (POR/PIN/SFT/IWDG/WWDG/LPWR/BOR) first,
  stores it, clears it with RMVF, and passes it to the app through the API table. Otherwise the
  app could never see the reason, because the flags are cleared.
- **Write path:** the app's HardFault/MemManage/BusFault/UsageFault handlers fill the record and
  reset. On the next boot the bootloader validates it, prints it over UART, and marks it consumed.
  A crash during TRIAL also counts as a failed attempt.
- **Retention:** survives system, IWDG, and pin resets. It survives power-off only with VBAT. On
  the STM32F4-Discovery, VBAT is tied to VDD (to confirm against the schematic). The retention
  matrix is measured, not assumed.

---

## 7. Boot-path timing (DWT)
- Enable `DEMCR.TRCENA` and `DWT_CYCCNT` as the first instructions of `Reset_Handler`, before
  `.data`/`.bss` init. Never reset CYCCNT in the bootloader. The app reads it as its first action
  in `main`.
- **Checkpoints:** reset→bootloader `main`, clock-up, journal scan, SHA-256, Ed25519 verify,
  handoff (jump → app's first instruction), and total validated boot.
- **Clock domains:** the core starts on HSI at 16 MHz and moves to PLL at 168 MHz. Each checkpoint
  is logged as (cycles, f_clk) and converted per segment.
- **Cross-check:** the time from NRST release to the first instruction cannot be seen by DWT, so it
  is measured with a GPIO toggle and a logic analyzer.
- Timing runs are repeated (n ≥ 20) and reported as min/median/max.

---

## 8. Threat model (honest scope) → `docs/threat-model.md`
**Defended (through the update path):**
- Forged or modified images: Ed25519 signatures provide authenticity, and the SHA-256 digest
  provides integrity. Only images signed by the holder of the private key are accepted.
- Downgrades through the update path: `min_version` rejects older signed images.
- Accidental corruption and power loss: the journal design (§2), verification on every boot,
  trial boots, and automatic rollback.
- A buggy but signed image: the trial-boot counter and IWDG roll back automatically.

**Not defended. These are explicit non-goals, and the F407 has no TrustZone, secure element, or
OTP-backed secure boot:**
- An attacker with SWD/JTAG or physical flash access can read the firmware, rewrite the bootloader
  or public key, reset `min_version` by erasing the journal, or jump past verification.
- RDP Level 1 still allows a debugger to attach and mass-erase the chip. RDP Level 2 is permanent
  and blocks debug, but published fault-injection attacks on the STM32F2/F4 family (e.g. the
  wallet.fail RDP downgrade on the STM32F205) show it is not a hard boundary. WRP on the
  bootloader and S4 sectors stops only accidental or software writes.
- Fault injection (voltage or clock glitching) aimed at skipping the signature check. This gets
  cheap hardening only (redundant checks, constant-time compare) and no resistance claims.
- Confidentiality: images are not encrypted, so anyone sniffing UART sees the firmware.
- A compromised signing key: `key_id` exists for future revocation, but the key in S4 can only be
  replaced by reprogramming.
- Side channels on Ed25519 verify do not matter (public data). Signing happens off-device.

---

## 9. Metrics (collected as each part is built) → `metrics/`
Each phase appends to `metrics/metrics.md` (human summary) and `metrics/*.csv` (raw). Every entry
records the git hash, board, clock, and compiler flags.

| Metric | Phase | How |
|---|---|---|
| Bootloader/app `.text/.rodata/.data/.bss`, per-module sizes | 0, every phase | `arm-none-eabi-size`, map file |
| Reset→main, journal scan, SHA-256, Ed25519, handoff, total validated boot | 1, 2, 3 | DWT (§7), logic analyzer |
| SHA-256 throughput (MB/s, cycles/byte) and Ed25519 verify time | 0 spike, 3 | DWT |
| Flash erase time per sector size, program throughput | 2 | DWT around HAL calls |
| UART goodput vs baud (115200 / 460800 / 921600) and retry rate under injected errors | 5 | Host tool |
| Rollback time (failed trial → previous image running) and recovery time (both invalid → recovery mode ready) | 4 | DWT + host timestamps |
| Fault-test pass counts per scenario (sim exhaustive, HW software-reset, HW power-cut) | 2, 4, 5, 6 | `passed/total` |
| Crash-record retention matrix by reset type | 6 | Scripted |

---

## 10. Phases

| # | Phase | Exit criteria |
|---|---|---|
| **0** | **Feasibility check** (below) | All go/no-go items resolved and layout A or fallback chosen |
| 1 | Skeleton: bootloader + app linker scripts per slot, handoff (VTOR, MSP, peripherals reset), UART log, DWT checkpoints | Bootloader jumps to app in A and B; sizes and timings logged |
| 2 | Flash driver (RAM-resident), journal, `flash_sim` + exhaustive fault harness | 100% pass on journal scenarios in sim; HW erase/program timings measured |
| 3 | Image header, signing tool, SHA-256 + Ed25519 verify on every boot | Tampered/wrong-slot/old-version images rejected; crypto timings logged |
| 4 | State machine: PENDING/TRIAL/attempts/IWDG/`boot_confirm()`/ROLLBACK | Auto-rollback after N bad boots; fault pass counts for confirm and rollback |
| 5 | Resumable UART protocol + host tool | Resume after cable pull and after power cut; duplicates and bad CRCs handled; throughput logged |
| 6 | Crash record + reset reason | Bootloader prints a decoded fault after a deliberate crash; retention matrix logged |
| 7 | Threat model doc, HW power-cut campaign, final metrics report | Report complete with honest limits |

### Phase 0 — Feasibility check (current)

| ID | Check | Go criterion | Status (2026-10-26) |
|---|---|---|---|
| F0.1 | Identify the board/part: `DBGMCU_IDCODE`, flash size register at 0x1FFF_7A22 | 1 MB → layout A; 512 KB → VE layout | ✅ STM32F407G-DISC1 MB997E: DEV_ID 0x413 (IDCODE 0x101F6413), flash size reg 0x400 = 1024 KB → **layout A**. Option bytes OPTCR 0x0FFFAAED: RDP 0, no WRP, software IWDG. VDD 2.90 V (x32 programming OK) |
| F0.2 | Cross toolchain: Arm GNU Toolchain (`arm-none-eabi-gcc`), OpenOCD or stlink | Builds and flashes a blinky | ✅ Arm GNU Toolchain 15.3.Rel2 (`~/opt/arm-gnu-toolchain-15.3.rel2`, extracted from Arm's notarized pkg, no sudo), OpenOCD 0.12.0, stlink 1.9.0. OpenOCD sees ST-LINK V2J46M31 (0483:374B). Register-level blink flashed, verified, running (LEDs cycle; read back over SWD). Factory flash backed up to `backups/factory_flash_1MB.bin` |
| F0.3 | Crypto spike: build Monocypher (Ed25519 verify + SHA-512) and a SHA-256 for Cortex-M4 `-Os`; measure size and DWT time on the board | Bootloader estimate ≤ 32 KB → layout A, else fallback; verify < ~100 ms | ⏳ Size measured on host: Monocypher Ed25519 verify incl. SHA-512 ≈ 10.7 KB `.text` at `-Os`. Board timing still to do |
| F0.4 | Erase stall check: UART DMA RX keeps receiving during a 128 KB erase with code in RAM | Zero lost bytes at 921600 baud | ⏳ Board available; not yet run |
| F0.5 | DWT CYCCNT runs without a debugger attached (TRCENA set by firmware) | Counts at 168 MHz cold-booted, no probe | ◐ CYCCNT enabled by firmware counts correctly (4,079,778 cycles per 0.255 s loop at 16 MHz). The debugger had been attached since power-up, so a cold boot with no probe is still to check |
| F0.6 | Backup SRAM retention: SFT/IWDG/PIN reset, power cycle (VBAT wiring) | Survives resets; power-cycle behavior documented | ⏳ Board available; not yet run |
| F0.7 | Host-native simulation path: `flash_sim` + bootloader core build with host clang | Core logic compiles for host and target from one source | ✅ Host build + unit tests pass (ASan/UBSan). Quick fault harness: 22,324/22,324 runs pass. **But** mutation testing caught only 3 of 7 deliberate bugs, so the harness needs strengthening before its pass counts mean much |
| F0.8 | QEMU `netduinoplus2` (STM32F405) as an extra target | Only if it models FLASH, DWT, and BKPSRAM. Likely not, so host sim stays the primary off-target path | ⚠️ QEMU 11.0.2 present; models unverified |
| F0.9 | Host Python tools: pyserial, cryptography/pynacl in a venv | Imports work | ✅ `.venv` with pyserial 3.5, cryptography 50.0.2 |
| F0.10 | Power-cut rig: MOSFET or USB relay driven by the host | Available for Phase 7 (optional; sim + software reset are used otherwise) | ✖ No rig. Real power cuts are reported as **not tested** |
| F0.11 | ST-LINK VCP wired to a target USART? | A host serial link to the update USART | ⚠️ The VCP enumerates (`/dev/cu.usbmodem*`), but **no data arrives** from USART2/PA2, USART3/PD8 or USART6/PC6, all of which were confirmed transmitting (SR=0xC0). This matches community reports of UM1472: on the DISC1 the VCP pins are not connected to the F407. Needs a fly-wire retrofit (UM1472 §6.1.3) or a 3.3 V USB-UART adapter on PA2/PA3 |
