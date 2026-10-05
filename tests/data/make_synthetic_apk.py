#!/usr/bin/env python3
"""Builds tests/data/synthetic.apk, the fixture for the APK reader tests.

A tiny but structurally real APK:
  lib/arm64-v8a/libgood.so    stored, ZIP data 16 KB aligned, PT_LOAD align 16 KB
  lib/arm64-v8a/libbad.so     stored, not 16 KB aligned,      PT_LOAD align 4 KB
  lib/armeabi-v7a/libold.so   deflated, 32-bit ARM,           PT_LOAD align 4 KB
  META-INF/CERT.SF, CERT.RSA  a v1 (JAR) signature
  APK Signing Block           v2 and v3 signers carrying cert_android_debug.der,
                              plus a verity padding block

Signatures and digests are placeholders: the reader only parses structure, it
does not verify. Run from anywhere; writes next to itself.
"""
import os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
read = lambda name: open(os.path.join(HERE, name), "rb").read()

def elf64(machine, aligns, size=8192):
    phnum = len(aligns) + 1
    head = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    head += struct.pack("<HHIQQQIHHHHHH", 3, machine, 1, 0, 64, 0, 0, 64, 56, phnum, 64, 0, 0)
    ph = b""
    for a in aligns:  # PT_LOAD
        ph += struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, 0x1000, 0x1000, a)
    ph += struct.pack("<IIQQQQQQ", 2, 6, 0, 0, 0, 0x100, 0x100, 8)  # PT_DYNAMIC, ignored
    return (head + ph).ljust(size, b"\0")

def elf32(machine, aligns, size=4096):
    phnum = len(aligns)
    head = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    head += struct.pack("<HHIIIIIHHHHHH", 3, machine, 1, 0, 52, 0, 0, 52, 32, phnum, 40, 0, 0)
    ph = b"".join(struct.pack("<IIIIIIII", 1, 0, 0, 0, 0x800, 0x800, 5, a) for a in aligns)
    return (head + ph).ljust(size, b"\0")

ENTRIES = [
    ("AndroidManifest.xml", b"<manifest/>" * 20, 8, None),
    ("lib/arm64-v8a/libgood.so", elf64(0xB7, [0x4000, 0x4000]), 0, 16384),
    ("lib/arm64-v8a/libbad.so", elf64(0xB7, [0x1000, 0x1000]), 0, 4),
    ("lib/armeabi-v7a/libold.so", elf32(0x28, [0x1000]), 8, None),
    ("META-INF/CERT.SF", b"Signature-Version: 1.0\r\n", 8, None),
    ("META-INF/CERT.RSA", read("v1_signature.rsa"), 8, None),
]

out = bytearray()
central = bytearray()
for name, data, method, align in ENTRIES:
    nb = name.encode()
    payload = zlib.compress(data)[2:-4] if method == 8 else data
    crc = zlib.crc32(data)
    extra = b""
    if align:
        # zipalign-style padding: extra field 0xd935 carrying the alignment.
        base = len(out) + 30 + len(nb) + 6
        pad = (-base) % align
        extra = struct.pack("<HHH", 0xD935, 2 + pad, align) + bytes(pad)
    offset = len(out)
    out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, method, 0, 0x21, crc,
                       len(payload), len(data), len(nb), len(extra)) + nb + extra + payload
    central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, method, 0, 0x21, crc,
                           len(payload), len(data), len(nb), 0, 0, 0, 0, 0, offset) + nb

lp = lambda b: struct.pack("<I", len(b)) + b
cert = read("cert_android_debug.der")
digests = lp(lp(struct.pack("<I", 0x0103) + lp(bytes(32))))
signatures = lp(lp(struct.pack("<I", 0x0103) + lp(bytes(256))))
pubkey = lp(bytes(16))
v2_signed = lp(digests) + lp(lp(cert)) + lp(b"")
v2 = lp(lp(lp(v2_signed) + signatures + pubkey))
v3_signed = lp(digests) + lp(lp(cert)) + struct.pack("<II", 24, 0x7FFFFFFF) + lp(b"")
v3 = lp(lp(lp(v3_signed) + struct.pack("<II", 24, 0x7FFFFFFF) + signatures + pubkey))
pairs = b""
for block_id, value in ((0x7109871A, v2), (0xF05368C0, v3), (0x42726577, bytes(100))):
    pairs += struct.pack("<QI", len(value) + 4, block_id) + value
size = len(pairs) + 8 + 16
out += struct.pack("<Q", size) + pairs + struct.pack("<Q", size) + b"APK Sig Block 42"

cd_offset = len(out)
out += central
out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(ENTRIES), len(ENTRIES), len(central),
                   cd_offset, 0)
open(os.path.join(HERE, "synthetic.apk"), "wb").write(out)
print(f"synthetic.apk: {len(out)} bytes, central directory at {cd_offset}")
