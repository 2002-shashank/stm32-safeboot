#!/usr/bin/env python3
"""Drive the STM32F407G-DISC1 over SWD (OpenOCD): flash, reset, read/write
memory, capture the RTT log, and run the on-board measurements.

  board.py bench                  flash the bench firmware, collect results
  board.py retention              reset-type retention matrix (needs bench)
  board.py deploy [images...]     bootloader + provisioning + images, then log
  board.py log [--secs N]         attach (no reset) and print RTT output
  board.py run-scenario NAME      on-board scenarios (see SCENARIOS)
"""
from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "target" / "target"
NM = Path.home() / "opt/arm-gnu-toolchain-15.3.rel2/bin/arm-none-eabi-nm"
TCL_PORT, RTT_PORT = 6666, 9090
RTT_ADDR = 0x2001F000
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sbimage as sb  # noqa: E402
import sbjournal as sj  # noqa: E402


class OpenOCD:
    def __init__(self) -> None:
        # Log to a file: a full stdout pipe would block openocd.
        self.log_path = ROOT / "build" / "openocd.log"
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            ["openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg",
             "-c", f"tcl_port {TCL_PORT}; telnet_port disabled; gdb_port disabled",
             "-c", "init"],
            stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", TCL_PORT), timeout=60)
                break
            except OSError:
                if self.proc.poll() is not None or time.time() > deadline:
                    self.proc.kill()
                    raise SystemExit(f"openocd failed to start:\n{self.log_path.read_text()}")
                time.sleep(0.1)
        self.rtt = None

    def cmd(self, c: str) -> str:
        self.sock.sendall(c.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("openocd closed the connection")
            buf += chunk
        return buf[:-1].decode(errors="replace").strip()

    def read32(self, addr: int, n: int = 1) -> list[int]:
        vals: list[int] = []
        for i in range(0, n, 1024):
            k = min(1024, n - i)
            out = self.cmd(f"read_memory 0x{addr + 4 * i:08X} 32 {k}")
            vals += [int(x, 0) for x in out.split()]
        return vals

    def write32(self, addr: int, values: list[int]) -> None:
        self.cmd(f"write_memory 0x{addr:08X} 32 {{{' '.join(hex(v) for v in values)}}}")

    def program(self, path: Path, addr: int | None = None) -> None:
        where = "" if addr is None else f" 0x{addr:08X}"
        out = self.cmd(f"program {{{path}}} verify{where}")
        if "Verified OK" not in out and "** Programming Finished **" not in out:
            # program prints to the log, not the result; check the error marker
            if "rror" in out:
                raise RuntimeError(f"program {path} failed: {out}")

    def erase_sectors(self, first: int, last: int) -> None:
        self.cmd("reset halt")
        self.cmd(f"flash erase_sector 0 {first} {last}")

    def rtt_start(self) -> None:
        self.cmd(f'rtt setup 0x{RTT_ADDR:08X} 0x100 "SEGGER RTT"')
        self.cmd("rtt polling_interval 1")
        self.cmd("rtt start")
        self.cmd(f"rtt server start {RTT_PORT} 0")
        self.rtt = socket.create_connection(("127.0.0.1", RTT_PORT), timeout=0.2)

    def rtt_lines(self, secs: float, until: str | None = None) -> list[tuple[float, str]]:
        """Collect RTT output as (host time, line) until `until` appears in a
        line or secs elapse. Host timestamps are within the RTT polling
        interval (1 ms requested) plus USB latency."""
        lines, part, t0 = [], b"", time.time()
        while time.time() - t0 < secs:
            try:
                chunk = self.rtt.recv(4096)
            except socket.timeout:
                continue
            now = time.time()
            part += chunk
            while b"\n" in part:
                line, part = part.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip("\r")
                lines.append((now, text))
                if until and until in text:
                    return lines
        if part:
            lines.append((time.time(), part.decode(errors="replace")))
        return lines

    def rtt_read(self, secs: float, until: str | None = None) -> str:
        return "\n".join(t for _, t in self.rtt_lines(secs, until))

    def rtt_send(self, data: bytes) -> None:
        self.rtt.sendall(data)

    def close(self) -> None:
        try:
            self.cmd("shutdown")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
            self.proc.wait()
        self.log.close()


def symbols(elf: Path) -> dict[str, int]:
    out = subprocess.run([str(NM), str(elf)], capture_output=True, text=True, check=True).stdout
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}


