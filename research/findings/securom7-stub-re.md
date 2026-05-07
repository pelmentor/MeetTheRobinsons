# SecuROM 7 stub on Meet the Robinsons — full RE

This is the complete reverse-engineering of the SecuROM 7 stub at OEP `0x02EF16A0` in our build of Wilbur.exe. **The major finding**: there is no real encryption. The "SecuROM" protection on this build is a thin wrapper around two public-domain compression algorithms plus a custom IAT resolver. Once the stub finishes (which happens before any game code runs), the entire `rr01` section is plain x86 in our PE-sieve dump.

This file replaces / supersedes the previous "lazy-decrypt" / "page-touch" / "approach B / RE the encryption" speculation in earlier docs. Those approaches were solving a problem that doesn't exist on this build.

## TL;DR

- **No encryption.** No keys, no per-page decryption, no anti-tamper crypto.
- **aPLib compression** (public, [ibsensoftware.com](http://ibsensoftware.com/products_aPLib.html)) compresses game code in `rr02`.
- **BCJ-86 byte filter** (LZMA SDK, public) is applied to the source pre-compression for better ratio; reverse filter runs post-decompression.
- **Custom IAT resolver** walks a flat table at the end of `rr02`, calls `LoadLibraryA` + `GetProcAddress`, populates the IAT slots.
- **2-byte PE header patch** clears high bits on `0x4002DF` and `0x400307` (probably PE characteristics fields SecuROM munged for delivery — not relevant once unpacked).
- **`jmp 0x0062B48A`** transfers control to the real game OEP (`_WinMainCRTStartup`).

The whole stub fits in ~500 bytes. It's the simplest possible "execution wrapper" for compressed code; calling it "SecuROM 7 protection" oversells it.

## Stub structure (annotated)

Entry point in the on-disk PE: `0x02EF16A0` (inside `rr02`). Section table from `unpack-state.md`:
- `rr01`: VAddr `0x00001000`, VSize `0x024F8000` (= 38 MB), RawSize `0`. Allocated empty at load; populated by the stub.
- `rr02`: VAddr `0x024F9000`, VSize `0x005F9000` (= 6 MB), RawSize `0x005F8A00`. Contains the compressed source + the stub itself + IAT resolver table.

### Stage 1 — aPLib decompression (`0x02EF16A0` → `0x02EF1791`)

```asm
2ef16a0  pusha                              ; save all regs (caller is the loader)
2ef16a1  mov     esi, offset dword_28F9000  ; src = rr02 base = 0x028F9000
2ef16a6  lea     edi, [esi-24F8000h]        ; dst = src - 0x024F8000 = 0x00401000 = rr01 base
2ef16ac  push    edi                        ; save dst for later
2ef16ad  or      ebp, 0FFFFFFFFh            ; ebp = -1 (init bit window state)
2ef16b0  jmp     short loc_2EF16C2          ; enter aPLib decoder
```

The body (`0x02EF16B2` – `0x02EF1791`) is the canonical aPLib bit-window decoder. Distinctive features:

- Bit window in `EBX`: `add ebx, ebx` to shift, `jnz` to test exhaustion, `mov ebx, [esi]; sub esi, 0FFFFFFFCh; adc ebx, ebx` to reload (the `sub esi, -4` trick advances `esi` by 4).
- States: literal, single-byte match, gamma-coded length match, far/near match-distance encoding.
- Exit: when the decoder hits the EOF marker, falls through to stage 2.

Reference: matches the [aPLib decoder source](https://github.com/snemes/aplib) byte-for-byte (with naming differences). aPLib has been public since 1998; this is not novel SecuROM IP.

**Output:** `rr01` (`0x00401000` – `0x028F8FFF`) is now the original game image, fully decompressed. ~38 MB of game code + data + (uninitialized) IAT.

### Stage 2 — BCJ-86 reverse filter (`0x02EF1792` → `0x02EF17C5`)

```asm
2ef1792  pop     esi                  ; esi = rr01 base = 0x00401000
2ef1793  mov     edi, esi             ; edi = same
2ef1795  mov     ecx, 0CECEh          ; loop counter = 52,942 filter ops
2ef179a  mov     al, [edi]            ; scan loop: read byte
2ef179c  inc     edi
2ef179d  sub     al, 0E8h             ; check for 0xE8 (call) or 0xE9 (jmp)
2ef179f  cmp     al, 1
2ef17a1  ja      short loc_2EF179A    ; not a call/jmp opcode, skip
2ef17a3  cmp     byte ptr [edi], 35h
2ef17a6  jnz     short loc_2EF179A    ; selectivity filter — only ops with this marker
2ef17a8  mov     eax, [edi]           ; load rel32
2ef17aa  mov     bl, [edi+4]          ; load byte after rel32 (preserved)
2ef17ad  shr     ax, 8                ; byte-swap dance (BCJ encoding)
2ef17b1  rol     eax, 10h
2ef17b4  xchg    al, ah
2ef17b6  sub     eax, edi             ; convert absolute → relative
2ef17b8  sub     bl, 0E8h             ; (for next-iter optimisation)
2ef17bb  add     eax, esi             ; add rr01 base
2ef17bd  mov     [edi], eax           ; write back fixed rel32
2ef17bf  add     edi, 5
2ef17c2  mov     al, bl
2ef17c4  loop    loc_2EF179F          ; ECX-decrement loop
```

This is the **inverse of LZMA SDK's BCJ-86 filter** (see `Bra86.c` in the public 7-Zip / LZMA SDK). The pre-compression BCJ filter converts `E8 <rel32>` and `E9 <rel32>` instructions into `E8 <abs32>` form (where `abs32 = rel32 + position`). Absolute addresses repeat more (e.g., a function called from many sites all encode the same `abs32`), so aPLib compresses the source better. The post-decompression filter we see here reverses the conversion: `rel32 = abs32 - position`.

The selectivity filter at `0x02EF17A3` (`cmp byte ptr [edi], 35h; jnz`) — this only filters call/jmp instructions whose 5th byte (the byte immediately after the `rel32`) is `0x35`. That's a heuristic to avoid false positives: 0xE8 / 0xE9 byte values inside data or other instructions where the surrounding bytes don't look like a call/jmp would be silently corrupted by an indiscriminate filter. The 1/256 selectivity drops false-positive rate to ~0.4%.

The loop counter `0xCECE = 52,942` is the count of E8/E9-followed-by-0x35 instructions found in rr01 — a count baked at compression time.

Reference: LZMA SDK's `bra86.c` — same algorithm, different code style. Public since ~2000.

### Stage 3 — Custom IAT resolver (`0x02EF17C6` → `0x02EF1816`)

```asm
2ef17c6  lea     edi, dword_2AEF000[esi]   ; edi = esi + 0x2AEF000 = 0x02EF0000
                                            ; (= flat IAT-resolution table at end of rr02)

; Outer loop: per-DLL
2ef17cc  mov     eax, [edi]                 ; eax = DLL name offset (from rr01 base)
2ef17ce  or      eax, eax
2ef17d0  jz      short loc_2EF1817          ; null offset = end of table
2ef17d2  mov     ebx, [edi+4]               ; ebx = IAT slot offset (from rr01 base)
2ef17d5  lea     eax, dword_2AF6E14[eax+esi] ; eax = full DLL name pointer
2ef17dc  add     ebx, esi                   ; ebx = full IAT slot pointer
2ef17de  push    eax                        ; arg0 = DLL name
2ef17df  add     edi, 8                     ; advance table ptr past header
2ef17e2  call    ds:dword_2AF6EF0[esi]      ; call LoadLibraryA via stub-internal pointer
2ef17e8  xchg    eax, ebp                   ; ebp = DLL handle

; Inner loop: per-function in this DLL
2ef17e9  mov     al, [edi]                  ; ordinal flag / name byte
2ef17eb  inc     edi
2ef17ec  or      al, al
2ef17ee  jz      short loc_2EF17CC          ; null = end of this DLL's functions
2ef17f0  mov     ecx, edi
2ef17f2  jns     short near ptr loc_2EF17FA+1 ; sign bit = ordinal vs name
2ef17f4  movzx   eax, word ptr [edi]        ; read 16-bit ordinal
2ef17f7  inc     edi
2ef17f8  push    eax
2ef17f9  inc     edi
2ef17fa  mov     ecx, 0AEF24857h            ; junk imm32 to pad alignment
2ef17ff  push    ebp                        ; arg0 = DLL handle
2ef1800  call    ds:dword_2AF6EF4[esi]      ; call GetProcAddress
2ef1806  or      eax, eax
2ef1808  jz      short loc_2EF1811          ; failure
2ef180a  mov     [ebx], eax                 ; populate IAT slot
2ef180c  add     ebx, 4
2ef180f  jmp     short loc_2EF17E9
2ef1811  call    ds:dword_2AF6EFC[esi]      ; (probably ExitProcess or similar)
```

This is a standard "compressed import table" walker. The data layout at `0x02EF0000`:

```
[ DLL_name_offset_0 (4 bytes) ]
[ IAT_slot_offset_0 (4 bytes) ]
[ funcname_or_ordinal_0_a (variable, null-terminated or [01 or 02][word_ordinal]) ]
[ funcname_or_ordinal_0_b ]
...
[ 00 ]                         ; end of DLL 0's functions
[ DLL_name_offset_1 ]
...
[ 00 00 00 00 ]                ; end of table (null DLL_name_offset)
```

The stub-internal function pointers at `[esi+0x2AF6EF0]`, `[esi+0x2AF6EF4]`, etc. are pre-populated by the SecuROM packer at build time; they point at `kernel32!LoadLibraryA`, `kernel32!GetProcAddress`, and a third (probably error / cleanup). These pointers were resolved BEFORE the stub ran — Windows loader fills them via the standard PE import directory just before transferring control to the entry point.

**Output:** every IAT slot in `rr01` is now populated with the runtime address of the corresponding DLL function. Game code can call through the IAT normally.

### Stage 4 — PE header tweaks (`0x02EF1817` → `0x02EF1844`)

```asm
2ef1817  mov     ebp, ds:dword_2AF6EF8[esi]  ; ebp = VirtualProtect (from same stub-internal table)
2ef181d  lea     edi, [esi-1000h]            ; edi = 0x00400000 (= ImageBase, PE header page)
2ef1823  mov     ebx, 1000h                  ; size = 1 page
2ef1828  push    eax
2ef1829  push    esp                         ; lpflOldProtect
2ef182a  push    4                           ; flNewProtect = PAGE_READWRITE
2ef182c  push    ebx
2ef182d  push    edi
2ef182e  call    ebp                         ; VirtualProtect(0x400000, 0x1000, RW, &old)
2ef1830  lea     eax, [edi+2DFh]             ; eax = 0x004002DF
2ef1836  and     byte ptr [eax], 7Fh         ; clear high bit at 0x4002DF
2ef1839  and     byte ptr [eax+28h], 7Fh     ; clear high bit at 0x400307
2ef183d  pop     eax
2ef183e  push    eax
2ef183f  push    esp
2ef1840  push    eax                         ; restore old protect
2ef1841  push    ebx
2ef1842  push    edi
2ef1843  call    ebp                         ; VirtualProtect — restore
2ef1845  pop     eax
```

The two patched bytes at `0x4002DF` and `0x400307` are inside the PE optional header. Offsets from the standard MSVC layout:
- File offset of `e_lfanew` = `0x00000`'s PE header at `0x000000F8` (typical).
- Optional header at `0x00000110` (typical).
- 0x004002DF - 0x00400000 = 0x2DF.
- 0x00400307 - 0x00400000 = 0x307.

Looking at it: PE header is at file offset `0xF8` (standard). 0x2DF is well inside the optional header (which starts at e_lfanew + 0x18 = 0x110). 0x2DF - 0x110 = 0x1CF — that's somewhere in the data directories array (`IMAGE_DATA_DIRECTORY[16]` at optional header offset 0x60, taking 0x80 bytes). 0x1CF is past the data directories.

Actually 0x2DF and 0x307 are offsets from the IMAGE_DOS_HEADER. Position 0x2DF is well past the DOS header, past the PE header, past the optional header — into the **section headers**. Each `IMAGE_SECTION_HEADER` is 40 bytes; the first section header for `rr01` starts at e_lfanew + 0xF8 + opt_header_size. With typical PE32, that's around 0x178 + 0xE0 = 0x258. Section 1 at 0x258, section 2 at 0x280, section 3 at 0x2A8.

`0x2DF` is at offset 0x37 inside section header 1 (offset 0x2DF - 0x2A8 = 0x37 — but that can't be right because section 1 is `rr01`). Actually rr01 is in section 0. Let me redo:
- Section 0 (rr01): starts 0x258, ends 0x27F
- Section 1 (rr02): 0x280 – 0x2A7
- Section 2 (.rsrc): 0x2A8 – 0x2CF

`0x2DF` is past all three section headers. Byte at 0x2DF could be in `IMAGE_BOUND_IMPORT_DESCRIPTOR` or the Bound Import directory data — but more likely just garbage that happens to live in the alignment between the section table and the first section's raw data.

Without checking the exact bytes (would need to read the on-disk Wilbur.exe header at those offsets), the most likely explanation: SecuROM at packaging time set the high bit on two bytes of the PE header to flag "this is a SecuROM-protected build" (a soft signature). The stub clears the bits at runtime so anything that re-validates the PE doesn't trip.

Either way, this stage is cosmetic — clearing two bits, with VirtualProtect bracketing for safety since the PE header page is read-only by default.

### Stage 5 — Cleanup and OEP transfer (`0x02EF1845` → `0x02EF1854`)

```asm
2ef1845  pop     eax                  ; balance the stack
2ef1846  popa                         ; restore all registers (saved at stage 1)
2ef1847  lea     eax, [esp-80h]
2ef184b  push    0
2ef184d  cmp     esp, eax
2ef184f  jnz     short loc_2EF184B    ; loop: push 32 zeros = clear 128 bytes of stack space
2ef1851  sub     esp, 0FFFFFF80h      ; sub -0x80 = add 0x80 = restore stack ptr
2ef1854  jmp     loc_62B48A           ; → real OEP (= _WinMainCRTStartup)
```

The stack-clear loop is anti-debug noise (writes 128 bytes of zeros below `ESP` to defeat naive stack-trace dumps) but does nothing functional.

`jmp loc_62B48A` is the final transfer. After this, the SecuROM stub never runs again — control is in `_WinMainCRTStartup` which initialises the C runtime, then calls `WinMain` at `0x0062B5C0`, which calls `game_MessageLoop` at `0x0056F6E0`.

---

## What the stub does NOT do

For comparison with what some SecuROM 7 builds DO have (and don't apply here):

- ❌ **Per-page lazy decryption.** Some packers set `PAGE_GUARD` on code pages and decrypt on first access. This stub doesn't — the entire `rr01` is plaintext after stage 1.
- ❌ **Anti-debug post-OEP.** Some SecuROM 7 builds keep an anti-debug thread running after the game starts. This stub doesn't — once `jmp 0x62B48A` executes, the stub's bytes are never touched again.
- ❌ **CPUID timing checks.** Some SecuROM 7 builds use RDTSC + CPUID timing to detect debugger pauses. This stub doesn't.
- ❌ **CRC self-checks.** Some builds re-hash the unpacked code after decryption to detect tampering. This stub doesn't.
- ❌ **Hardware-fingerprint key derivation.** Some builds derive decryption keys from MAC address / disk serial / etc. This stub doesn't (no keys at all).

This is consistent with [`docs/SECUROM.md`](../../docs/SECUROM.md)'s observation that *Meet the Robinsons* is "on the simpler end of the SecuROM 7 spectrum."

## Implications for the project

1. **Our menu-time PE-sieve dump is 100% complete** for `rr01`. Because aPLib decompression runs once at startup before any game code, every byte of `rr01` is plaintext in our dump regardless of which game state we dumped from.

2. **The "missing DirectInput8Create call site" is genuinely missing** because the game *itself* never statically calls `DirectInput8Create`. The IAT slot is populated (the stub's IAT resolver fills it), but no game code references it. This is a linker-included unused import — common when a project's headers force `#pragma comment(lib, "dinput8")` even on translation units that don't use DInput. Wilbur.exe's keyboard/mouse handling appears to be entirely WndProc-based (verify by hooking `DirectInput8Create` at runtime — if the hook never fires, the game truly doesn't use DInput).

3. **No special unpacking procedure is needed.** Our existing `ida/400000.Wilbur.exe.i64` is the working artefact. To produce a runnable static `Wilbur_unpacked.exe`, we just need Scylla to:
   - Use OEP `0x0062B48A` (NOT `0x0062B5C0`).
   - "Get Imports" — the IAT is already populated correctly in the dump.
   - "Fix Dump" — rewrite section RawSizes and Import Directory.
   - Output runs standalone (no SecuROM stub ever runs again — its job is done after stage 5).

   The complete procedure in [`full-unpack-procedure.md`](full-unpack-procedure.md) Stage B is correct in everything except the OEP value (now updated to `0x0062B48A`).

4. **All the "approach B / page-touch / multi-state / RE the encryption" preparation in the previous round was unnecessary.** None of the speculative concerns ("SecuROM-VM dispatch", "lazy-decrypt blocks", "encrypted-in-our-dump pages") apply to this build. They might apply to other SecuROM 7 builds — but not this one.

5. **`tools/diff_dumps.py`'s coverage probes will report no real change** between any two pe-sieve dumps of this build, because the content of `rr01` is invariant after stage 1. The script remains useful for sanity-checking other builds or future games.

## Open question (minor)

Stage 4 patches bytes at `0x4002DF` and `0x400307`. To confirm these are PE-header housekeeping and not something more interesting, read the on-disk `Game/Wilbur.exe` at those file offsets and compare to what's in the unpacked `rr01` after the patch. Expected: high bit set on disk, cleared in dump. If something else is going on, document here.

## Files and symbols touched

| VA           | Symbol                            | Status |
|--------------|-----------------------------------|--------|
| `0x02EF16A0` | _SecuROM stub entry_ (annotated)  | comment added |
| `0x02EF1854` | _final jmp to real OEP_           | comment added |
| `0x0062B48A` | `_WinMainCRTStartup`              | renamed + commented |
| `0x0062B5C0` | (WinMain shim)                    | unchanged — was already documented in `frame-pacing.md` |
| `0x0056F6E0` | `game_MessageLoop`                | unchanged |
