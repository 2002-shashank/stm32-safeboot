#!/usr/bin/env python3
"""UART update client for the safeboot bootloader.

  sbflash.py update IMAGE [IMAGE...] [--port P] [--baud 921600] [--no-activate]
  sbflash.py query [--port P]
  sbflash.py reboot [--port P]

Pass the builds for both slots (e.g. app_a_v3.sbimg app_b_v3.sbimg): the
device reports which slot is inactive and the matching one is sent.

Resumable: the session id is derived from the image header, so re-running
the same update after a dropped link or a device reset continues from the
device's last checkpoint. Stop-and-wait with retries; the device treats
duplicates (same seq) and re-sent offsets idempotently.

Test options inject faults into the link: --drop/--corrupt/--dup/--lose-ack
(probabilities) and --abort-after N (exit after N payload bytes, as if the
cable were pulled).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import random
import struct
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import os

import serial

DEBUG = os.environ.get("SBFLASH_DEBUG")

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sbimage as sb  # noqa: E402

CMD = {"BEGIN": 1, "INFO": 2, "DATA": 3, "QUERY": 4, "FINALIZE": 5, "VERIFY": 6, "REBOOT": 7,
       "SET_BAUD": 8}
STATUS = {0: "OK", 1: "ERR_SEQ", 2: "ERR_CRC", 3: "ERR_STATE", 4: "ERR_HDR", 5: "ERR_VERSION",
          6: "ERR_RANGE", 7: "ERR_FLASH", 8: "ERR_VERIFY", 9: "RESTART", 10: "ERR_LEN", 11: "ERR_CMD"}
STATES = {0: "-", 1: "EMPTY", 2: "VALID", 3: "PENDING", 4: "TRIAL", 5: "CONFIRMED", 6: "INVALID",
          7: "ROLLBACK"}
DATA_MAX = 1024


class LinkError(Exception):
    pass


@dataclass
class Resp:
    status: int
    slot: int
    slot_state: int
    next_offset: int
    a: int
    b: int

    @property
    def name(self) -> str:
        return STATUS.get(self.status, str(self.status))


@dataclass
class Stats:
    frames: int = 0
    retries: int = 0
    timeouts: int = 0
    resyncs: int = 0
    nacks: dict = field(default_factory=dict)
    injected: dict = field(default_factory=dict)


class Link:
    def __init__(self, port: str, baud: int, faults: dict, seed: int) -> None:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, baud, 0.05
        s.dtr = s.rts = False   # keep auto-reset lines of dev-board bridges idle
        s.open()
        self.s = s
        self.seq = random.randrange(0x10000)
        self.faults = faults
        self.rng = random.Random(seed)
        self.stats = Stats()
        self.target_baud = baud   # rate negotiated with SET_BAUD
        self.in_resync = False
        self.last_ok = time.time()  # last valid reply

    def _frame(self, typ: int, seq: int, payload: bytes) -> bytes:
        hdr = struct.pack("<BHH", typ, seq, len(payload))
        return b"\xa5\x5a" + hdr + payload + struct.pack("<I", zlib.crc32(hdr + payload))

    def _inject(self, kind: str) -> bool:
        p = self.faults.get(kind, 0.0)
        hit = p > 0 and self.rng.random() < p
        if hit:
            self.stats.injected[kind] = self.stats.injected.get(kind, 0) + 1
        return hit

    def _read_resp(self, typ: int, seq: int, timeout: float) -> Resp | None:
        buf, t0 = b"", time.time()
        while time.time() - t0 < timeout:
            # Read what has arrived (at least 1 byte): a fixed-size read would
            # wait out the port timeout for every 27-byte reply.
            buf += self.s.read(max(1, self.s.in_waiting))
            while True:
                i = buf.find(b"\xa5\x5a")
                if i < 0 or len(buf) - i < 7:
                    break
                rtyp, rseq, ln = struct.unpack_from("<BHH", buf, i + 2)
                if ln > 64:
                    buf = buf[i + 2:]
                    continue
                if len(buf) - i < 11 + ln:
                    break
                body = buf[i + 2:i + 7 + ln]
                crc = struct.unpack_from("<I", buf, i + 7 + ln)[0]
                buf = buf[i + 11 + ln:]
                if crc != zlib.crc32(body) or rtyp != (typ | 0x80) or rseq != seq or ln != 16:
                    continue   # stale or corrupt reply: keep waiting
                return Resp(*struct.unpack("<BBBxIII", body[5:]))
        return None

    def call(self, name: str, payload: bytes = b"", timeout: float = 2.5, retries: int = 10) -> Resp:
        typ = CMD[name]
        self.seq = (self.seq + 1) & 0xFFFF
        seq = self.seq   # local: a nested resync call must not change it
        frame = self._frame(typ, seq, payload)
        for attempt in range(retries + 1):
            self.stats.frames += 1
            if attempt:
                self.stats.retries += 1
            out = frame
            if self._inject("drop"):
                out = b""
            elif self._inject("corrupt"):
                b = bytearray(frame)
                b[self.rng.randrange(7, len(b))] ^= 1 << self.rng.randrange(8)
                out = bytes(b)
            self.s.write(out)
            dup = bool(out) and self._inject("dup")
            if dup:
                self.s.write(out)
            r = self._read_resp(typ, seq, timeout)
            lost = r is not None and self._inject("lose_ack")
            if lost:
                r = None   # pretend the reply was lost: same seq is resent
            if DEBUG:
                sent = "dropped" if not out else "corrupt" if out != frame else "sent"
                print(f"{time.time():.3f} {name} seq={seq} try={attempt} {sent}{' +dup' if dup else ''}"
                      f" @{self.s.baudrate} -> {('ack-lost' if lost else 'timeout') if r is None else r.name + ' next=' + str(r.next_offset)}",
                      file=sys.stderr)
            if r is None:
                self.stats.timeouts += 1
                # The bootloader falls back to 115200 only after 3 s without
                # receiving anything, so resync only after that much silence.
                if (self.target_baud != 115200 and not self.in_resync
                        and time.time() - self.last_ok > 3.2):
                    self._resync()
                continue
            self.last_ok = time.time()
            if r.status == 2:   # ERR_CRC (our frame was corrupted in flight)
                self.stats.nacks["ERR_CRC"] = self.stats.nacks.get("ERR_CRC", 0) + 1
                self.seq = (self.seq + 1) & 0xFFFF   # new seq: it is a new request
                seq = self.seq
                frame = self._frame(typ, seq, payload)
                continue
            return r
        raise LinkError(f"{name}: no valid reply after {retries + 1} attempts")

    def _resync(self) -> None:
        """The bootloader returns to 115200 after 3 s without traffic (e.g.
        several frames lost in a row): find it there and renegotiate."""
        self.in_resync = True
        try:
            self.s.baudrate = 115200
            self.s.reset_input_buffer()
            r = self.call("QUERY", timeout=0.3, retries=1)
            self.stats.resyncs += 1
            r = self.call("SET_BAUD", struct.pack("<I", self.target_baud), timeout=0.5, retries=1)
            if r.status == 0:
                time.sleep(0.02)
                self.s.baudrate = self.target_baud
                time.sleep(0.02)
                self.s.reset_input_buffer()
        except LinkError:
            self.s.baudrate = self.target_baud   # not at 115200 either: keep trying as before
        finally:
            self.in_resync = False

    def close(self) -> None:
        self.s.close()


def enter_update_mode(link: Link, wait: float = 6.0, also_baud: int | None = None) -> Resp:
    """Ask a running app (it listens for SBUPDATE) to reboot into the
    bootloader, then poll with QUERY until the bootloader answers. A
    bootloader left at a higher rate by an interrupted session is found by
    also probing `also_baud` (it falls back to 115200 after 3 s idle)."""
    t0 = time.time()
    while time.time() - t0 < wait:
        for baud in [115200] + ([also_baud] if also_baud and also_baud != 115200 else []):
            link.s.baudrate = baud
            try:
                return link.call("QUERY", timeout=0.3, retries=0)
            except LinkError:
                pass
        link.s.baudrate = 115200
        link.s.write(b"SBUPDATE")
        time.sleep(0.4)
        link.s.reset_input_buffer()
    raise LinkError("device did not enter update mode (is the app or bootloader running?)")


def set_baud(link: Link, baud: int) -> int:
    if link.s.baudrate == baud:
        return baud
    r = link.call("SET_BAUD", struct.pack("<I", baud), timeout=0.5)
    if r.status != 0:
        raise LinkError(f"SET_BAUD {baud}: {r.name}")
    link.target_baud = baud
    time.sleep(0.02)
    link.s.baudrate = baud
    time.sleep(0.02)
    link.s.reset_input_buffer()
    return r.a


def pick_image(images: list[Path], slot: int) -> tuple[Path, bytes, sb.Header]:
    want = sb.REGIONS["AB"[slot]][0]
    for p in images:
        raw = p.read_bytes()
        h = sb.Header.parse(raw)
        if h.load_addr == want:
            return p, raw, h
    raise SystemExit(f"the device will write slot {'AB'[slot]} (0x{want:08X}); none of the given "
                     f"images is built for it")


def do_update(a) -> dict:
    faults = {k: getattr(a, k) for k in ("drop", "corrupt", "dup", "lose_ack")}
    link = Link(a.port, 115200, faults, a.seed)
    try:
        q = enter_update_mode(link, also_baud=a.baud)
        link.stats = Stats()   # count only the transfer, not the wake-up polls
        actual = set_baud(link, a.baud)
        q = link.call("QUERY")
        path, raw, h = pick_image([Path(p) for p in a.images], q.slot)
        payload = raw[sb.HDR_SIZE:sb.HDR_SIZE + h.image_size]
        session = struct.unpack("<I", hashlib.sha256(raw[:sb.HDR_SIZE]).digest()[:4])[0]
        print(f"device: target slot {'AB'[q.slot]} ({STATES.get(q.slot_state)} v{q.a}), "
              f"{actual} baud; sending {path.name} v{h.image_version} {h.image_size} B")
        r = link.call("BEGIN", struct.pack("<I", session))
        if r.status:
            raise SystemExit(f"BEGIN_UPDATE: {r.name}")
        r = link.call("INFO", raw[:sb.HDR_SIZE], timeout=3.0)
        if r.status:
            raise SystemExit(f"IMAGE_INFO: {r.name}" + (" (anti-rollback: version not newer "
                             "than the device's floor)" if r.status == 5 else ""))
        nxt = r.next_offset
        resumed_from = nxt
        if nxt:
            print(f"resuming at offset {nxt} ({nxt * 100 // h.image_size}%)")
        t0, sent, last_print = time.time(), 0, 0.0
        while nxt < h.image_size:
            n = min(DATA_MAX, h.image_size - nxt)
            chunk = payload[nxt:nxt + n]
            r = link.call("DATA", struct.pack("<II", nxt, zlib.crc32(chunk)) + chunk)
            sent += n
            if r.status == 9:   # RESTART: resend the header, start over
                link.stats.nacks["RESTART"] = link.stats.nacks.get("RESTART", 0) + 1
                r = link.call("INFO", raw[:sb.HDR_SIZE], timeout=3.0)
            elif r.status not in (0, 1):
                raise SystemExit(f"DATA @{nxt}: {r.name}")
            elif r.status == 1:
                link.stats.nacks["ERR_SEQ"] = link.stats.nacks.get("ERR_SEQ", 0) + 1
            nxt = r.next_offset
            if a.abort_after and sent >= a.abort_after:
                print(f"--abort-after: stopping at offset {nxt} (simulated link loss)")
                return {"aborted_at": nxt}
            if time.time() - last_print > 0.5:
                last_print = time.time()
                rate = sent / max(time.time() - t0, 1e-6) / 1024
                print(f"\r  {nxt * 100 // h.image_size:3d}%  {nxt:7d}/{h.image_size} B  {rate:6.1f} KB/s",
                      end="", flush=True)
        t_data = time.time() - t0
        print(f"\r  100%  {h.image_size}/{h.image_size} B  in {t_data:.1f} s".ljust(60))
        r = link.call("FINALIZE", struct.pack("<I", h.image_size))
        if r.status:
            raise SystemExit(f"FINALIZE: {r.name}")
        tv = time.time()
        r = link.call("VERIFY", bytes([0 if a.no_activate else 1]), timeout=5.0)
        t_verify = time.time() - tv
        if r.status == 3:
            # ERR_STATE: VERIFY already ran (its reply was lost and the link
            # resynced, which replaced the cached reply). Ask for the outcome.
            q = link.call("QUERY")
            want = 2 if a.no_activate else 3   # VALID / PENDING
            if q.slot_state == want and q.a == h.image_version:
                r = Resp(0, q.slot, q.slot_state, h.image_size, 0, h.image_version)
        if r.status:
            raise SystemExit(f"VERIFY failed: {r.name} (verify result {r.a - (1 << 32) if r.a >> 31 else r.a})")
        print(f"verified: slot {'AB'[r.slot]} is {STATES.get(r.slot_state)} (v{r.b}); "
              f"verify took {t_verify * 1000:.0f} ms")
        if not a.no_reboot:
            link.call("REBOOT", timeout=0.5, retries=2)
            print("rebooting into the new image" + (" (trial boot)" if not a.no_activate else ""))
        st = link.stats
        res = {"image": path.name, "version": h.image_version, "bytes": h.image_size,
               "baud": a.baud, "actual_baud": actual, "resumed_from": resumed_from,
               "data_seconds": round(t_data, 2),
               "goodput_KBps": round((h.image_size - resumed_from) / t_data / 1024, 1),
               "verify_ms": round(t_verify * 1000), "frames": st.frames, "retries": st.retries,
               "timeouts": st.timeouts, "resyncs": st.resyncs, "nacks": st.nacks, "injected": st.injected,
               "slot_state": STATES.get(r.slot_state)}
        print(f"goodput {res['goodput_KBps']} KB/s, {st.frames} frames, {st.retries} retries, "
              f"{st.timeouts} timeouts, {st.resyncs} resyncs, nacks {st.nacks}, injected {st.injected}")
        return res
    finally:
        link.close()


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("update", "query", "reboot"):
        c = sub.add_parser(name)
        c.add_argument("--port", default="/dev/cu.usbserial-0001")
        if name == "update":
            c.add_argument("images", nargs="+")
            c.add_argument("--baud", type=int, default=921600)
            c.add_argument("--no-activate", action="store_true", help="install as VALID, do not trial-boot")
            c.add_argument("--no-reboot", action="store_true")
            for f in ("drop", "corrupt", "dup", "lose_ack"):
                c.add_argument(f"--{f.replace('_', '-')}", dest=f, type=float, default=0.0)
            c.add_argument("--abort-after", type=int, default=0)
            c.add_argument("--seed", type=int, default=1)
            c.add_argument("--json", help="write the result summary here")
    a = p.parse_args()
    if a.cmd == "update":
        res = do_update(a)
        if a.json:
            Path(a.json).write_text(json.dumps(res, indent=2) + "\n")
        return
    link = Link(a.port, 115200, {}, 0)
    try:
        q = enter_update_mode(link)
        if a.cmd == "query":
            print(f"bootloader: target slot {'AB'[q.slot]} state {STATES.get(q.slot_state)} "
                  f"v{q.a}, rx session 0x{q.b:08X}, next_offset {q.next_offset}")
        else:
            link.call("REBOOT", timeout=0.5, retries=2)
            print("rebooting")
    finally:
        link.close()


if __name__ == "__main__":
    main()
