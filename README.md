# stm32-safeboot

[![ci](https://github.com/2002-shashank/stm32-safeboot/actions/workflows/ci.yml/badge.svg)](https://github.com/2002-shashank/stm32-safeboot/actions/workflows/ci.yml)

A secure, power-fail-safe A/B bootloader for the STM32F407 (tested on the
STM32F407G-DISC1). It provides:
- Ed25519-signed images, verified with SHA-256 before every boot.
- Anti-rollback.
- Trial boots with automatic rollback.
- A power-fail-atomic boot journal.
- A resumable UART update path.
- A crash record that survives resets.

Every claim below has a test behind it and a number in
[`metrics/metrics.md`](metrics/metrics.md).

## Results on hardware

| | |
|---|---|
| Validated boot (Reset_Handler → app `main`) | **22.6 ms** for a 4.3 KB image, **175 ms** for a 391 KB image (SHA-256 156 ms, Ed25519 17.8 ms) |
| Bootloader size | 22.8 KB of 32 KB (Ed25519 + SHA-512 is 10.5 KB of that) |
| Power-cut tests, simulator | 124,930 / 124,930: a cut at every flash op × 5 tear variants |
| Reset injection on silicon | **646 / 646**: before, after and *during* each flash op, including real aborted word programs and partial sector erases |
| Automatic rollback | 74 ms after 3 crashing trial boots; 10.8 s after 3 hanging ones (IWDG) |
| UART update, 391 KB | 43.7 KB/s at 921,600 baud. Completes under 5% drop, 5% corrupt, 2% duplicate and 5% lost-ACK faults. Resumes after a link drop or a device reset. |
| Rejected | Tampered, wrong-slot, wrong-key and older-version images |

**Not covered:** real power cuts, since there is no power-switching rig.
Software and mid-op resets stand in for them. Read the
[threat model](docs/threat-model.md) before relying on any of this: nothing
on the F407 resists an attacker with debug or physical access to the flash.

## Hardware setup

- STM32F407G-DISC1, flashed and logged over its on-board ST-LINK (SWD,
  with RTT for logs).
- A 3.3 V USB-serial bridge on USART2. The DISC1's ST-LINK serial port is
  **not** wired to the target. Tested with the CP2102 of an ESP32 dev board,
  with the ESP32 held in reset (EN to GND).

| Bridge | STM32 |
|---|---|
| TXD | PA3 (USART2 RX) |
| RXD | PA2 (USART2 TX) |
| GND | GND |

## Build

Requirements:
- CMake and Ninja.
- The [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads).
  The default path is `~/opt/arm-gnu-toolchain-15.3.rel2`; override it with
  `ARM_TOOLCHAIN_DIR`.
- OpenOCD.
- Python 3 with `cryptography` and `pyserial`.

```sh
python3 -m venv .venv && .venv/bin/pip install cryptography pyserial
.venv/bin/python tools/sbtool.py keygen --out keys/dev_ed25519.pem   # dev key, never committed

# Host: unit tests (ASan/UBSan), cross-language tests, power-cut harness
cmake -B build/host -G Ninja && ninja -C build/host && ctest --test-dir build/host -LE slow
cmake -B build/harness -G Ninja -DCMAKE_BUILD_TYPE=Release -DSB_SANITIZE=OFF
ninja -C build/harness fault_harness && ./build/harness/fault_harness --quick

# Firmware: bootloader, signed apps per slot, bench and check images
cmake -B build/target -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=MinSizeRel
ninja -C build/target
```

## Use

```sh
# Factory-program: bootloader, provisioning block (public key), image for slot A
.venv/bin/python tools/board.py deploy app_a_v1

# Update over UART. Pass both slot builds; the device picks the inactive slot.
.venv/bin/python tools/sbflash.py update build/target/target/app_a_v3.sbimg \
    build/target/target/app_b_v2.sbimg --port /dev/cu.usbserial-0001

# Live log over SWD
.venv/bin/python tools/board.py log --secs 5
```

An update proceeds like this:
1. The new image boots as a trial, with the IWDG running.
2. The app calls `boot_confirm()` once it has initialized.
3. If it doesn't confirm within 3 boots, the bootloader rolls back to the
   previous image.

## On-board test suites

| Command | What it does |
|---|---|
| `tools/board.py bench` | Crypto, flash and journal timings |
| `tools/board.py retention` | What survives each reset type (`--after-power-cycle` after a USB replug) |
| `tools/board.py run-scenario all` | Trial/confirm, rollback by crash and by hang, tamper, crash records, empty device |
| `tools/board.py boot-timing` | DWT boot breakdown for small and near-maximum images |
| `tools/board.py hw-faults` | Reset injection at every flash op (`bootloader_fi` build) |
| `tools/uart_check.py` | UART echo at 3 rates, plus DMA reception during a 128 KB erase |
| `tools/uart_update_test.py` | The UART update suite |
| `tools/metrics_report.py` | Regenerates `metrics/metrics.md` |

## Repository

| Path | Contents |
|---|---|
| `core/` | Portable boot logic: journal, image verification, state machine, update protocol, crash record. Built for host and target from one source. |
| `host/` | Flash simulator with power-cut injection, unit tests, fault harness, `sbhost` |
| `target/` | Startup, linker scripts, flash driver (RAM-resident), UART/DMA, RTT, bootloader, test app, bench |
| `tools/` | Signing (`sbtool.py`), UART client (`sbflash.py`), SWD board runner, test suites, metrics report |
| `docs/` | [Flash layout](docs/flash-layout.md), [journal](docs/journal.md), [protocol](docs/protocol.md), [threat model](docs/threat-model.md) |
| `metrics/` | Raw results (JSON/CSV) and the generated report |
| `PLAN.md` | The plan, Phase 0 feasibility record, per-phase status |

Third-party code is in `third_party/`: Monocypher, CMSIS, and ST device
headers. Versions, licenses and hashes are in its README.
