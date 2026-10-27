#!/usr/bin/env python3
"""Phase 0 UART checks against target/feasibility/uartcheck.c.

  uart_check.py [--port /dev/cu.usbserial-0001] [--no-flash]

1. Echo: random data at 115200, 460800 and 921600 baud, compared byte for byte.
2. Erase stall (F0.4): a burst arrives while the board erases a 128 KB sector
   with the CPU stalled; DMA must receive every byte (CRC checked).
WARNING: overwrites the bootloader region and erases S11.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import board as B  # noqa: E402

BAUDS = [115200, 460800, 921600]


def open_port(port: str, baud: int) -> serial.Serial:
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.5
    # DTR/RTS drive the ESP32 board's auto-reset transistors; keep them idle.
    s.dtr = s.rts = False
    s.open()
    return s


def readline(s: serial.Serial, timeout: float = 3.0) -> str:
    t0, buf = time.time(), b""
    while time.time() - t0 < timeout:
        buf += s.read(1)
        if buf.endswith(b"\n"):
            return buf.decode(errors="replace").strip()
    raise TimeoutError(f"no reply (got {buf!r})")


def set_baud(s: serial.Serial, baud: int) -> int:
    s.reset_input_buffer()
    s.write(b"B" + struct.pack("<I", baud))
    line = readline(s)
    if not line.startswith("OK"):
        raise RuntimeError(f"baud switch: {line!r}")
    time.sleep(0.05)
    s.baudrate = baud
    time.sleep(0.05)
    s.reset_input_buffer()
    return int(line.split()[1])


def echo(s: serial.Serial, n: int) -> dict:
    data = os.urandom(n)
    got = bytearray()
    s.write(b"E" + struct.pack("<I", n))

    def writer() -> None:
        for i in range(0, n, 1024):
            s.write(data[i:i + 1024])

    t0 = time.time()
    th = threading.Thread(target=writer)
    th.start()
    while len(got) < n and time.time() - t0 < 10 + n * 20 / s.baudrate:
        got += s.read(n - len(got))
    th.join()
    secs = time.time() - t0
    tail = readline(s)
    mismatch = sum(a != b for a, b in zip(got, data)) + abs(len(got) - n)
    return {"bytes": n, "received": len(got), "mismatched_bytes": mismatch,
            "uart_errors": int(tail.split()[1]) if tail.startswith("ED") else None,
            "seconds": round(secs, 3), "echo_KBps": round(n / secs / 1024, 1)}


def erase_stall(s: serial.Serial, n: int, sector: int = 11) -> dict:
    data = os.urandom(n)
    s.reset_input_buffer()
    s.write(b"S" + struct.pack("<II", n, sector))
    if readline(s) != "SR":
        raise RuntimeError("erase-stall: no SR")
    s.write(data)
    line = readline(s, timeout=10)
    f = line.split()
    if f[0] != "SD":
        raise RuntimeError(f"erase-stall: {line!r}")
    erase_us, during, received, crc, errors, max_fill = (int(x) for x in f[1:])
    return {"burst_bytes": n, "erase_ms": round(erase_us / 1000, 1),
            "bytes_received_during_erase": during, "received": received,
            "crc_ok": crc == zlib.crc32(data), "uart_errors": errors, "max_ring_fill": max_fill}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--no-flash", action="store_true")
    a = ap.parse_args()
    if not a.no_flash:
        o = B.OpenOCD()
        try:
            o.cmd("reset halt")
            o.program(B.BUILD / "uartcheck.elf")
            o.cmd("reset run")
        finally:
            o.close()   # no SWD traffic during the tests
        time.sleep(0.5)

    res: dict = {"port": a.port, "echo": {}, "erase_stall": {}}
    ok = True
    s = open_port(a.port, 115200)
    try:
        set_baud(s, 115200)
        for baud in BAUDS:
            actual = set_baud(s, baud)
            r = echo(s, 32 * 1024)
            r["actual_baud"] = actual
            r["baud_error_pct"] = round((actual - baud) / baud * 100, 2)
            res["echo"][str(baud)] = r
            good = r["mismatched_bytes"] == 0 and r["uart_errors"] == 0
            ok &= good
            print(f"echo {baud:>7} (device {actual}, {r['baud_error_pct']:+.2f}%): "
                  f"{r['received']}/{r['bytes']} B, {r['mismatched_bytes']} mismatched, "
                  f"uart_errors={r['uart_errors']}, {r['echo_KBps']} KB/s  "
                  f"[{'PASS' if good else 'FAIL'}]")
        for baud, n in [(921600, 40 * 1024), (115200, 16 * 1024)]:
            set_baud(s, baud)
            r = erase_stall(s, n)
            res["erase_stall"][str(baud)] = r
            good = r["crc_ok"] and r["received"] == n and r["uart_errors"] == 0 and \
                r["bytes_received_during_erase"] > 0
            ok &= good
            print(f"erase-stall {baud:>7}: 128 KB erase {r['erase_ms']} ms, "
                  f"{r['bytes_received_during_erase']} B arrived during the erase, "
                  f"{r['received']}/{n} received, crc_ok={r['crc_ok']}, "
                  f"uart_errors={r['uart_errors']}  [{'PASS' if good else 'FAIL'}]")
        set_baud(s, 115200)
    finally:
        s.close()
    out = B.ROOT / "metrics" / "board_uart.json"
    out.write_text(json.dumps(res, indent=2) + "\n")
    print(f"wrote {out}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
