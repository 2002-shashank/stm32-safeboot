# Threat model

## Assets
1. **Code integrity:** only firmware signed by the key holder runs.
2. **Version integrity:** a device cannot be moved to an older signed
   version through the update path.
3. **Availability:** a power loss, bad image or crashing release never
   bricks the device.

## Defended

| Threat | Defense | Where tested |
|---|---|---|
| Forged or modified image, over the update path or written into a slot | SHA-256 of the payload, plus an Ed25519 signature over the header (including the digest, version, size, load address and flags). Verified before *every* boot, not only at install. | `test_image_verify`, `test_tools.py`, the on-board `tamper` scenario |
| Image built or signed for the other slot, or for the recovery region | `load_addr` and the recovery flag are signed and must match the region the image sits in | `test_image_verify` |
| Downgrade to an older signed image through the update path | `min_version`: an install needs `version > min_version`, and a boot needs `version ≥ min_version`. The floor rises only when an image is confirmed, so an automatic rollback to the previous image stays possible. | `test_boot_flow`, the on-board `confirm` scenario |
| Power loss or reset during any flash write | The journal design (`docs/journal.md`) | Simulator, plus silicon fault injection |
| A signed but broken release (crashes or hangs during init) | Trial boots with `max_attempts = 3`, the IWDG during trials, and `boot_confirm()` only after the app has initialized. Then automatic rollback. | The on-board `rollback-crash` and `rollback-hang` scenarios |
| Corrupted transfer (UART noise, duplicates, gaps) | A per-frame CRC32, the offset protocol, and verification of the whole image before VALID | `test_proto_edges`. The UART link itself is not yet tested. |

## Not defended: explicit non-goals

The STM32F407 has no TrustZone, no secure element, no OTP key storage and no
hardware root of trust. Nothing below is claimed.

- **An attacker with SWD/JTAG or physical access to the flash.** They can
  read out the firmware, replace the bootloader or the public key in S4,
  erase the journal (which resets `min_version`, the anti-rollback counter),
  or simply jump past verification with a debugger.
- **RDP is not a security boundary here.** RDP Level 1 still lets a
  debugger attach and mass-erase the chip. RDP Level 2 permanently disables
  debug, but published fault-injection attacks on the STM32F2/F4 family
  show RDP can be downgraded (for example, the 2018 wallet.fail attack on
  the STM32F205). WRP on S0, S1 and S4 stops only accidental or software
  writes. It can be removed by anyone who can change option bytes.
- **Fault injection on the verifier (voltage or clock glitching) to skip
  the signature check.** There is cheap hardening only (a constant-time
  hash compare, and vector-table checks after the signature). There is no
  claim of resistance.
- **Confidentiality.** Images are not encrypted. Anyone on the UART link,
  or with flash access, sees the firmware.
- **A compromised signing key.** `key_id` exists so that revocation can be
  added later. Today a new key needs reprogramming S4 over SWD.
- **Side channels.** Ed25519 *verification* handles only public data.
  Signing happens on the host. The development key in `keys/` is
  unencrypted and must not be used for production images (set
  `SB_KEY_PASSPHRASE` to encrypt it).
- **Denial of service over UART.** Anyone who can talk to the update UART
  can erase the inactive slot by starting an update. That costs the
  fallback image, but never the running one: updates target only the
  non-active slot.

## Residual risks inside the defended scope
- **Weak bits after a torn write.** On resume, a word that reads back as
  the expected value is accepted, even though it might be a weakly
  programmed cell left by an interrupted write. Safety is not affected:
  the whole image is hashed at VERIFY and on every boot. If such a bit
  flips later, that boot fails verification. The device then falls back to
  the other slot if that slot is still at or above `min_version`, and
  otherwise to the golden image or bootloader recovery mode.
- **A stack overflow inside a fault handler.** This escalates to a lockup
  reset with no crash record. The reset is still counted as a failed trial
  attempt.
