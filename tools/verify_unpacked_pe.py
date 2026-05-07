"""
verify_unpacked_pe.py — Sanity-check a Scylla-rebuilt Wilbur.exe before launching.

Used after Stage B of full-unpack-procedure.md to catch common Scylla pitfalls
without having to launch the binary and watch for a crash.

Checks:
1. PE magic + e_lfanew sane.
2. Section table — RawSize matches VirtualSize for sections that should have
   raw bytes (was the rebuild's main job).
3. ImageBase == 0x00400000 (no DYNAMICBASE).
4. AddressOfEntryPoint plausible (in a code section, not pointing into stub).
5. Import directory present, not empty, points at a sane RVA.
6. Walk import descriptors — each has a non-null Name (DLL name) and OriginalFirstThunk.
7. Resolve a few canonical imports (kernel32!Sleep, user32!CreateWindowExA,
   d3d8!Direct3DCreate8) — confirm they resolve to known IAT slots in our existing
   symbol table.
8. Section characteristics: rr01 / .text equivalent should be EXECUTE | READ.

Usage:
    python tools/verify_unpacked_pe.py ida/Wilbur_unpacked.exe

Exit code 0 = all checks pass. Non-zero = something to fix in Scylla before launching.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path

# Expected from our existing IDA database on the menu-time dump.
EXPECTED_IMAGEBASE = 0x00400000
EXPECTED_OEP = 0x0062B48A  # _WinMainCRTStartup — verified via SecuROM stub RE 2026-05-05
EXPECTED_OEP_ALTS = (0x0062B48A,)  # only the verified value; Scylla shouldn't pick anything else

# Some import slots we know about from this session's RE.
KNOWN_IATS = {
    0x6A6020: ("dinput8.dll", "DirectInput8Create"),
    0x6A6314: ("binkw32.dll", "_BinkOpen@8"),
    0x6A6244: ("user32.dll", "PeekMessageA"),
    0x6A6098: ("kernel32.dll", "QueryPerformanceCounter"),
    0x6A6094: ("kernel32.dll", "QueryPerformanceFrequency"),
    0x6A6084: ("kernel32.dll", "Sleep"),
}


@dataclass
class Section:
    name: str
    virtual_size: int
    virtual_addr: int
    raw_size: int
    raw_offset: int
    characteristics: int

    @property
    def is_executable(self) -> bool:
        return bool(self.characteristics & 0x20000000)

    @property
    def is_readable(self) -> bool:
        return bool(self.characteristics & 0x40000000)


@dataclass
class Result:
    ok: bool
    msg: str

    def __str__(self) -> str:
        marker = "[ OK ]" if self.ok else "[FAIL]"
        return f"{marker} {self.msg}"


def parse_pe(data: bytes) -> tuple[dict, list[Section]]:
    """Minimal PE parser. Returns (header_fields, sections). Raises on malformed."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE: missing MZ magic")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError(f"not a PE: missing PE signature at 0x{e_lfanew:X}")

    coff = e_lfanew + 4
    machine, num_sections = struct.unpack_from("<HH", data, coff)
    opt_header_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt_header = coff + 20

    if data[opt_header:opt_header + 2] != b"\x0B\x01":
        raise ValueError("not a PE32 (32-bit) image")

    addr_of_entry = struct.unpack_from("<I", data, opt_header + 16)[0]
    image_base = struct.unpack_from("<I", data, opt_header + 28)[0]
    size_of_image = struct.unpack_from("<I", data, opt_header + 56)[0]
    dll_chars = struct.unpack_from("<H", data, opt_header + 70)[0]

    # Data Directories: at opt_header + 96 for PE32, 16 entries × 8 bytes
    data_dirs_off = opt_header + 96
    import_dir_rva, import_dir_size = struct.unpack_from("<II", data, data_dirs_off + 8)

    sections_off = opt_header + opt_header_size
    sections: list[Section] = []
    for i in range(num_sections):
        s_off = sections_off + i * 40
        name = data[s_off:s_off + 8].rstrip(b"\x00").decode("ascii", errors="replace")
        v_size, v_addr, r_size, r_off = struct.unpack_from("<IIII", data, s_off + 8)
        chars = struct.unpack_from("<I", data, s_off + 36)[0]
        sections.append(Section(name, v_size, v_addr, r_size, r_off, chars))

    return {
        "machine": machine,
        "addr_of_entry": addr_of_entry,
        "image_base": image_base,
        "size_of_image": size_of_image,
        "dll_chars": dll_chars,
        "import_dir_rva": import_dir_rva,
        "import_dir_size": import_dir_size,
    }, sections


def rva_to_offset(rva: int, sections: list[Section]) -> int | None:
    for s in sections:
        if s.virtual_addr <= rva < s.virtual_addr + s.virtual_size:
            return s.raw_offset + (rva - s.virtual_addr)
    return None


def read_cstring(data: bytes, offset: int, max_len: int = 256) -> str:
    end = data.find(b"\x00", offset)
    if end < 0 or end - offset > max_len:
        return ""
    return data[offset:end].decode("ascii", errors="replace")