BENCH_FIELDS = [
    "magic", "hz", "sha_64k_cyc", "sha_384k_cyc", "crc32_1k_cyc", "ed_verify_cyc", "ed_ok",
    "ed_tampered_rejected", "ed_stack_bytes", "erase16_cyc", "erase64_cyc", "erase128_cyc",
    "prog16k_cyc", "prog_ok", "erase_ok", "jscan_empty_cyc", "jappend_cyc", "jscan_full_cyc",
    "jcompact_cyc", "journal_ok", "boot_count", "rcc_csr", "bkpsram_kept", "noinit_kept",
    "debugger_attached", "cyccnt_counts",
]


def read_bench(o: OpenOCD, addr: int) -> dict[str, int]:
    vals = o.read32(addr, len(BENCH_FIELDS))
    return dict(zip(BENCH_FIELDS, vals))


def wait_bench(o: OpenOCD, addr: int, secs: float = 20) -> dict[str, int]:
    t0 = time.time()
    while time.time() - t0 < secs:
        r = read_bench(o, addr)
        if r["magic"] == 0xBE7C0001:
            return r
        time.sleep(0.3)
    raise SystemExit("bench did not finish")


def cmd_bench(a) -> None:
    elf = BUILD / "bench.elf"
    sym = symbols(elf)
    o = OpenOCD()
    try:
        o.cmd("reset halt")
        o.program(elf)
        o.cmd("reset run")
        o.rtt_start()
        r = wait_bench(o, sym["bench"])
        log = o.rtt_read(1.0)
    finally:
        o.close()
    hz = r["hz"]
    us = lambda c: c * 1e6 / hz  # noqa: E731
    res = {
        "board": "STM32F407G-DISC1 MB997E", "clock_hz": hz,
        "sha256_64k_us": us(r["sha_64k_cyc"]), "sha256_384k_us": us(r["sha_384k_cyc"]),
        "sha256_MBps": 384 * 1024 / (us(r["sha_384k_cyc"]) / 1e6) / 1e6,
        "sha256_cycles_per_byte": r["sha_384k_cyc"] / (384 * 1024),
        "crc32_1k_us": us(r["crc32_1k_cyc"]),
        "ed25519_verify_us": us(r["ed_verify_cyc"]), "ed25519_verify_cycles": r["ed_verify_cyc"],
        "ed25519_ok": r["ed_ok"], "ed25519_tampered_rejected": r["ed_tampered_rejected"],
        "ed25519_stack_bytes": r["ed_stack_bytes"],
        "erase_16k_ms": us(r["erase16_cyc"]) / 1e3, "erase_64k_ms": us(r["erase64_cyc"]) / 1e3,
        "erase_128k_ms": us(r["erase128_cyc"]) / 1e3, "erase_ok": r["erase_ok"],
        "program_16k_ms": us(r["prog16k_cyc"]) / 1e3,
        "program_KBps": 16 / (us(r["prog16k_cyc"]) / 1e6),
        "program_us_per_word": us(r["prog16k_cyc"]) / 4096, "program_ok": r["prog_ok"],
        "journal_scan_empty_us": us(r["jscan_empty_cyc"]),
        "journal_scan_full_page_us": us(r["jscan_full_cyc"]),
        "journal_append_us": us(r["jappend_cyc"]),
        "journal_compacting_append_ms": us(r["jcompact_cyc"]) / 1e3, "journal_ok": r["journal_ok"],
        "cyccnt_counts": r["cyccnt_counts"], "debugger_attached_at_boot": r["debugger_attached"],
    }
    out = ROOT / "metrics" / "board_bench.json"
    out.write_text(json.dumps(res, indent=2) + "\n")
    print(log.strip())
    print(json.dumps(res, indent=2))
    print(f"wrote {out}")


