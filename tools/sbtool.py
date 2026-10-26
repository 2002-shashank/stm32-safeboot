#!/usr/bin/env python3
"""safeboot host tool: keys, provisioning block, image signing and inspection.

  sbtool.py keygen   --out keys/dev_ed25519.pem
  sbtool.py provision --key keys/dev_ed25519.pem --out build/prov.bin [--device-id HEX]
  sbtool.py sign     --key K --slot A|B|recovery --version N --in app.bin --out app.sbimg
  sbtool.py inspect  app.sbimg [--key K | --prov prov.bin]

The private key never leaves the host. A key without a passphrase is a
development key only.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sbimage as sb  # noqa: E402


def load_key(path: str) -> Ed25519PrivateKey:
    pw = os.environ.get("SB_KEY_PASSPHRASE")
    key = serialization.load_pem_private_key(Path(path).read_bytes(),
                                             password=pw.encode() if pw else None)
    if not isinstance(key, Ed25519PrivateKey):
        raise SystemExit(f"{path}: not an Ed25519 private key")
    return key


def raw_pub(key: Ed25519PrivateKey) -> bytes:
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def cmd_keygen(a) -> None:
    out = Path(a.out)
    if out.exists() and not a.force:
        raise SystemExit(f"{out} exists (use --force to overwrite)")
    key = Ed25519PrivateKey.generate()
    pw = os.environ.get("SB_KEY_PASSPHRASE")
    enc = (serialization.BestAvailableEncryption(pw.encode()) if pw
           else serialization.NoEncryption())
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                      serialization.PrivateFormat.PKCS8, enc))
    out.chmod(0o600)
    pub = raw_pub(key)
    print(f"wrote {out}  key_id=0x{sb.key_id_of(pub):08X}  pubkey={pub.hex()}")
    if not pw:
        print("note: unencrypted development key (set SB_KEY_PASSPHRASE to encrypt)")


def cmd_provision(a) -> None:
    key = load_key(a.key)
    pub = raw_pub(key)
    blob = sb.prov_block(pub, sb.key_id_of(pub), bytes.fromhex(a.device_id))
    Path(a.out).write_bytes(blob)
    print(f"wrote {a.out} ({len(blob)} B) for 0x{sb.PROV_ADDR:08X}  key_id=0x{sb.key_id_of(pub):08X}")


def cmd_sign(a) -> None:
    key = load_key(a.key)
    pub = raw_pub(key)
    load_addr, region_size = sb.REGIONS[a.slot]
    payload = Path(a.infile).read_bytes()
    payload += b"\xff" * (-len(payload) % 4)
    if len(payload) > region_size - sb.HDR_SIZE:
        raise SystemExit(f"image is {len(payload)} B; slot {a.slot} holds at most "
                         f"{region_size - sb.HDR_SIZE} B")
    if not 0 < a.version < 2**32:
        raise SystemExit("version must be 1..2^32-1")
    try:
        sb.check_vectors(payload, load_addr)
    except ValueError as e:
        raise SystemExit(f"refusing to sign: {e}")
    hdr = sb.Header(image_version=a.version, image_size=len(payload), load_addr=load_addr,
                    flags=sb.F_RECOVERY if a.slot == "recovery" else 0,
                    key_id=sb.key_id_of(pub), sha256=hashlib.sha256(payload).digest())
    hdr.signature = key.sign(hdr.signed_bytes())
    Path(a.outfile).write_bytes(hdr.to_bytes() + payload)
    print(f"signed {a.outfile}: slot {a.slot} @0x{load_addr:08X} v{a.version} "
          f"{len(payload)} B sha256={hdr.sha256.hex()[:16]}...")


def cmd_inspect(a) -> None:
    raw = Path(a.image).read_bytes()
    h = sb.Header.parse(raw)
    payload = raw[sb.HDR_SIZE:sb.HDR_SIZE + h.image_size]
    region = next((n for n, (addr, _) in sb.REGIONS.items() if addr == h.load_addr), "?")
    print(f"version     {h.image_version}\nsize        {h.image_size}\n"
          f"load_addr   0x{h.load_addr:08X} (slot {region})\nflags       0x{h.flags:08X}\n"
          f"key_id      0x{h.key_id:08X}\nsha256      {h.sha256.hex()}")
    ok = len(payload) == h.image_size and hashlib.sha256(payload).digest() == h.sha256
    print(f"hash        {'OK' if ok else 'MISMATCH'}")
    pub = None
    if a.key:
        pub = raw_pub(load_key(a.key))
    elif a.prov:
        pub = Path(a.prov).read_bytes()[8:40]
    if pub is not None:
        try:
            Ed25519PublicKey.from_public_bytes(pub).verify(h.signature, h.signed_bytes())
            print("signature   OK")
        except InvalidSignature:
            print("signature   INVALID")
            ok = False
    sys.exit(0 if ok else 1)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("--out", required=True)
    k.add_argument("--force", action="store_true")
    k.set_defaults(fn=cmd_keygen)
    v = sub.add_parser("provision")
    v.add_argument("--key", required=True)
    v.add_argument("--out", required=True)
    v.add_argument("--device-id", default="", help="up to 16 bytes, hex")
    v.set_defaults(fn=cmd_provision)
    s = sub.add_parser("sign")
    s.add_argument("--key", required=True)
    s.add_argument("--slot", required=True, choices=sorted(sb.REGIONS))
    s.add_argument("--version", required=True, type=int)
    s.add_argument("--in", dest="infile", required=True)
    s.add_argument("--out", dest="outfile", required=True)
    s.set_defaults(fn=cmd_sign)
    i = sub.add_parser("inspect")
    i.add_argument("image")
    i.add_argument("--key")
    i.add_argument("--prov")
    i.set_defaults(fn=cmd_inspect)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
