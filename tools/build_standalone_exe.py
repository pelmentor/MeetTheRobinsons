"""
build_standalone_exe.py — turn the PE-sieve memory dump into a standalone runnable EXE.

The SecuROM 7 stub on Meet the Robinsons does aPLib decompression + BCJ-86 byte
filter + custom IAT resolver — all documented in
research/findings/securom7-stub-re.md. By the time pe-sieve runs, the stub has
already done its job: rr01 is fully decompressed, BCJ-filtered, and the IAT
slots are populated.

So all we need to produce a runnable standalone Wilbur_unpacked.exe is:

1. Take the existing PE-sieve dump verbatim (sections, IAT, resources).
2. Rewrite AddressOfEntryPoint from the stub entry (VA 0x02EF16A0) to the real
   game entry (VA 0x0062B48A = _WinMainCRTStartup).
3. Clear the high bit on bytes 0x2DF and 0x307 of the PE header if set
   (SecuROM marker bits the stub clears at runtime — usually already cleared
   in our dump because pe-sieve ran after the stub).
4. Optionally zero out the obsolete SecuROM stub region in rr02 (~0x500 bytes
   at the end of rr02 starting at the original OEP) so it doesn't confuse
   downstream tools. Off by default — keeping it for now since it's harmless.

The result loads in IDA Pro without special handling, and runs standalone
(launcher / dxwrapper still work — the standalone EXE just skips the SecuROM
stub stage that's no longer needed).

Output is gitignored (analysis-only, never committed).

Usage:
    python tools/build_standalone_exe.py
    python tools/build_standalone_exe.py --in <dump.exe> --out <Wilbur_unpacked.exe>
    python tools/build_standalone_exe.py --zero-stub    # also zero the obsolete stub
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

DEFAULT_IN  = Path("ida/dumps/process_22276/400000.Wilbur.exe")
DEFAULT_OUT = Path("ida/Wilbur_unpacked.exe")

IMAGEBASE        = 0x00400000
REAL_OEP_VA      = 0x0062B48A           # _WinMainCRTStartup
REAL_OEP_RVA     = REAL_OEP_VA - IMAGEBASE
STUB_OEP_VA      = 0x02EF16A0           # what pe-sieve preserved
STUB_OEP_RVA     = STUB_OEP_VA - IMAGEBASE

# PE header bytes the stub clears at runtime. We mirror that here so the
# standalone PE looks the same as the runtime image. If pe-sieve already
# captured them cleared, this is a no-op.
PATCH_BYTES = [0x2DF, 0x307]

# Region of rr02 that holds the SecuROM stub itself (entry -> final jmp + a
# few bytes of slack). With --zero-stub we wipe it; otherwise it stays as
# dead code in the standalone PE.
STUB_FILE_RANGE_START = 0x02EF16A0 - IMAGEBASE   # = 0x02AF16A0 (file offset == RVA in pe-sieve dump)
STUB_FILE_RANGE_END   = STUB_FILE_RANGE_START + 0x500


def parse_pe_basics(data: bytes) -> dict:
    """Minimal PE header parsing. Returns offsets we'll patch."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE: missing MZ magic")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError(f"not a PE: missing PE signature at 0x{e_lfanew:X}")

    coff = e_lfanew + 4
    opt = coff + 20
    if data[opt:opt + 2] != b"\x0B\x01":
        raise ValueError("not PE32 (i386)")

    addr_of_entry_offset = opt + 16
    addr_of_entry        = struct.unpack_from("<I", data, addr_of_entry_offset)[0]
    image_base           = struct.unpack_from("<I", data, opt + 28)[0]

    return {
        "e_lfanew": e_lfanew,
        "addr_of_entry_offset": addr_of_entry_offset,
        "addr_of_entry": addr_of_entry,
        "image_base": image_base,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in",  dest="inp",  type=Path, default=DEFAULT_IN)
    ap.add_argument("--out", dest="outp", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--zero-stub", action="store_true",
                    help="zero out the obsolete SecuROM stub region in rr02")
    args = ap.parse_args()

    if not args.inp.is_file():
        print(f"error: input not found: {args.inp}", file=sys.stderr)
        return 1

    print(f"reading: {args.inp}  ({args.inp.stat().st_size:,} bytes)")
    data = bytearray(args.inp.read_bytes())

    pe = parse_pe_basics(bytes(data))

    if pe["image_base"] != IMAGEBASE:
        print(f"error: ImageBase is 0x{pe['image_base']:08X}, expected 0x{IMAGEBASE:08X}",
              file=sys.stderr)
        return 1

    print(f"\n--- before ---")
    print(f"  ImageBase           = 0x{pe['image_base']:08X}")
    print(f"  AddressOfEntryPoint = 0x{pe['addr_of_entry']:08X} "
          f"(VA 0x{IMAGEBASE + pe['addr_of_entry']:08X})")

    # Patch 1: AddressOfEntryPoint -> real OEP RVA
    if pe["addr_of_entry"] != REAL_OEP_RVA:
        if pe["addr_of_entry"] != STUB_OEP_RVA:
            print(f"WARNING: existing entry isn't the stub at 0x{STUB_OEP_RVA:X}; "
                  f"is 0x{pe['addr_of_entry']:X}. Continuing — overwrite anyway.")
        struct.pack_into("<I", data, pe["addr_of_entry_offset"], REAL_OEP_RVA)
        print(f"\npatched AddressOfEntryPoint:")
        print(f"  0x{pe['addr_of_entry']:08X} -> 0x{REAL_OEP_RVA:08X}  "
              f"(VA 0x{REAL_OEP_VA:08X} = _WinMainCRTStartup)")
    else:
        print(f"\nAddressOfEntryPoint already correct.")

    # Patch 2: Clear high bits at 0x2DF / 0x307 if set (mirror what stub does at runtime)
    cleared = []
    for off in PATCH_BYTES:
        if off < len(data) and data[off] & 0x80:
            data[off] &= 0x7F
            cleared.append(off)
    if cleared:
        print(f"\ncleared high bit at file offsets: {[f'0x{o:X}' for o in cleared]}")
    else:
        print(f"\nno high-bit patches needed — already cleared in dump.")

    # Patch 3 (optional): zero the obsolete stub
    if args.zero_stub:
        zero_len = STUB_FILE_RANGE_END - STUB_FILE_RANGE_START
        if STUB_FILE_RANGE_END <= len(data):
            data[STUB_FILE_RANGE_START:STUB_FILE_RANGE_END] = b"\xCC" * zero_len
            print(f"\nzero-stub: replaced 0x{zero_len:X} bytes at "
                  f"0x{STUB_FILE_RANGE_START:X}..0x{STUB_FILE_RANGE_END:X} "
                  f"with INT3 padding (the original SecuROM stub bytes — "
                  f"now unreachable after OEP redirect).")

    args.outp.parent.mkdir(parents=True, exist_ok=True)
    args.outp.write_bytes(data)
    print(f"\nwrote: {args.outp}  ({args.outp.stat().st_size:,} bytes)")

    print(f"\nnext steps:")
    print(f"  python tools/verify_unpacked_pe.py {args.outp}")
    print(f"  Copy-Item {args.outp} Game\\Wilbur.exe -Force   # smoke test (back up original first)")
    print(f"  Open {args.outp} in IDA Pro, fresh database.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
