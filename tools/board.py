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
        self.rtt_part = b""

    def rtt_lines(self, secs: float, until: str | None = None) -> list[tuple[float, str]]:
        """Collect RTT output as (host time, line) until `until` appears in a
        line or secs elapse. Host timestamps are within the RTT polling
        interval (1 ms requested) plus USB latency."""
        # The partial-line buffer persists across calls so a line split
        # between two reads is never broken in two.
        lines, t0 = [], time.time()
        while time.time() - t0 < secs:
            try:
                chunk = self.rtt.recv(4096)
            except socket.timeout:
                continue
            now = time.time()
            self.rtt_part += chunk
            while b"\n" in self.rtt_part:
                line, self.rtt_part = self.rtt_part.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip("\r")
                lines.append((now, text))
                if until and until in text:
                    return lines
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


def deploy(o: OpenOCD, images: list[str], bootloader: str = "bootloader") -> None:
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
    o.program(BUILD / f"{bootloader}.elf")
    o.program(prov, sb.PROV_ADDR)
    for img in images:
        p = Path(img) if "/" in img else BUILD / f"{img}.sbimg"
        hdr = sb.Header.parse(p.read_bytes())
        o.program(p, hdr.load_addr)


def dump(o: OpenOCD, addr: int, size: int) -> bytes:
    """Binary memory read via dump_image: fast, and unlike read_memory it
    does not echo every word into the OpenOCD log."""
    tmp = ROOT / "build" / "dump_tmp.bin"
    o.cmd(f"dump_image {{{tmp}}} 0x{addr:08X} {size}")
    data = tmp.read_bytes()
    if len(data) != size:
        raise RuntimeError(f"dump of 0x{addr:08X} returned {len(data)} of {size} bytes")
    return data


def read_journal(o: OpenOCD) -> sj.Journal:
    return sj.scan(journal_pages(o))


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


# ---------------- on-silicon fault injection ----------------
FI_ADDR, FI_MAGIC = 0x40024C00, 0x494A4E46
FI_BEFORE, FI_MID, FI_AFTER = 0, 1, 2
US = 168   # cycles per microsecond at 168 MHz
PROG_DELAYS = [3 * US, 8 * US, 13 * US]                   # word program ~14 us
ERASE_DELAYS = [1000 * US, 50000 * US, 100000 * US, 180000 * US]   # 16 KB erase ~195 ms


def bkpsram_unlock(o: OpenOCD) -> None:
    """After a reset the backup domain is write-protected and unclocked."""
    for addr, bit in [(0x40023840, 1 << 28), (0x40007000, 1 << 8), (0x40023830, 1 << 18)]:
        o.write32(addr, [o.read32(addr)[0] | bit])


def fi_arm(o: OpenOCD, k: int, mode: int = 0, delay: int = 0, magic: int = FI_MAGIC) -> None:
    bkpsram_unlock(o)
    # Clear the whole block, evidence fields included: stale post_* words from
    # an earlier session would otherwise look like this run's result.
    o.write32(FI_ADDR, [magic, k, mode, delay] + [0] * (FI_WORDS - 4))


def journal_pages(o: OpenOCD) -> tuple[bytes, bytes]:
    both = dump(o, sj.PAGE_ADDR[0], 2 * sj.PAGE_SIZE)   # S2 and S3 are contiguous
    return both[:sj.PAGE_SIZE], both[sj.PAGE_SIZE:]


def restore_journal(o: OpenOCD, pages: tuple[bytes, bytes]) -> None:
    o.cmd("reset halt")
    o.cmd("flash erase_sector 0 2 3")
    for addr, data in zip(sj.PAGE_ADDR, pages):
        end = len(data.rstrip(b"\xff"))
        end = (end + 63) // 64 * 64
        if end:
            write_flash_noerase(o, addr, data[:end])


def records_after(pages: tuple[bytes, bytes], seq: int) -> list[tuple[int, int, sj.Record]]:
    """Valid records with seq > `seq` as (seq, page, record), in seq order."""
    out = []
    for p, data in enumerate(pages):
        for off in range(0, sj.PAGE_SIZE, sj.REC_SIZE):
            r = sj.Record.unpack(data[off:off + sj.REC_SIZE])
            if r.valid() and r.seq > seq:
                out.append((r.seq, p, r))
    return sorted(out, key=lambda t: t[0])


