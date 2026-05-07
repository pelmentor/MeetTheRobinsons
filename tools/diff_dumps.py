"""
diff_dumps.py — Compare two PE-sieve memory dumps of Wilbur.exe.

Reports the byte regions that differ between an old (less-complete) dump and a
new (more-complete) dump. Used in the full-unpack procedure (Stage A.4) to
verify that a new in-game dump captured previously-encrypted code paths.

Output:
- Summary: total differing bytes, count of newly-instructions-shaped regions.
- Per-region detail: VA range, length, whether the OLD region was zeros / 0xCC pad / partial bytes.
- Sanity probe: presence of `FF 15 20 60 6A 00` (call DirectInput8Create) and other markers.

Usage:
    python tools/diff_dumps.py <old_dump.exe> <new_dump.exe>
    python tools/diff_dumps.py ida/dumps/process_22276/400000.Wilbur.exe \
                               ida/dumps/full/process_<new>/400000.Wilbur.exe

No external deps. ImageBase 0x00400000 assumed (no ASLR on this binary).
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path

IMAGEBASE = 0x00400000


@dataclass
class DiffRegion:
    va_start: int
    va_end: int  # inclusive
    old_bytes: bytes
    new_bytes: bytes

    @property
    def length(self) -> int:
        return self.va_end - self.va_start + 1

    @property
    def old_is_zeros(self) -> bool:
        return all(b == 0 for b in self.old_bytes)

    @property
    def old_is_pad(self) -> bool:
        return all(b == 0xCC for b in self.old_bytes)

    @property
    def looks_like_code(self) -> bool:
        # Heuristic: contains any of the common x86 call/jmp/push opcodes
        # in a density that suggests instructions, not data.
        opcodes_seen = sum(
            1 for b in self.new_bytes
            if b in (0xE8, 0xE9, 0xFF, 0x68, 0x6A, 0x55, 0x53, 0x56, 0x57, 0x8B, 0xC3, 0xC2)
        )
        return opcodes_seen >= max(4, self.length // 16)


def parse_pe_image_offset(data: bytes) -> int:
    """Return the file offset where the loaded image starts (skip PE header padding)."""
    # For PE-sieve dumps, the file IS the loaded image (RVA == file offset
    # after section alignment). ImageBase + RVA == VA. We use offset 0 as RVA 0.
    return 0


def find_diff_regions(
    old: bytes, new: bytes, *, min_run: int = 16, gap_tolerance: int = 8
) -> list[DiffRegion]:
    """
    Walk both buffers, find runs of differing bytes.
    Coalesce runs separated by `gap_tolerance` matching bytes.
    Drop runs shorter than `min_run` bytes.
    """
    n = min(len(old), len(new))
    regions: list[DiffRegion] = []
    i = 0
    while i < n:
        if old[i] != new[i]:
            start = i
            last_diff = i
            j = i + 1
            while j < n:
                if old[j] != new[j]:
                    last_diff = j
                    j += 1
                elif j - last_diff <= gap_tolerance:
                    j += 1
                else:
                    break
            end = last_diff
            if end - start + 1 >= min_run:
                regions.append(DiffRegion(
                    va_start=IMAGEBASE + start,
                    va_end=IMAGEBASE + end,
                    old_bytes=old[start:end + 1],
                    new_bytes=new[start:end + 1],
                ))
            i = j
        else:
            i += 1
    return regions


# Byte patterns that, if found in NEW dump and not in OLD, indicate
# specific code paths were captured.
COVERAGE_PROBES: list[tuple[str, bytes]] = [
    ("call dword ptr [DirectInput8Create]", b"\xff\x15\x20\x60\x6a\x00"),
    ("call DirectInput8Create_thunk @0x655AB0", b"\xff\x15\xb0\x5a\x65\x00"),
    ("push 0x800 (DIRECTINPUT_VERSION)", b"\x68\x00\x08\x00\x00"),
    # Add more as we discover them. Each tuple = (description, byte_pattern).
]


def report_coverage_probes(old: bytes, new: bytes) -> None:
    print("\n=== Coverage probes ===")
    print(f"{'Probe':<55} {'Old':>5} {'New':>5} {'Δ':>5}  Verdict")
    print("-" * 90)
    for desc, pat in COVERAGE_PROBES:
        old_count = old.count(pat)
        new_count = new.count(pat)
        delta = new_count - old_count
        if old_count == 0 and new_count > 0:
            verdict = "RECOVERED ✓"
        elif old_count > 0 and new_count > old_count:
            verdict = "more ✓"
        elif old_count > 0 and new_count == old_count:
            verdict = "no change"
        elif old_count == 0 and new_count == 0:
            verdict = "still missing"
        else:
            verdict = "?"
        print(f"{desc:<55} {old_count:>5} {new_count:>5} {delta:>+5}  {verdict}")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[0])
        print("\nusage: python diff_dumps.py <old_dump.exe> <new_dump.exe>")
        return 2

    old_path = Path(sys.argv[1])
    new_path = Path(sys.argv[2])

    if not old_path.is_file():
        print(f"error: old dump not found: {old_path}", file=sys.stderr)
        return 1
    if not new_path.is_file():
        print(f"error: new dump not found: {new_path}", file=sys.stderr)
        return 1

    old = old_path.read_bytes()
    new = new_path.read_bytes()

    print(f"old: {old_path}  ({len(old):,} bytes)")
    print(f"new: {new_path}  ({len(new):,} bytes)")

    if len(old) != len(new):
        print(f"WARNING: dump sizes differ by {abs(len(old) - len(new)):,} bytes.")
        print("Comparison will use the shorter length; size mismatch usually means")
        print("a different ImageSize from the loader, NOT recovered code.")

    regions = find_diff_regions(old, new)
    total_diff = sum(r.length for r in regions)
    code_regions = [r for r in regions if r.looks_like_code]

    print(f"\n=== Diff summary ===")
    print(f"differing regions:        {len(regions):>6}")
    print(f"differing bytes (total):  {total_diff:>6,}")
    print(f"code-shaped regions:      {len(code_regions):>6}  "
          f"({sum(r.length for r in code_regions):,} bytes)")

    if regions:
        # Show top 20 by length, grouping into "old-was-zeros" (likely
        # newly-decrypted) vs "old-was-padding" vs "old-had-content".
        print(f"\n=== Top differing regions (up to 20) ===")
        regions_sorted = sorted(regions, key=lambda r: r.length, reverse=True)
        print(f"{'VA range':<23} {'len':>7}  {'old':<8}  {'looks like'}")
        print("-" * 70)
        for r in regions_sorted[:20]:
            if r.old_is_zeros:
                old_state = "zeros"
            elif r.old_is_pad:
                old_state = "0xCC pad"
            else:
                old_state = "mixed"
            verdict = "code" if r.looks_like_code else "data?"
            print(f"0x{r.va_start:08X}-0x{r.va_end:08X} {r.length:>7,}  "
                  f"{old_state:<8}  {verdict}")

    report_coverage_probes(old, new)

    return 0 if regions else 0


if __name__ == "__main__":
    sys.exit(main())
