"""Host-side mirror of the boot journal (core/src/journal.c): parse a dump of
the two journal sectors, find the current record, and build new records.

Used to stage updates over SWD while no UART link exists, and to check the
on-board fault tests. Field layout must match sb_record_t.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, fields

REC_SIZE = 64
PAGE_SIZE = 0x4000
PAGE_ADDR = (0x08008000, 0x0800C000)
REC_MAGIC = 0x4A425321
REC_COMMIT = 0x5AFEC0DE
SLOT_NONE = 0xFF
STATES = {1: "EMPTY", 2: "VALID", 3: "PENDING", 4: "TRIAL", 5: "CONFIRMED", 6: "INVALID",
          7: "ROLLBACK"}
ST = {v: k for k, v in STATES.items()}
RF_RX = 0x01

_FMT = "<II4BIIIBBBBII5III"
assert struct.calcsize(_FMT) == REC_SIZE


@dataclass
class Record:
    magic: int = REC_MAGIC
    seq: int = 0
    state_a: int = ST["EMPTY"]
    state_b: int = ST["EMPTY"]
    active: int = SLOT_NONE
    candidate: int = SLOT_NONE
    version_a: int = 0
    version_b: int = 0
    min_version: int = 0
    attempts: int = 0
    max_attempts: int = 3
    rx_slot: int = SLOT_NONE
    flags: int = 0
    rx_session: int = 0
    rx_offset: int = 0
    r0: int = 0xFFFFFFFF
    r1: int = 0xFFFFFFFF
    r2: int = 0xFFFFFFFF
    r3: int = 0xFFFFFFFF
    r4: int = 0xFFFFFFFF
    crc: int = 0
    commit: int = REC_COMMIT

    @classmethod
    def unpack(cls, raw: bytes) -> "Record":
        return cls(*struct.unpack(_FMT, raw))

    def pack(self) -> bytes:
        return struct.pack(_FMT, *(getattr(self, f.name) for f in fields(self)))

    def sealed(self) -> bytes:
        body = self.pack()[:56]
        self.crc = zlib.crc32(body)
        self.commit = REC_COMMIT
        return self.pack()

    def valid(self) -> bool:
        return (self.commit == REC_COMMIT and self.magic == REC_MAGIC
                and self.crc == zlib.crc32(self.pack()[:56]))

    def state(self, slot: int) -> int:
        return self.state_b if slot else self.state_a

    def set_state(self, slot: int, s: int) -> None:
        if slot:
            self.state_b = s
        else:
            self.state_a = s

    def set_version(self, slot: int, v: int) -> None:
        if slot:
            self.version_b = v
        else:
            self.version_a = v

    def logical(self) -> tuple:
        """Everything except seq/crc/commit/magic (as sb_record_same_state)."""
        return tuple(getattr(self, f.name) for f in fields(self))[2:-2]

    def describe(self) -> str:
        def slot(v: int) -> str:
            return "-" if v == SLOT_NONE else "AB"[v] if v < 2 else f"?{v}"
        a, c = slot(self.active), slot(self.candidate)
        return (f"seq {self.seq}: A={STATES.get(self.state_a, '?')} v{self.version_a} "
                f"B={STATES.get(self.state_b, '?')} v{self.version_b} active={a} cand={c} "
                f"att={self.attempts}/{self.max_attempts} min_ver={self.min_version}")


@dataclass
class Journal:
    cur: Record | None
    page: int
    next_off: int


def scan(pages: tuple[bytes, bytes]) -> Journal:
    cur, cur_page = None, 0
    for p, data in enumerate(pages):
        for off in range(0, PAGE_SIZE, REC_SIZE):
            r = Record.unpack(data[off:off + REC_SIZE])
            if r.valid() and (cur is None or r.seq > cur.seq):
                cur, cur_page = r, p
    data = pages[cur_page]
    end = 0
    for off in range(PAGE_SIZE, 0, -REC_SIZE):
        if data[off - REC_SIZE:off] != b"\xff" * REC_SIZE:
            end = off
            break
    return Journal(cur, cur_page, end)