def cmd_retention(a) -> None:
    """Reset-type matrix: does backup SRAM / .noinit RAM survive, and what
    does RCC_CSR report? (Power cycle is a manual step, see --after-power-cycle.)"""
    elf = BUILD / "bench.elf"
    sym = symbols(elf)
    rows = []
    o = OpenOCD()
    try:
        if a.after_power_cycle:
            r = read_bench(o, sym["bench"])
            rows.append(("power cycle (USB unplug/replug, no debugger)", r))
        else:
            o.cmd("reset halt")
            o.program(elf)
            o.cmd("reset run")
            wait_bench(o, sym["bench"])
            for name, how in [("software reset (NVIC_SystemReset)", 1), ("IWDG timeout", 2),
                              ("debugger reset (openocd 'reset run')", "reset")]:
                o.write32(sym["bench"], [0])  # clear magic so we see the new boot
                if how == "reset":
                    o.cmd("reset run")
                else:
                    o.write32(sym["bench_cmd"], [how])
                time.sleep(0.5)
                rows.append((name, wait_bench(o, sym["bench"])))
    finally:
        o.close()
    flags = ["BOR", "PIN", "POR", "SFT", "IWDG", "WWDG", "LPWR"]
    result = []
    for name, r in rows:
        rr = [f for i, f in enumerate(flags) if r["rcc_csr"] >> (25 + i) & 1]
        entry = {"reset": name, "rcc_csr_flags": rr, "bkpsram_kept": bool(r["bkpsram_kept"]),
                 "noinit_ram_kept": bool(r["noinit_kept"]), "boot_count": r["boot_count"],
                 "debugger_attached": bool(r["debugger_attached"]),
                 "cyccnt_counts": bool(r["cyccnt_counts"])}
        result.append(entry)
        print(f"{name:45s} flags={','.join(rr):12s} bkpsram={entry['bkpsram_kept']!s:5} "
              f"noinit={entry['noinit_ram_kept']!s:5} dbg={entry['debugger_attached']!s:5} "
              f"cyccnt={entry['cyccnt_counts']}")
    out = ROOT / "metrics" / ("board_retention_powercycle.json" if a.after_power_cycle
                              else "board_retention.json")
    out.write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {out}")


def deploy(o: OpenOCD, images: list[str]) -> None:
    """Factory programming: bootloader, provisioning block, erased journal,
    then the given signed images at their header's load address."""
    prov = ROOT / "build" / "prov.bin"
    subprocess.run([sys.executable, str(ROOT / "tools" / "sbtool.py"), "provision", "--key",
                    str(ROOT / "keys" / "dev_ed25519.pem"), "--out", str(prov)], check=True,
                   stdout=subprocess.DEVNULL)
    o.cmd("reset halt")
    o.write32(RTT_ADDR, [0, 0, 0, 0])   # drop the previous firmware's RTT block
    # S2-S3 journal, S4 provisioning, S5-S11 slots and recovery.
    o.cmd("flash erase_sector 0 2 11")
    o.program(BUILD / "bootloader.elf")
    o.program(prov, sb.PROV_ADDR)
    for img in images:
        p = Path(img) if "/" in img else BUILD / f"{img}.sbimg"
        hdr = sb.Header.parse(p.read_bytes())
        o.program(p, hdr.load_addr)


