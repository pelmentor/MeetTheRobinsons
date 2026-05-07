"""Locate the 4:3 aspect float (0x3FAAAAAB / AB AA AA 3F) inside the
unpacked Wilbur.exe and report the corresponding runtime RVA.

Wilbur.exe has SecuROM rr01/rr02 sections that are encrypted on disk in the
SHIPPED binary, but the unpacked dump (ida/dumps/process_*/400000.Wilbur.exe)
contains the decrypted bytes at the same RVAs they occupy at runtime. So an
RVA found in the dump = a runtime RVA in the live process (ImageBase 0x400000,
no ASLR).
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DUMP = next(REPO.glob("ida/dumps/process_*/400000.Wilbur.exe"))

PATTERN = bytes.fromhex("AB AA AA 3F".replace(" ", ""))  # float 4/3


def parse_sections(data: bytes):
    """Return list of (name, raw_start, raw_end, va_start)."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    nt = e_lfanew
    if data[nt:nt+4] != b"PE\x00\x00":
        raise RuntimeError("not a PE")
    coff = nt + 4
    n_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size  = struct.unpack_from("<H", data, coff + 16)[0]
    sec_off   = coff + 20 + opt_size

    out = []
    for i in range(n_sections):
        h = sec_off + i * 40
        name = data[h:h+8].rstrip(b"\x00").decode("latin-1")
        virt_size, virt_addr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, h + 8)
        out.append((name, raw_ptr, raw_ptr + raw_size, virt_addr, virt_size))
    return out


def file_off_to_rva(sections, off):
    for (name, raw_start, raw_end, va_start, va_size) in sections:
        if raw_start <= off < raw_end:
            return name, va_start + (off - raw_start)
    return None, None


def main():
    print(f"unpacked dump: {DUMP}")
    data = DUMP.read_bytes()
    print(f"size: {len(data):,}")

    secs = parse_sections(data)
    print("sections:")
    for (name, raw_start, raw_end, va, vs) in secs:
        print(f"  {name:<10}  raw 0x{raw_start:08X}..0x{raw_end:08X}  RVA 0x{va:08X}  vsize 0x{vs:X}")

    print()
    print(f"searching for {PATTERN.hex(' ')} ...")
    offsets = []
    start = 0
    while True:
        i = data.find(PATTERN, start)
        if i < 0: break
        offsets.append(i)
        start = i + 1
    print(f"  {len(offsets)} match(es)")

    for off in offsets:
        sec_name, rva = file_off_to_rva(secs, off)
        if rva is None:
            print(f"  file +0x{off:08X}  not in any section")
            continue
        va = 0x400000 + rva
        print(f"  file +0x{off:08X}  in section {sec_name!r:8}  RVA 0x{rva:08X}  VA 0x{va:08X}")


if __name__ == "__main__":
    main()
