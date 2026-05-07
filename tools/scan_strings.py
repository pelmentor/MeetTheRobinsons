"""
scan_strings.py - quick string scanner for sanity-checking the unpacked Wilbur.

Pulls all printable ASCII strings (>=4 chars) and categorises a few:
- debug / assert markers
- format strings
- source-file paths
- function-name-like strings
- Russian (cp1251) strings

Compares two files side-by-side so you can see SecuROM-packed vs unpacked.

Usage:
    python tools/scan_strings.py Game/Wilbur.exe ida/Wilbur_unpacked.exe
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from collections import Counter


def scan_ascii(data: bytes, min_len: int = 4) -> list[str]:
    pattern = re.compile(rb"[\x20-\x7E]{" + str(min_len).encode() + rb",}")
    return [m.decode("ascii", errors="replace") for m in pattern.findall(data)]


def scan_cp1251(data: bytes, min_len: int = 4) -> list[str]:
    # cp1251 printable: ASCII + Cyrillic block 0xC0-0xFF
    pattern = re.compile(rb"[\x20-\x7E\xC0-\xFF]{" + str(min_len).encode() + rb",}")
    out = []
    for m in pattern.findall(data):
        s = m.decode("cp1251", errors="replace")
        # only keep if it has at least one cyrillic char (not pure ASCII)
        if any(c >= "Ѐ" and c <= "ӿ" for c in s):
            out.append(s)
    return out


def categorise(strings: list[str]) -> dict[str, list[str]]:
    cats: dict[str, list[str]] = {
        "debug_markers": [],
        "assert":        [],
        "format":        [],
        "source_paths":  [],
        "func_names":    [],
        "errors":        [],
    }
    for s in strings:
        sl = s.lower()
        if "debug" in sl or "dbg" in sl:
            cats["debug_markers"].append(s)
        if "assert" in sl or "abort" in sl:
            cats["assert"].append(s)
        if "%s" in s or "%d" in s or "%x" in s or "%f" in s:
            cats["format"].append(s)
        if re.search(r"\.(cpp|c|cxx|cc|h|hpp|inl):", s):
            cats["source_paths"].append(s)
        if re.match(r"^[A-Z][a-zA-Z]+::[A-Za-z_]+", s) or re.match(r"^[a-z_]+::[a-z_]+", s):
            cats["func_names"].append(s)
        if "fail" in sl or "error" in sl or "invalid" in sl:
            cats["errors"].append(s)
    return cats


def report(label: str, path: Path):
    data = path.read_bytes()
    print(f"\n{'='*70}")
    print(f"{label}: {path}")
    print(f"  size: {len(data):,} bytes")

    ascii_strings = scan_ascii(data)
    cp1251_strings = scan_cp1251(data)

    print(f"  ASCII strings (>=4 chars): {len(ascii_strings):>7,}")
    print(f"  cp1251 (cyrillic) strings: {len(cp1251_strings):>7,}")

    cats = categorise(ascii_strings)
    print(f"\n  categories (count of unique):")
    for k, v in cats.items():
        unique = sorted(set(v))
        print(f"    {k:<14} : {len(unique):>5}")

    print(f"\n  sample debug markers (first 8 unique):")
    for s in sorted(set(cats["debug_markers"]))[:8]:
        print(f"    {s[:100]}")

    print(f"\n  sample assert/abort (first 8 unique):")
    for s in sorted(set(cats["assert"]))[:8]:
        print(f"    {s[:100]}")

    print(f"\n  sample format strings (first 8 unique):")
    for s in sorted(set(cats["format"]))[:8]:
        print(f"    {s[:100]}")

    print(f"\n  sample source paths (first 8 unique):")
    for s in sorted(set(cats["source_paths"]))[:8]:
        print(f"    {s[:100]}")

    if cp1251_strings:
        print(f"\n  sample cyrillic strings (first 5 unique):")
        for s in sorted(set(cp1251_strings))[:5]:
            print(f"    {s[:80]}")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: python tools/scan_strings.py <pe1> [pe2] [...]", file=sys.stderr)
        return 2
    for arg in sys.argv[1:]:
        p = Path(arg)
        if not p.is_file():
            print(f"error: not found: {p}", file=sys.stderr)
            continue
        report(p.name, p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
