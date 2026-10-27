# Boot journal: power-fail-atomic state

Code: `core/src/journal.c`, `core/src/boot.c`. Host mirror: `tools/sbjournal.py`.

## Record (64 B, `sb_record_t`)

| Off | Field | Notes |
|---|---|---|
| 0 | magic | `0x4A425321` |
| 4 | seq | +1 per append. The valid record with the highest seq is the current state. |
| 8 | state[A], state[B] | EMPTY 1, VALID 2, PENDING 3, TRIAL 4, CONFIRMED 5, INVALID 6, ROLLBACK 7 |
| 10 | active, candidate | Slot index, or 0xFF for none |
| 12 | version[A], version[B] | Taken from verified headers |
| 20 | min_version | Anti-rollback floor. It never decreases and is raised only on confirm. |
| 24 | attempts, max_attempts | Trial boots counted so far (default limit 3) |
| 26 | rx_slot, flags | `SB_RF_RX` marks a transfer in progress |
| 28 | rx_session, rx_offset | Resume checkpoint: payload bytes written and read back |
| 36 | reserved (0xFF) | |
| 56 | crc32 | Over bytes 0–55 |
| 60 | commit | `0x5AFEC0DE`, programmed last as its own word write |

## Rules

1. **Write order.** Program the 15 body words, read them back, then program
   the commit word and read it back.
2. **Validity.** A record counts only if magic, CRC and commit are all
   right. Each check is unit-tested on its own (`test_journal_layers`). The
   CRC is the only one that catches a partially erased old record whose
   seq bits flipped from 0 to 1.
3. **Append only into erased space.** A torn or garbage slot is skipped
   forever.
4. **Compaction.** When a page is full, erase the *other* page and write
   the current state there with seq + 1. The current record is never in the
   sector being erased.
5. **Intent before action.** A slot is marked EMPTY before it is erased. It
   is marked VALID only after the whole image verifies. A trial attempt is
   counted before the jump, so a crash or hang still counts.

## State machine

```
            BEGIN_UPDATE          VERIFY ok             VERIFY ok + activate
 any  ───────────────► EMPTY ───────────────► VALID ─────────────────────► PENDING
                         │ VERIFY fails                                       │ boot
                         ▼                                                    ▼
                      INVALID ◄──── fails verify at boot ──────────────── TRIAL (attempts++)
                                                                         │      │
                                       app calls boot_confirm() ─────────┘      │ attempts = max
                                                ▼                               ▼
                                            CONFIRMED                      ROLLBACK
                                  (old CONFIRMED slot → VALID,       (previous CONFIRMED slot
                                   min_version = this version)        boots, min_version kept)
```

- A slot that is VALID but below `min_version` stays in flash, but it is
  never booted.
- If the active image fails verification, a VALID image in the other slot
  that is ≥ `min_version` is *promoted* to a trial. If none exists, the
  golden image boots. If that's missing too, the device stays in bootloader
  recovery mode.
- An empty journal (factory state) adopts the newest verifying image. An
  empty journal can't be produced by a power cut: compaction never erases
  the page that holds the current record.

## Wear

Confirmed boots write nothing. An update writes about 7 records plus one
checkpoint per 16 KB, so about 30 records for a full slot. A 16 KB page
holds 256 records, so it lasts about 8 full updates before a compaction.
Flash is rated for 10k erase cycles, which allows roughly 80k updates.

## Measured costs (`metrics/board_bench.json`)

| Operation | Time |
|---|---|
| Scan, empty journal (2 × 16 KB) | 1.28 ms |
| Scan, full page | 2.27 ms |
| Append (16 word programs plus read-back) | 198 µs |
| Append that compacts (includes a 16 KB erase) | 196 ms |

## How power-fail safety is tested

Harness note: while the device erases flash, the host must not read flash
over SWD. Such a read stalls until the erase ends. In testing, the device
then sometimes failed to take its own reset request. The on-silicon
harness only polls backup SRAM while the device runs.

| Level | What | Result |
|---|---|---|
| Unit | Each validity layer alone, torn records, compaction (`test_core`) | Pass under ASan/UBSan |
| Simulator (**124,930/124,930 pass**) | `fault_harness`: power cut at every flash op × 5 variants (not done / done / torn-random / torn-prefix / sparse partial erase). Checks journal = before-or-after, boot safety, min_version monotonic, convergence, liveness, no overwrite. | `metrics/fault_sim.csv` |
| Harness self-test | Seven deliberate bugs. Every real bug is caught; the one survivor is an equivalent mutant, because writing the CRC word last is still atomic. | See the commit log |
| Silicon | `bootloader_fi`: chip reset before, after, and in the middle of each flash op of real trial / confirm / compaction / rollback sequences. Mid-op resets abort real word programs and real sector erases. | **646/646 pass** (`metrics/board_fault_injection.json`) |
| Power cut | Not tested. No power-switching rig. | — |
