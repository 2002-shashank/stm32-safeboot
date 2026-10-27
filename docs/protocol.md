# UART update protocol

Device side: `core/src/proto.c` (transport-independent) and
`target/bootloader/main.c` (UART transport, SET_BAUD). Host side:
`tools/sbflash.py`. Link: USART2 on PA2/PA3 through a 3.3 V USB-serial
bridge, 8N1, starting at 115200 baud.

## Entering update mode
- The running app receives `SBUPDATE` on its UART and calls the bootloader
  API `boot_request_update()`. That sets a flag in backup SRAM and resets.
- The user button (PA0) is held at reset.
- Nothing is bootable.

If update mode was entered by request or button, it reboots after 30 s
without traffic. After 3 s without traffic the link falls back to
115200 baud, so a new session can start.

## Framing
`A5 5A | type u8 | seq u16 | len u16 | payload | crc32`. All fields are
little-endian, and the CRC-32 (zlib) covers type through payload. A frame
with a bad CRC is dropped silently, and the host retries after its timeout.
A partial frame is abandoned after a 100 ms gap. Every reply has type
`cmd | 0x80`, the request's seq, and a 16-byte payload:
`status u8, slot u8, slot_state u8, 0, next_offset u32, a u32, b u32`.

| Cmd | Payload | Reply fields |
|---|---|---|
| 0x01 BEGIN_UPDATE | session u32 | a = max image size |
| 0x02 IMAGE_INFO | 512 B header | next_offset (nonzero when resuming) |
| 0x03 DATA | offset u32, crc32 u32, 4–1024 B (word multiple) | next_offset |
| 0x04 QUERY_PROGRESS | — | slot, slot_state, a = slot version, b = rx session, next_offset |
| 0x05 FINALIZE | total u32 | |
| 0x06 VERIFY | activate u8 | a = verify result, b = version |
| 0x07 REBOOT | — | (device resets after replying) |
| 0x08 SET_BAUD | baud u32 | a = actual rate (reply at the old rate, then switch) |

Status codes: 0 OK, 1 ERR_SEQ (resend from next_offset), 2 ERR_CRC,
3 ERR_STATE, 4 ERR_HDR, 5 ERR_VERSION (anti-rollback), 6 ERR_RANGE,
7 ERR_FLASH, 8 ERR_VERIFY, 9 RESTART (resend IMAGE_INFO), 10 ERR_LEN,
11 ERR_CMD.

## Reliability rules
- **Stop-and-wait.** One request is in flight at a time. While the device
  erases a sector (about 1 s per 128 KB) it holds the reply back, and the
  host waits (2.5 s timeout). RX runs on DMA during the stall, measured
  with zero loss at 921600 baud.
- **Retransmits reuse the seq.** The device answers a repeated (type, seq)
  from its reply cache without executing it again. A DATA offset below
  next_offset is acknowledged without writing.
- **The session id is the first 4 bytes of SHA-256(header).** Rerunning the
  same image resumes: from the RAM position if the device stayed in update
  mode, otherwise from the last 16 KB journal checkpoint. On resume, data
  that is already written is compared and skipped. A mismatch (a torn word)
  rewinds that sector: the checkpoint is lowered first, then the sector is
  erased.
- **Baud resync.** After more than 3.2 s with no valid reply, the host looks
  for the device at 115200 and renegotiates.
- **Nothing is trusted until VERIFY.** It checks SHA-256 and Ed25519 over
  the stored image, then writes VALID or PENDING. The bootloader verifies
  again before every boot.

## Measured (`metrics/board_uart_update.json`)

| | Result |
|---|---|
| 391 KB image at 921600 / 460800 baud | 43.6 / 29.0 KB/s goodput (stop-and-wait, including sector erases) |
| Injected 5% drop, 5% corrupt, 2% duplicate, 5% lost ACK | Completes. Each lost frame costs a 2.5 s timeout. |
| Link drop at 200 KB / device reset at 200 KB | Resumes at the exact offset / at the 16 KB checkpoint |
| Older version, tampered image | Refused at IMAGE_INFO / rejected at VERIFY |