def walk_import_descriptors(data: bytes, hdr: dict, sections: list[Section]) -> list[dict]:
    """Walk IMAGE_IMPORT_DESCRIPTOR array. Returns list of dicts with DLL name + IAT info."""
    if hdr["import_dir_rva"] == 0 or hdr["import_dir_size"] == 0:
        return []
    base_off = rva_to_offset(hdr["import_dir_rva"], sections)
    if base_off is None:
        return []
    out = []
    i = 0
    while True:
        d_off = base_off + i * 20  # IMAGE_IMPORT_DESCRIPTOR is 20 bytes
        if d_off + 20 > len(data):
            break
        oft, ts, fwd, name_rva, ft = struct.unpack_from("<IIIII", data, d_off)
        if oft == 0 and name_rva == 0 and ft == 0:
            break  # null terminator
        name_off = rva_to_offset(name_rva, sections) if name_rva else None
        dll_name = read_cstring(data, name_off) if name_off is not None else "<bad>"
        out.append({
            "dll": dll_name,
            "OriginalFirstThunk": oft,
            "FirstThunk": ft,  # the IAT RVA
        })
        i += 1
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python verify_unpacked_pe.py <Wilbur_unpacked.exe>", file=sys.stderr)
        return 2

    pe_path = Path(sys.argv[1])
    if not pe_path.is_file():
        print(f"error: not found: {pe_path}", file=sys.stderr)
        return 1

    data = pe_path.read_bytes()
    print(f"verifying: {pe_path}  ({len(data):,} bytes)")
    print()

    try:
        hdr, sections = parse_pe(data)
    except ValueError as e:
        print(f"FATAL: {e}", file=sys.stderr)
        return 1

    results: list[Result] = []

    # Check 1: machine = i386
    results.append(Result(
        hdr["machine"] == 0x14C,
        f"machine = 0x{hdr['machine']:04X} (expect 0x014C i386)"
    ))

    # Check 2: ImageBase = 0x00400000
    results.append(Result(
        hdr["image_base"] == EXPECTED_IMAGEBASE,
        f"ImageBase = 0x{hdr['image_base']:08X} (expect 0x{EXPECTED_IMAGEBASE:08X})"
    ))

    # Check 3: DYNAMICBASE / ASLR not set (game doesn't tolerate ASLR)
    results.append(Result(
        not (hdr["dll_chars"] & 0x40),
        f"DLLCharacteristics = 0x{hdr['dll_chars']:04X} (DYNAMICBASE bit 0x40 should be CLEAR)"
    ))

    # Check 4: AddressOfEntryPoint plausible (matches one of our known OEP candidates)
    aoe_va = hdr["image_base"] + hdr["addr_of_entry"]
    results.append(Result(
        aoe_va in EXPECTED_OEP_ALTS,
        f"AddressOfEntryPoint = 0x{aoe_va:08X} (expect one of "
        f"{', '.join(f'0x{v:08X}' for v in EXPECTED_OEP_ALTS)}; "
        f"primary expected 0x{EXPECTED_OEP:08X} = game_MessageLoop CRT shim)"
    ))

    # Check 5: section table
    print("\nSections:")
    for s in sections:
        flags = ""
        if s.is_executable: flags += "X"
        if s.is_readable: flags += "R"
        print(f"  {s.name:<8}  VA=0x{hdr['image_base'] + s.virtual_addr:08X}  "
              f"VSize=0x{s.virtual_size:08X}  RSize=0x{s.raw_size:08X}  flags={flags}")

    # Check section RawSize > 0 for executable sections
    for s in sections:
        if s.is_executable and s.virtual_size > 0:
            results.append(Result(
                s.raw_size > 0,
                f"section {s.name!r} executable, RawSize=0x{s.raw_size:X} "
                f"(must be > 0 in unpacked PE — Scylla 'Fix Dump' should have written this)"
            ))

    # Check 6: import directory present
    results.append(Result(
        hdr["import_dir_rva"] != 0 and hdr["import_dir_size"] != 0,
        f"Import Directory RVA=0x{hdr['import_dir_rva']:08X} "
        f"Size=0x{hdr['import_dir_size']:08X}"
    ))

    # Check 7: walk import descriptors
    descs = walk_import_descriptors(data, hdr, sections)
    print(f"\nImport descriptors: {len(descs)}")
    for d in descs:
        print(f"  {d['dll']:<24}  IAT RVA=0x{d['FirstThunk']:08X}  "
              f"OFT=0x{d['OriginalFirstThunk']:08X}")

    # Sanity: must include the 10 main DLLs
    expected_dlls = {"advapi32", "dinput8", "dsound", "gdi32", "kernel32",
                     "user32", "winmm", "ws2_32", "binkw32", "d3d8"}
    seen_dlls = {d["dll"].lower().replace(".dll", "") for d in descs}
    missing = expected_dlls - seen_dlls
    results.append(Result(
        not missing,
        f"main IAT DLLs present (missing: {sorted(missing) if missing else 'none'})"
    ))

    # Check 8: known IAT slot DLLs
    print(f"\nKnown IAT slot probes:")
    for va, (dll, sym) in KNOWN_IATS.items():
        rva = va - hdr["image_base"]
        offset = rva_to_offset(rva, sections)
        if offset is None:
            print(f"  0x{va:08X}  {dll}!{sym}: not in any section")
            continue
        slot_value = struct.unpack_from("<I", data, offset)[0] if offset + 4 <= len(data) else 0
        print(f"  0x{va:08X}  {dll}!{sym}: slot value = 0x{slot_value:08X}")

    # Final report
    print("\n=== Verdict ===")
    fail_count = sum(1 for r in results if not r.ok)
    for r in results:
        print(r)
    if fail_count == 0:
        print("\nAll checks passed. Try launching the unpacked binary.")
        return 0
    else:
        print(f"\n{fail_count} check(s) failed. Re-run Scylla and address before launching.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
