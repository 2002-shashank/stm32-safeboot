"""Image format and layout shared by the host tools.

Values are parsed from the C headers (core/include/sb/layout.h, image.h) so
the Python side cannot drift from the firmware.
"""
from __future__ import annotations

import hashlib
import re
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "core" / "include" / "sb"


def _defines(*names: str) -> dict[str, int]:
    vals: dict[str, int] = {}
    pat = re.compile(r"#define\s+(SB_\w+)\s+(.+?)\s*(?:/\*.*)?$")
    for name in names:
        for line in (INCLUDE / name).read_text().splitlines():
            m = pat.match(line)
            if not m:
                continue
            expr = re.sub(r"(0x[0-9A-Fa-f]+|\d+)u\b", r"\1", m.group(2))
            expr = re.sub(r"\bSB_\w+\b", lambda t: str(vals.get(t.group(0), t.group(0))), expr)
            try:
                vals[m.group(1)] = int(eval(expr, {"__builtins__": {}}))
            except Exception:
                pass
    return vals


D = _defines("layout.h", "image.h")
HDR_SIZE = D["SB_HDR_SIZE"]
SIG_OFF = HDR_SIZE - 64
IMG_MAGIC = D["SB_IMG_MAGIC"]
IMG_HDR_VER = D["SB_IMG_HDR_VER"]
F_RECOVERY = D["SB_IMG_F_RECOVERY"]
PROV_MAGIC = D["SB_PROV_MAGIC"]
PROV_ADDR = D["SB_PROV_ADDR"]
SRAM_BASE = D["SB_SRAM_BASE"]
APP_RAM_TOP = D["SB_SRAM_BASE"] + D["SB_APP_RAM_SIZE"]

REGIONS = {
    "A": (D["SB_SLOT_A_ADDR"], D["SB_SLOT_SIZE"]),
    "B": (D["SB_SLOT_B_ADDR"], D["SB_SLOT_SIZE"]),
    "recovery": (D["SB_RECOVERY_ADDR"], D["SB_RECOVERY_SIZE"]),
}

# magic, hdr_version, hdr_size, image_version, image_size, load_addr, flags, key_id, reserved0
_HDR_FMT = "<IHHIIIIII"


def key_id_of(pubkey: bytes) -> int:
    return struct.unpack("<I", hashlib.sha256(pubkey).digest()[:4])[0]


@dataclass
class Header:
    image_version: int
    image_size: int
    load_addr: int
    flags: int
    key_id: int
    sha256: bytes
    signature: bytes = b"\xff" * 64

    def signed_bytes(self) -> bytes:
        fixed = struct.pack(_HDR_FMT, IMG_MAGIC, IMG_HDR_VER, HDR_SIZE, self.image_version,
                            self.image_size, self.load_addr, self.flags, self.key_id, 0xFFFFFFFF)
        body = fixed + self.sha256
        return body + b"\xff" * (SIG_OFF - len(body))

    def to_bytes(self) -> bytes:
        return self.signed_bytes() + self.signature

    @classmethod
    def parse(cls, raw: bytes) -> "Header":
        if len(raw) < HDR_SIZE:
            raise ValueError("file shorter than an image header")
        f = struct.unpack_from(_HDR_FMT, raw)
        if f[0] != IMG_MAGIC or f[1] != IMG_HDR_VER or f[2] != HDR_SIZE:
            raise ValueError("not a safeboot image (bad magic/version)")
        return cls(image_version=f[3], image_size=f[4], load_addr=f[5], flags=f[6], key_id=f[7],
                   sha256=raw[32:64], signature=raw[SIG_OFF:HDR_SIZE])


def check_vectors(payload: bytes, load_addr: int) -> None:
    """Same sanity check as the bootloader, so a bad build fails at signing."""
    sp, pc = struct.unpack_from("<II", payload)
    base = load_addr + HDR_SIZE
    if not (SRAM_BASE < sp <= APP_RAM_TOP and sp % 8 == 0):
        raise ValueError(f"initial SP 0x{sp:08X} outside app RAM")
    if not pc & 1 or not base <= (pc & ~1) < base + len(payload):
        raise ValueError(f"reset vector 0x{pc:08X} not a Thumb address inside the image "
                         f"(linked for the wrong slot?)")


def prov_block(pubkey: bytes, key_id: int, device_id: bytes) -> bytes:
    body = struct.pack("<II", PROV_MAGIC, key_id) + pubkey + device_id.ljust(16, b"\0")[:16]
    return body + struct.pack("<I", zlib.crc32(body))
