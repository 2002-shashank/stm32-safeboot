#!/usr/bin/env python3
"""On-board UART update tests (CP2102 bridge on PA2/PA3), Phase 5.

Each step runs tools/sbflash.py's update over the serial link only; SWD is
used just to deploy the starting state and to read the journal while the
board is idle. Writes metrics/board_uart_update.json.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import board as B  # noqa: E402
import sbflash  # noqa: E402
import sbimage as sb  # noqa: E402
import sbjournal as sj  # noqa: E402

IMG = B.BUILD
PORT = "/dev/cu.usbserial-0001"
results: list[dict] = []


def update(images: list[str], **kw) -> tuple[dict | None, str]:
    a = argparse.Namespace(port=PORT, images=[str(IMG / f"{i}.sbimg") if "/" not in i else i
                                              for i in images],
                           baud=921600, no_activate=False, no_reboot=False, drop=0.0, corrupt=0.0,
                           dup=0.0, lose_ack=0.0, abort_after=0, seed=1)
    for k, v in kw.items():
        setattr(a, k, v)
    try:
        return sbflash.do_update(a), ""
    except SystemExit as e:
        return None, str(e)


last_pc_region = ""


def journal(settle: float = 2.5) -> sj.Record:
    """Journal state, read with the core halted once the board is idle (no
    SWD flash reads during an erase). Also records where the CPU is."""
    global last_pc_region
    time.sleep(settle)
    o = B.OpenOCD()
    try:
        o.cmd("halt")
        pc = int(o.cmd("reg pc").split(":")[-1], 16)
        last_pc_region = ("bootloader" if pc < 0x08008000 else "slot A" if pc < 0x08080000
                          else "slot B" if pc < 0x080E0000 else f"0x{pc:08X}")
        cur = B.read_journal(o).cur
        o.cmd("resume")
    finally:
        o.close()
    return cur


def reboot_device(baud: int = 921600) -> str:
    """Leave update mode. The session may have left the link at `baud`, and
    probes at another rate keep resetting the device's idle fallback timer,
    so probe at both."""
    try:
        link = sbflash.Link(PORT, 115200, {}, 0)
        try:
            sbflash.enter_update_mode(link, also_baud=baud)
            link.call("REBOOT", timeout=0.5, retries=2)
        finally:
            link.close()
        return ""
    except Exception as e:   # noqa: BLE001
        return f" (reboot: {e})"


def swd_reset() -> None:
    o = B.OpenOCD()
    try:
        o.cmd("reset run")
    finally:
        o.close()


def check(name: str, ok: bool, detail: dict) -> None:
    results.append({"test": name, "pass": bool(ok), **detail})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")


def confirmed(r: sj.Record, slot: int, ver: int) -> bool:
    return (r is not None and r.active == slot and r.state(slot) == sj.ST["CONFIRMED"]
            and r.min_version == ver)


def main() -> None:
    o = B.OpenOCD()
    try:
        B.deploy(o, ["app_a_v1"])
        o.cmd("reset run")
    finally:
        o.close()
    time.sleep(1.0)

    # 1-2: full near-maximum images at 921600 and 460800 baud.
    for baud, images, slot, ver in [(921600, ["app_a_max_v3", "app_b_max_v2"], 1, 2),
                                    (460800, ["app_a_max_v3", "app_b_max_v4"], 0, 3)]:
        res, err = update(images, baud=baud)
        r = journal()
        check(f"full {res and res['bytes']} B image at {baud} baud, trial-boots and confirms",
              res is not None and confirmed(r, slot, ver), {"result": res, "error": err,
                                                           "journal": r and r.describe()})

    # 3: injected link faults.
    res, err = update(["app_b_max_v4", "app_a_max_v5"], drop=0.05, corrupt=0.05, dup=0.02,
                      lose_ack=0.05, seed=7)
    r = journal()
    check("injected faults (5% drop, 5% corrupt, 2% duplicate, 5% lost ACK): completes and confirms",
          res is not None and confirmed(r, 1, 4) and res["retries"] > 0,
          {"result": res, "error": err, "journal": r and r.describe()})

    # 4: link drop mid-transfer, then resume (device kept RAM progress).
    part, _ = update(["app_a_max_v5", "app_b_max_v6"], abort_after=200 * 1024)
    res, err = update(["app_a_max_v5", "app_b_max_v6"])
    r = journal()
    check("host stops at ~200 KB (link drop), rerun resumes at the exact offset",
          part is not None and res is not None and res["resumed_from"] == part["aborted_at"]
          and confirmed(r, 0, 5),
          {"aborted_at": part and part["aborted_at"], "result": res, "error": err,
           "journal": r and r.describe()})

    # 5: device reset mid-transfer (power-cut analog): resume from the journal checkpoint.
    part, _ = update(["app_b_max_v6", "app_a_max_v7"], abort_after=200 * 1024)
    swd_reset()
    time.sleep(1.0)
    res, err = update(["app_b_max_v6", "app_a_max_v7"])
    r = journal()
    ok = (part is not None and res is not None and 0 < res["resumed_from"] <= part["aborted_at"]
          and res["resumed_from"] % 0x4000 == 0 and confirmed(r, 1, 6))
    check("device reset at ~200 KB, rerun resumes from the last 16 KB journal checkpoint",
          ok, {"aborted_at": part and part["aborted_at"], "result": res, "error": err,
               "journal": r and r.describe()})

    # 6: 115200 baud for the first 96 KB, then finish at 921600 (resume across baud rates).
    part, _ = update(["app_b_max_v6", "app_a_max_v7"], baud=115200, abort_after=96 * 1024)
    res, err = update(["app_b_max_v6", "app_a_max_v7"], baud=921600)
    r = journal()
    check("first 96 KB at 115200 baud, resumed and finished at 921600",
          part is not None and res is not None and res["resumed_from"] > 0 and confirmed(r, 0, 7),
          {"partial_115200": part, "result": res, "error": err, "journal": r and r.describe()})

    # 7: anti-rollback: v2 must be refused (device floor is 7).
    res, err = update(["app_b_max_v2"])
    rb = reboot_device()
    r = journal()
    check("older version (v2 < floor 7) refused at IMAGE_INFO; v7 keeps running from slot A",
          res is None and "anti-rollback" in err and not rb and confirmed(r, 0, 7)
          and last_pc_region == "slot A",
          {"error": err + rb, "running": last_pc_region, "journal": r and r.describe()})

    # 8: tampered image: signature/hash check at VERIFY fails, slot marked INVALID.
    bad = B.ROOT / "build" / "tampered_b_v8.sbimg"
    shutil.copy(IMG / "app_b_max_v8.sbimg", bad)
    raw = bytearray(bad.read_bytes())
    raw[sb.HDR_SIZE + 200000] ^= 0x01
    bad.write_bytes(raw)
    res, err = update([str(bad)])
    rb = reboot_device()
    r = journal()
    check("tampered v8 transferred, rejected at VERIFY (slot B INVALID), v7 keeps running from slot A",
          res is None and "VERIFY failed" in err and not rb and r is not None
          and r.state_b == sj.ST["INVALID"] and confirmed(r, 0, 7) and last_pc_region == "slot A",
          {"error": err + rb, "running": last_pc_region, "journal": r and r.describe()})

    out = B.ROOT / "metrics" / "board_uart_update.json"
    out.write_text(json.dumps(results, indent=2) + "\n")
    passed = sum(x["pass"] for x in results)
    print(f"{passed}/{len(results)} passed; wrote {out}")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