def read_journal(o: OpenOCD) -> sj.Journal:
    pages = []
    for addr in sj.PAGE_ADDR:
        words = o.read32(addr, sj.PAGE_SIZE // 4)
        pages.append(b"".join(w.to_bytes(4, "little") for w in words))
    return sj.scan((pages[0], pages[1]))


def write_flash_noerase(o: OpenOCD, addr: int, data: bytes) -> None:
    tmp = ROOT / "build" / "stage_tmp.bin"
    tmp.write_bytes(data)
    out = o.cmd(f"flash write_image {{{tmp}}} 0x{addr:08X} bin")
    if "rror" in out:
        raise RuntimeError(out)


def stage_update(o: OpenOCD, image: Path, tamper: bool = False) -> sj.Record:
    """SWD stand-in for a UART update: program the image into the inactive
    slot and append the PENDING record VERIFY(activate) would write. The
    bootloader still verifies the image before its trial boot."""
    raw = bytearray(image.read_bytes())
    hdr = sb.Header.parse(bytes(raw))
    slot = 0 if hdr.load_addr == sb.REGIONS["A"][0] else 1
    if tamper:
        raw[sb.HDR_SIZE + 1000] ^= 0x01
    o.cmd("reset halt")
    j = read_journal(o)
    if j.cur is None or j.cur.active == slot:
        raise SystemExit(f"cannot stage into slot {'AB'[slot]}: journal {j.cur and j.cur.describe()}")
    if j.next_off + sj.REC_SIZE > sj.PAGE_SIZE:
        raise SystemExit("journal page full; let the bootloader compact first")
    o.program(_tmp_copy(bytes(raw)), hdr.load_addr)
    r = sj.Record.unpack(j.cur.pack())
    r.seq = j.cur.seq + 1
    r.set_state(slot, sj.ST["PENDING"])
    r.set_version(slot, hdr.image_version)
    r.candidate, r.attempts, r.flags = slot, 0, 0
    write_flash_noerase(o, sj.PAGE_ADDR[j.page] + j.next_off, r.sealed())
    return r


def _tmp_copy(data: bytes) -> Path:
    tmp = ROOT / "build" / "stage_image.bin"
    tmp.write_bytes(data)
    return tmp


def boot_and_log(o: OpenOCD, until: str, secs: float = 10) -> list[tuple[float, str]]:
    o.cmd("reset run")
    return o.rtt_lines(secs, until)


def show(lines: list[tuple[float, str]]) -> None:
    t0 = lines[0][0] if lines else 0
    for t, text in lines:
        print(f"  {1000 * (t - t0):8.0f} ms  {text}")


def expect(cond: bool, what: str, results: list) -> None:
    results.append((what, bool(cond)))
    print(f"  [{'PASS' if cond else 'FAIL'}] {what}")


def scenario_confirm(o: OpenOCD, res: list, m: dict) -> None:
    deploy(o, ["app_a_v1"])
    o.rtt_start()
    show(boot_and_log(o, "boot_confirm()"))
    stage_update(o, BUILD / "app_b_v2.sbimg")
    lines = boot_and_log(o, "boot_confirm()")
    show(lines)
    text = "\n".join(t for _, t in lines)
    expect("boot slot B v2 (trial 1/3" in text, "candidate B v2 gets trial boot 1/3", res)
    expect("boot_confirm() = 0" in text, "app confirms", res)
    lines = boot_and_log(o, "boot_confirm()")
    show(lines)
    text = "\n".join(t for _, t in lines)
    expect("boot slot B v2 (confirmed)" in text, "next boot: B v2 confirmed, no trial", res)
    o.cmd("halt")
    j = read_journal(o)
    print("  journal", j.cur.describe())
    expect(j.cur.active == 1 and j.cur.min_version == 2 and j.cur.state_a == sj.ST["VALID"],
           "journal: active=B, min_version=2, A kept as VALID", res)


def scenario_rollback(o: OpenOCD, res: list, m: dict, image: str, kind: str) -> None:
    deploy(o, ["app_a_v1"])
    o.rtt_start()
    show(boot_and_log(o, "boot_confirm()"))
    stage_update(o, BUILD / f"{image}.sbimg")
    lines = boot_and_log(o, "boot slot A v1 (rollback)", secs=40)
    show(lines)
    text = "\n".join(t for _, t in lines)
    trials = [t for t, l in lines if "boot slot B v3 (trial" in l]
    back = [t for t, l in lines if "boot slot A v1 (rollback)" in l]
    expect(len(trials) == 3, f"{kind}: exactly 3 trial boots of B v3", res)
    expect(bool(back), f"{kind}: automatic rollback to A v1", res)
    if kind == "crash":
        expect(text.count("previous run crashed") >= 3, "crash record printed after each trial", res)
        expect("BusFault" in text and "PRECISERR" in text and "fault_addr=0xCCCCCCC0" in text,
               "crash record decodes BusFault PRECISERR at 0xCCCCCCC0", res)
    if trials and back:
        m[f"rollback_{kind}_ms"] = round(1000 * (back[0] - trials[0]), 1)
        print(f"  rollback time (first trial boot -> previous image booting): "
              f"{m[f'rollback_{kind}_ms']} ms (host-observed)")
    o.cmd("halt")
    j = read_journal(o)
    print("  journal", j.cur.describe())
    expect(j.cur.state_b == sj.ST["ROLLBACK"] and j.cur.active == 0 and j.cur.min_version == 1,
           "journal: B=ROLLBACK, active=A, min_version unchanged", res)


def scenario_tamper(o: OpenOCD, res: list, m: dict) -> None:
    deploy(o, ["app_a_v1"])
    o.rtt_start()
    show(boot_and_log(o, "boot_confirm()"))
    stage_update(o, BUILD / "app_b_v2.sbimg", tamper=True)
    lines = boot_and_log(o, "boot_confirm()")
    show(lines)
    text = "\n".join(t for _, t in lines)
    expect("boot slot B" not in text and "boot slot A v1" in text,
           "tampered candidate never boots; A v1 boots", res)
    o.cmd("halt")
    j = read_journal(o)
    print("  journal", j.cur.describe())
    expect(j.cur.state_b == sj.ST["INVALID"], "journal: B=INVALID", res)


def scenario_crash_report(o: OpenOCD, res: list, m: dict) -> None:
    deploy(o, ["app_a_v1"])
    o.rtt_start()
    show(boot_and_log(o, "boot_confirm()"))
    for cmd, needle in [(b"b", "PRECISERR"), (b"w", "IMPRECISERR"), (b"d", "DIVBYZERO"),
                        (b"u", "UNDEFINSTR"), (b"a", "AppAssert")]:
        o.rtt_send(cmd)
        lines = o.rtt_lines(5, "boot_confirm()")
        show(lines)
        text = "\n".join(t for _, t in lines)
        expect(needle in text and "boot slot A v1 (confirmed)" in text,
               f"'{cmd.decode()}' -> crash record shows {needle}; confirmed app boots again (no rollback)", res)


def scenario_no_image(o: OpenOCD, res: list, m: dict) -> None:
    deploy(o, [])
    o.rtt_start()
    lines = boot_and_log(o, "no bootable image", secs=5)
    show(lines)
    text = "\n".join(t for _, t in lines)
    expect("no bootable image" in text, "empty device stays in bootloader recovery mode", res)


SCENARIOS = {
    "confirm": scenario_confirm,
    "rollback-crash": lambda o, r, m: scenario_rollback(o, r, m, "app_b_v3_crash", "crash"),
    "rollback-hang": lambda o, r, m: scenario_rollback(o, r, m, "app_b_v3_hang", "hang"),
    "tamper": scenario_tamper,
    "crash-report": scenario_crash_report,
    "no-image": scenario_no_image,
}


def cmd_scenario(a) -> None:
    names = list(SCENARIOS) if a.name == "all" else [a.name]
    results, metrics = [], {}
    for n in names:
        print(f"== {n}")
        o = OpenOCD()
        try:
            SCENARIOS[n](o, results, metrics)
        finally:
            o.close()
    passed = sum(ok for _, ok in results)
    print(f"\n{passed}/{len(results)} checks passed")
    out = ROOT / "metrics" / "board_scenarios.json"
    prev = json.loads(out.read_text()) if out.exists() else {}
    prev.update({n: {"checks": [(w, ok) for w, ok in results]} for n in names} if len(names) == 1
                else {"all": {"checks": results}})
    prev.setdefault("metrics", {}).update(metrics)
    out.write_text(json.dumps(prev, indent=2) + "\n")
    sys.exit(0 if passed == len(results) else 1)


def cmd_deploy(a) -> None:
    o = OpenOCD()
    try:
        deploy(o, a.images)
        o.cmd("reset run")
        o.rtt_start()
        print(o.rtt_read(a.secs))
    finally:
        o.close()


def cmd_log(a) -> None:
    o = OpenOCD()
    try:
        o.rtt_start()
        print(o.rtt_read(a.secs))
    finally:
        o.close()


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("bench").set_defaults(fn=cmd_bench)
    r = sub.add_parser("retention")
    r.add_argument("--after-power-cycle", action="store_true",
                   help="only read results: run after unplugging/replugging USB with bench flashed")
    r.set_defaults(fn=cmd_retention)
    d = sub.add_parser("deploy")
    d.add_argument("images", nargs="*")
    d.add_argument("--secs", type=float, default=3)
    d.set_defaults(fn=cmd_deploy)
    sc = sub.add_parser("run-scenario")
    sc.add_argument("name", choices=sorted(SCENARIOS) + ["all"])
    sc.set_defaults(fn=cmd_scenario)
    lg = sub.add_parser("log")
    lg.add_argument("--secs", type=float, default=3)
    lg.set_defaults(fn=cmd_log)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