def fi_setup(o: OpenOCD, image: str, compact_at: int | None) -> tuple[tuple[bytes, bytes], sj.Record]:
    deploy(o, ["app_a_v1"], bootloader="bootloader_fi")
    fi_arm(o, 0, magic=0)
    o.cmd("reset run")
    adopted = lambda r: r is not None and r.active == 0 and r.state_a == sj.ST["CONFIRMED"]  # noqa: E731
    if not adopted(wait_journal(o, adopted, 10, need_fired=False)[0]):
        raise SystemExit("setup: factory image was not adopted")
    stage_update(o, BUILD / f"{image}.sbimg")
    if compact_at:
        # Fill the page so the compact_at-th append of the scenario compacts.
        j = read_journal(o)
        free = (sj.PAGE_SIZE - j.next_off) // sj.REC_SIZE
        fill, blob = free - (compact_at - 1), b""
        r = sj.Record.unpack(j.cur.pack())
        for _ in range(fill):
            r.seq += 1
            blob += r.sealed()
        if blob:
            write_flash_noerase(o, sj.PAGE_ADDR[j.page] + j.next_off, blob)
    o.cmd("reset halt")
    pages = journal_pages(o)
    return pages, sj.scan(pages).cur


def op_timeline(start: sj.Record, start_page: int, recs: list) -> tuple[list[int], list[str]]:
    """Op index of each record's commit word, and the op kinds, assuming each
    append is [erase if the page changed] + 15 body words + 1 commit word."""
    commits, kinds, idx, page = [], [], 0, start_page
    for _, p, _ in recs:
        if p != page:
            kinds.append("E")
            idx += 1
            page = p
        kinds += ["P"] * 16
        idx += 16
        commits.append(idx - 1)
    return commits, kinds


FI_WORDS = 26   # sb_fi_t: 9 control words + 16-word record + recovery


def fi_state(o: OpenOCD) -> dict:
    w = o.read32(FI_ADDR, FI_WORDS)
    return {"count": w[4], "fired": w[5], "fired_addr": w[6], "post_valid": w[7],
            "post_have": w[8], "post_rec": b"".join(x.to_bytes(4, "little") for x in w[9:25]),
            "recovery": w[25]}


def wait_journal(o: OpenOCD, final_ok, secs: float, need_fired: bool) -> tuple[sj.Record | None, dict]:
    """Poll (target running) until the journal reaches the goal state, and,
    for injected runs, the bootloader has recorded its post-reset scan."""
    t0, cur, st = time.time(), None, {}
    while time.time() - t0 < secs:
        time.sleep(0.25)
        st = fi_state(o)
        if need_fired and not (st["fired"] and st["post_valid"]):
            continue
        cur = read_journal(o).cur
        if final_ok(cur):
            return cur, st
    return cur, st


def same(a: sj.Record, b: sj.Record) -> bool:
    return a.seq == b.seq and a.logical() == b.logical()


def fi_campaign(o: OpenOCD, name: str, image: str, final_ok, compact_at: int | None,
                mids: bool, res: list, progress) -> dict:
    pages, start = fi_setup(o, image, compact_at)
    start_page = sj.scan(pages).page
    # Reference run: count ops, learn the committed record sequence.
    restore_journal(o, pages)
    fi_arm(o, 0xFFFFFFFF)
    o.cmd("reset run")
    cur, _ = wait_journal(o, final_ok, 40, need_fired=False)
    time.sleep(0.5)
    n_ops = fi_state(o)["count"]
    final_pages = journal_pages(o)
    recs = records_after(final_pages, start.seq)
    commits, kinds = op_timeline(start, start_page, recs)
    states = [start] + [r for _, _, r in recs]
    ok_ref = final_ok(cur) and len(kinds) == n_ops
    print(f"  reference: {n_ops} flash ops, {len(recs)} records, timeline "
          f"{'matches' if len(kinds) == n_ops else f'MISMATCH ({len(kinds)})'}", flush=True)
    progress.write(f"{name}: reference {n_ops} ops, {len(recs)} records, ok={ok_ref}\n")
    progress.flush()
    if not ok_ref:
        res.append((f"{name}: reference run", False))
        return {}

    runs = passed = 0
    fails: list[str] = []
    t_start = time.time()
    total = sum(2 + (len(ERASE_DELAYS if kd == "E" else PROG_DELAYS) if mids else 0) for kd in kinds)
    for k in range(n_ops):
        variants = [(FI_BEFORE, 0), (FI_AFTER, 0)]
        if mids:
            variants += [(FI_MID, d) for d in (ERASE_DELAYS if kinds[k] == "E" else PROG_DELAYS)]
        before = states[sum(c < k for c in commits)]
        after = states[sum(c <= k for c in commits)]
        for mode, delay in variants:
            runs += 1
            t0 = time.time()
            restore_journal(o, pages)
            fi_arm(o, k, mode, delay)
            o.cmd("reset run")
            cur, st = wait_journal(o, final_ok, 20, need_fired=True)
            err = None
            if not st.get("fired"):
                err = "injection did not fire"
            elif not st.get("post_valid"):
                err = "no boot recorded after the injected reset"
            elif not st["post_have"]:
                err = "post-reset scan found no valid record"
            else:
                post = sj.Record.unpack(st["post_rec"])
                if not (same(post, before) or same(post, after)):
                    err = f"post-reset state [{post.describe()}] is neither before nor after"
            if not err and st["recovery"]:
                err = "fell into recovery mode"
            if not err and not final_ok(cur):
                err = f"did not converge: {cur.describe() if cur else None}"
            if err:
                fails.append(f"op {k} ({kinds[k]}) mode {mode} delay {delay}: {err}")
            else:
                passed += 1
            eta = (time.time() - t_start) / runs * (total - runs)
            progress.write(f"{time.strftime('%H:%M:%S')} {name} run {runs}/{total} op {k} "
                           f"{kinds[k]} mode {mode} delay {delay}: {'ok' if not err else 'FAIL ' + err} "
                           f"({time.time() - t0:.1f} s, {passed}/{runs} passed, eta {eta / 60:.0f} min)\n")
            progress.flush()
    for f in fails[:10]:
        print("   FAIL", f)
    res.append((f"{name}: {passed}/{runs} injected resets passed", passed == runs))
    return {"scenario": name, "flash_ops": n_ops, "runs": runs, "passed": passed,
            "modes": "before/after" + ("/mid-op" if mids else ""), "failures": fails}


