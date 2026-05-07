"""
inspect_pe.py — quick PE-header inspector for the dumped Wilbur.exe.

Prints what's already in place, what we'd need to fix to build a standalone EXE.
Reuses the parser from verify_unpacked_pe.py.

Usage:
    python tools/inspect_pe.py ida/dumps/process_22276/400000.Wilbur.exe
"""

import struct
import sys
from pathlib import Path

if len(sys.argv) != 2:
    print("usage: python tools/inspect_pe.py <pe-file>", file=sys.stderr)
    sys.exit(2)

p = Path(sys.argv[1])
data = p.read_bytes()
print(f"file: {p}  ({len(data):,} bytes)")

# DOS header
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
print(f"e_lfanew: 0x{e_lfanew:X}")

# PE header
coff = e_lfanew + 4
machine, num_sections, timestamp, ptr_symtab, num_syms, opt_size, characteristics = \
    struct.unpack_from("<HHIIIHH", data, coff)
print(f"\nCOFF header:")
print(f"  machine = 0x{machine:04X}")
print(f"  num_sections = {num_sections}")
print(f"  optional header size = 0x{opt_size:X}")

# Optional header
opt = coff + 20
magic = struct.unpack_from("<H", data, opt)[0]
addr_of_entry, base_of_code, base_of_data, image_base = struct.unpack_from("<IIII", data, opt + 16)
size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
size_of_headers = struct.unpack_from("<I", data, opt + 60)[0]
checksum = struct.unpack_from("<I", data, opt + 64)[0]
dll_chars = struct.unpack_from("<H", data, opt + 70)[0]
data_dir_count = struct.unpack_from("<I", data, opt + 92)[0]

print(f"\nOptional header:")
print(f"  magic = 0x{magic:04X} ({'PE32' if magic == 0x10B else 'PE32+'})")
print(f"  ImageBase = 0x{image_base:08X}")
print(f"  AddressOfEntryPoint = 0x{addr_of_entry:08X} (VA 0x{image_base + addr_of_entry:08X})")
print(f"  SizeOfImage = 0x{size_of_image:X}")
print(f"  SizeOfHeaders = 0x{size_of_headers:X}")
print(f"  CheckSum = 0x{checksum:08X}")
print(f"  DLLCharacteristics = 0x{dll_chars:04X}")
print(f"  NumberOfRvaAndSizes = {data_dir_count}")

# Data directories
DD_NAMES = [
    "Export", "Import", "Resource", "Exception",
    "Security", "BaseReloc", "Debug", "Architecture",
    "GlobalPtr", "TLS", "LoadConfig", "BoundImport",
    "IAT", "DelayImport", "COMDescriptor", "Reserved15",
]
print(f"\nData Directories:")
dd_off = opt + 96
for i in range(min(data_dir_count, 16)):
    rva, size = struct.unpack_from("<II", data, dd_off + i * 8)
    if rva or size:
        print(f"  [{i:>2}] {DD_NAMES[i]:<14}  RVA=0x{rva:08X}  Size=0x{size:08X}")

# Sections
print(f"\nSections ({num_sections}):")
sec_off = opt + opt_size
for i in range(num_sections):
    s = sec_off + i * 40
    name = data[s:s+8].rstrip(b"\x00").decode("ascii", errors="replace")
    v_size, v_addr, r_size, r_off, _, _, _, _, chars = struct.unpack_from("<IIIIIIHHH", data, s + 8)
    chars = struct.unpack_from("<I", data, s + 36)[0]
    f = ""
    if chars & 0x20000000: f += "X"
    if chars & 0x40000000: f += "R"
    if chars & 0x80000000: f += "W"
    if chars & 0x00000020: f += "C"  # code
    if chars & 0x00000040: f += "I"  # init data
    print(f"  [{i}] {name:<8} VA=0x{image_base + v_addr:08X} VSize=0x{v_size:08X} "
          f"RSize=0x{r_size:08X} ROff=0x{r_off:08X} flags=[{f}]")

# Specific bytes the SecuROM stub patches at runtime
print(f"\nStub-patched bytes (need to clear high bits in standalone):")
for off, label in [(0x2DF, "0x2DF (= 0x4002DF)"), (0x307, "0x307 (= 0x400307)")]:
    if off < len(data):
        b = data[off]
        marker = "set"  if b & 0x80 else "clear"
        print(f"  byte at file 0x{off:X} {label}: 0x{b:02X} (high bit {marker})")
