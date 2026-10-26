#!/usr/bin/env python3
"""Cross-language tests: Python tools vs the C core (via build/host/sbhost).

  test_tools.py <path-to-sbhost>
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sbimage as sb  # noqa: E402
import sbjournal as sj  # noqa: E402

TOOL = Path(__file__).resolve().parent / "sbtool.py"
failures = 0


def check(cond: bool, what: str) -> None:
    global failures
    print(f"[{'PASS' if cond else 'FAIL'}] {what}")
    failures += not cond


def run(*args: str, ok: bool = True) -> subprocess.CompletedProcess:
    p = subprocess.run(args, capture_output=True, text=True)
    if ok and p.returncode:
        raise SystemExit(f"{args} failed:\n{p.stdout}{p.stderr}")
    return p


def fake_app(load_addr: int, size: int) -> bytes:
    """Minimal payload with a sane vector table for the given slot."""
    sp = sb.APP_RAM_TOP
    reset = load_addr + sb.HDR_SIZE + 0x200 + 1
    body = bytes((i * 7 + 3) & 0xFF for i in range(size - 8))
    return struct.pack("<II", sp, reset) + body


def main() -> None:
    sbhost = sys.argv[1]
    py = sys.executable
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        key, prov = d / "k.pem", d / "prov.bin"
        run(py, str(TOOL), "keygen", "--out", str(key))
        run(py, str(TOOL), "provision", "--key", str(key), "--out", str(prov))

        # Layout constants parsed from the C headers.
        check(sb.HDR_SIZE == 512 and sb.REGIONS["B"][0] == 0x08080000
              and sb.REGIONS["A"][1] == 0x60000, "layout constants parsed from layout.h")

        for slot in ("A", "B", "recovery"):
            addr = sb.REGIONS[slot][0]
            app = d / f"app_{slot}.bin"
            app.write_bytes(fake_app(addr, 6001))   # odd size: tool pads to a word
            img = d / f"app_{slot}.sbimg"
            run(py, str(TOOL), "sign", "--key", str(key), "--slot", slot, "--version", "5",
                "--in", str(app), "--out", str(img))
            p = run(sbhost, "verify", str(prov), str(img), ok=False)
            check(p.returncode == 0, f"C verifier accepts Python-signed image for slot {slot}")
            p = run(py, str(TOOL), "inspect", str(img), "--prov", str(prov), ok=False)
            check(p.returncode == 0 and "signature   OK" in p.stdout, f"sbtool inspect agrees ({slot})")

            raw = bytearray(img.read_bytes())
            raw[sb.HDR_SIZE + 3000] ^= 0x40
            bad = d / "bad.sbimg"
            bad.write_bytes(raw)
            p = run(sbhost, "verify", str(prov), str(bad), ok=False)
            check(p.stdout.strip() == "-4", f"C verifier rejects payload bit flip ({slot}): BAD_HASH")
            raw = bytearray(img.read_bytes())
            raw[8] ^= 0x01   # image_version, covered by the signature
            bad.write_bytes(raw)
            p = run(sbhost, "verify", str(prov), str(bad), ok=False)
            check(p.stdout.strip() == "-5", f"C verifier rejects header change ({slot}): BAD_SIG")

        # Signing refuses an image linked for the other slot.
        app = d / "wrong.bin"
        app.write_bytes(fake_app(sb.REGIONS["A"][0], 4096))
        p = run(py, str(TOOL), "sign", "--key", str(key), "--slot", "B", "--version", "1",
                "--in", str(app), "--out", str(d / "w.sbimg"), ok=False)
        check(p.returncode != 0 and "wrong slot" in p.stderr, "sign refuses an image linked for slot A as B")

        # A different key: C verifier must reject (key id mismatch).
        key2, prov2 = d / "k2.pem", d / "prov2.bin"
        run(py, str(TOOL), "keygen", "--out", str(key2))
        run(py, str(TOOL), "provision", "--key", str(key2), "--out", str(prov2))
        p = run(sbhost, "verify", str(prov2), str(d / "app_A.sbimg"), ok=False)
        check(p.stdout.strip() == "-3", "C verifier rejects image signed by another key: BAD_KEY")

        # Journal: C writes 300 records (crosses one compaction); Python mirror
        # must find the same current record.
        jf = d / "j.bin"
        run(sbhost, "journal", str(jf), "300")
        raw = jf.read_bytes()
        j = sj.scan((raw[:sj.PAGE_SIZE], raw[sj.PAGE_SIZE:]))
        check(j.cur is not None and j.cur.seq == 300 and j.cur.min_version == 300
              and j.cur.version_a == 7 and j.cur.state_a == sj.ST["CONFIRMED"],
              "Python journal mirror reads C-written journal (seq 300 after compaction)")
        check(j.page == 1 and j.next_off == (300 - 256) * 64,
              "mirror agrees on active page and next free offset")
        rec = sj.Record.unpack(j.cur.pack())
        check(rec.sealed() == j.cur.pack(), "Python record sealing reproduces the C CRC")

    print(f"{'all passed' if not failures else f'{failures} FAILED'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    os.environ.pop("SB_KEY_PASSPHRASE", None)
    main()