def cmd_hw_faults(a) -> None:
    res: list = []
    reports = []
    confirmed_b = lambda r: (r is not None and r.active == 1 and r.state_b == sj.ST["CONFIRMED"]  # noqa: E731
                             and r.min_version == 2 and r.attempts == 0)
    rolled_back = lambda r: (r is not None and r.active == 0 and r.state_b == sj.ST["ROLLBACK"]  # noqa: E731
                             and r.min_version == 1)
    campaigns = {
        "trial_confirm": ("app_b_v2", confirmed_b, None),
        "trial_confirm_compacting": ("app_b_v2", confirmed_b, 2),
        "crash_rollback": ("app_b_v3_crash", rolled_back, None),
    }
    prog_path = ROOT / "build" / "hw_faults_progress.log"
    print(f"progress: {prog_path}", flush=True)
    with open(prog_path, "w") as progress:
        for name in a.campaigns or campaigns:
            image, ok, compact = campaigns[name]
            print(f"== {name}", flush=True)
            o = OpenOCD()
            try:
                o.rtt_start()
                reports.append(fi_campaign(o, name, image, ok, compact, not a.no_mid, res, progress))
            finally:
                o.close()
    out = ROOT / "metrics" / "board_fault_injection.json"
    out.write_text(json.dumps(reports, indent=2) + "\n")
    for w, ok in res:
        print(f"[{'PASS' if ok else 'FAIL'}] {w}")
    print(f"wrote {out}")
    sys.exit(0 if all(ok for _, ok in res) else 1)


def cmd_scenario(a) -> None:
    names = list(SCENARIOS) if a.name == "all" else [a.name]
    results, metrics = [], {}
    for n in names:
        print(f"== {n}")
        o = OpenOCD()
        mine: list = []
        try:
            SCENARIOS[n](o, mine, metrics)
        finally:
            o.close()
            results += [(n, w, ok) for w, ok in mine]
    passed = sum(ok for _, _, ok in results)
    print(f"\n{passed}/{len(results)} checks passed")
    # Each scenario's latest result replaces its previous one.
    out = ROOT / "metrics" / "board_scenarios.json"
    data = json.loads(out.read_text()) if out.exists() else {}
    data = {"scenarios": data.get("scenarios", {}), "metrics": data.get("metrics", {})}
    for n in names:
        data["scenarios"][n] = [(w, ok) for sc_name, w, ok in results if sc_name == n]
    data["metrics"].update(metrics)
    out.write_text(json.dumps(data, indent=2) + "\n")
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
    hf = sub.add_parser("hw-faults", help="reset at every flash op on silicon (bootloader_fi)")
    hf.add_argument("campaigns", nargs="*")
    hf.add_argument("--no-mid", action="store_true", help="only before/after, no mid-op resets")
    hf.set_defaults(fn=cmd_hw_faults)
    lg = sub.add_parser("log")
    lg.add_argument("--secs", type=float, default=3)
    lg.set_defaults(fn=cmd_log)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
