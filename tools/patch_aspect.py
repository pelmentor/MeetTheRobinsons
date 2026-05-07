"""Patch the hardcoded 4:3 aspect float in Wilbur.exe.

Per the WSGF widescreen hack: the game stores its projection aspect ratio as
a 32-bit float constant in .rdata with value 4/3 = 0x3FAAAAAB (bytes
`AB AA AA 3F` little-endian). Replacing it with another aspect makes the game
compute a correct projection matrix natively — no D3D-level matrix hooking
needed.

By default we auto-detect the primary monitor's aspect and patch to that.

Usage:
    python tools\\patch_aspect.py                    # auto: monitor aspect
    python tools\\patch_aspect.py --aspect 16:9      # explicit ratio
    python tools\\patch_aspect.py --aspect 2.388889  # explicit float
    python tools\\patch_aspect.py --restore          # restore from .bak
    python tools\\patch_aspect.py --check            # report current state
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import shutil
import struct
import sys
from pathlib import Path

REPO   = Path(__file__).resolve().parent.parent
WILBUR = REPO / "Game" / "Wilbur.exe"
BAK    = WILBUR.with_suffix(".exe.bak")

OLD_PATTERN = bytes.fromhex("AB AA AA 3F".replace(" ", ""))  # float 1.333...

# Known reference values from WSGF guide.
KNOWN = {
    (4, 3):   0x3FAAAAAB,  # AB AA AA 3F
    (16, 10): 0x3FCCCCCD,  # CD CC CC 3F  (1.6)
    (16, 9):  0x3FE38E39,  # 39 8E E3 3F  (1.777...)
    (21, 9):  0x4018E560,  # 60 E5 18 40  (2.388...)
    (48, 9):  0x40AAAAAB,  # AB AA AA 40  (5.333...)
}


def primary_monitor_aspect() -> tuple[float, int, int]:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    SM_CXSCREEN = 0
    SM_CYSCREEN = 1
    w = user32.GetSystemMetrics(SM_CXSCREEN)
    h = user32.GetSystemMetrics(SM_CYSCREEN)
    if w <= 0 or h <= 0:
        raise RuntimeError("GetSystemMetrics returned bad dims")
    return w / h, w, h


def parse_aspect_arg(s: str) -> float:
    if ":" in s:
        a, b = s.split(":", 1)
        return float(a) / float(b)
    return float(s)


def float_bytes(value: float) -> bytes:
    return struct.pack("<f", value)


def find_pattern_offsets(data: bytes, pattern: bytes) -> list[int]:
    out, start = [], 0
    while True:
        i = data.find(pattern, start)
        if i < 0:
            break
        out.append(i)
        start = i + 1
    return out


def cmd_check():
    if not WILBUR.exists():
        print(f"[err] {WILBUR} not found")
        sys.exit(1)
    data = WILBUR.read_bytes()
    bak  = BAK.read_bytes() if BAK.exists() else None
    print(f"file: {WILBUR}  size={len(data):,}")
    if bak is not None:
        print(f"backup present: {BAK.name}  size={len(bak):,}  identical-to-current={data == bak}")
    else:
        print("no backup yet")

    # Look for the canonical 4/3 pattern AND any of the known ones.
    for ratio, raw in KNOWN.items():
        pat = struct.pack("<I", raw)
        offs = find_pattern_offsets(data, pat)
        if offs:
            label = f"{ratio[0]}:{ratio[1]}"
            print(f"  {label:<6}  ({pat.hex(' ')})  matches at: {[hex(o) for o in offs]}")


def cmd_restore():
    if not BAK.exists():
        print(f"[err] no backup at {BAK}")
        sys.exit(1)
    shutil.copy2(BAK, WILBUR)
    print(f"[ok] restored {WILBUR.name} from {BAK.name}")


def cmd_patch(aspect: float):
    if not WILBUR.exists():
        print(f"[err] {WILBUR} not found")
        sys.exit(1)

    if not BAK.exists():
        shutil.copy2(WILBUR, BAK)
        print(f"[ok] backup created: {BAK.name}")
    else:
        print(f"[note] backup already exists ({BAK.name}) — not overwriting")

    data = bytearray(BAK.read_bytes())  # always patch from clean original
    new_bytes = float_bytes(aspect)
    print(f"target aspect = {aspect:.6f}  -> bytes {new_bytes.hex(' ')}")

    offs = find_pattern_offsets(bytes(data), OLD_PATTERN)
    if not offs:
        print(f"[err] pattern AB AA AA 3F not found in original — already patched? "
              f"run --check or --restore first")
        sys.exit(1)
    print(f"found {len(offs)} occurrence(s) of AB AA AA 3F at {[hex(o) for o in offs]}")

    for o in offs:
        data[o:o + 4] = new_bytes

    WILBUR.write_bytes(bytes(data))
    print(f"[ok] patched {WILBUR.name} ({len(offs)} site(s))")


def main():
    p = argparse.ArgumentParser()
    g = p.add_mutually_exclusive_group()
    g.add_argument("--check",   action="store_true", help="report what aspect bytes are in the EXE")
    g.add_argument("--restore", action="store_true", help="restore Wilbur.exe from backup")
    p.add_argument("--aspect", type=str, default=None,
                   help="aspect to patch in: 'W:H' (e.g. 16:9, 21:9) or float (e.g. 1.7778). "
                        "Default: auto-detect primary monitor.")
    args = p.parse_args()

    if args.check:
        cmd_check()
        return
    if args.restore:
        cmd_restore()
        return

    if args.aspect:
        aspect = parse_aspect_arg(args.aspect)
        print(f"using explicit aspect: {args.aspect} -> {aspect:.6f}")
    else:
        aspect, w, h = primary_monitor_aspect()
        print(f"primary monitor: {w}x{h}  -> aspect {aspect:.6f}")

    cmd_patch(aspect)


if __name__ == "__main__":
    main()
