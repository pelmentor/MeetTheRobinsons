# Unpacking state — completed (M1 done)

> Single source of truth for "what we did to get a clean disassembly".
> Recorded 2026-05-04 to survive context compacting.

## Final method that worked

**PE-sieve standalone** (no debugger, no x32dbg, no ScyllaHide) against a normally-running Wilbur.exe.

Tried-and-failed paths (do NOT revisit unless forced):
- ❌ x32dbg + bundled Scylla + ScyllaHide ZIP — MCP plugin connection went flaky mid-trick
- ❌ ScyllaHide for IDA 9.x (TKazer fork) — NULL deref crash inside HookLibrary itself

## Reproducible procedure

1. Start Wilbur.exe normally (double-click).
2. **For the most complete dump:** play in-game past `Press Start` into a 3D scene with active gameplay (mouse-look, character control, shaders running, etc.). The current dump (`process_22276/`) was taken at the main menu — most of the engine is recovered, but some code paths that only run during real gameplay (notably DirectInput init, some lazy shader paths) are absent. SecuROM 7 lazily decrypts certain protected blocks on first execution; un-triggered blocks aren't in the dump as plaintext bytes. See [lessons-learned.md L2](lessons-learned.md) for evidence and impact.
3. Find PID via Task Manager → Details.
4. Run pe-sieve with **restrictive imports mode** (key flag: `/imp 3`):

```powershell
& "D:\Projects\Programming\MeetTheRobinsons\pe-sieve.exe" `
    /pid <PID> /imp 3 /shellc 0 `
    /dir "D:\Projects\Programming\MeetTheRobinsons\ida\dumps"
```

`/imp 3` = R0 mode (only terminated IAT blocks). Without it, `/imp 1` autodetect produces 70%+ wrong API guesses on SecuROM stolen-byte wrappers.

## Output

- `ida/dumps/process_<PID>/400000.Wilbur.exe` — 45 MB unpacked PE.
- `ida/dumps/process_<PID>/400000.Wilbur.exe.imports.txt` — full IAT scan log (informational).
- `ida/dumps/process_<PID>/dump_report.json`, `scan_report.json` — pe-sieve metadata.

## State of the dumped binary

| Property | Value |
|---|---|
| ImageBase | `0x00400000` |
| Sections | `rr01` (code, 0x024F8000 raw), `rr02` (data, 0x005F9000), `.rsrc` |
| EntryPoint | `0x02EF16A0` (still SecuROM stub — irrelevant for IDA analysis) |
| IDA database | `D:\Projects\Programming\MeetTheRobinsons\ida\400000.Wilbur.exe.i64` |
| Functions found by IDA auto-analysis | **12,555** |
| Hex-Rays | works |
| Strings cache | 21,551 entries |

## Address invariants (critical for the ASI mod)

- ImageBase **never moves** at runtime (no DYNAMICBASE flag — 2007 PE32, no ASLR).
- SecuROM unpacking is **deterministic**: same code always lands at same VAs.
- **VAs in the IDA database === VAs in the live process** — mod hooks at fixed addresses work without rebasing.
- The mod hooks the live (still-protected) `Game/Wilbur.exe` via Ultimate ASI Loader; SecuROM stub has finished by the time the mod's `DllMain` runs.

## Wrapper noise we live with

The dumped PE has 27 import descriptors. First 10 (advapi32, dinput8, dsound, gdi32, kernel32, user32, winmm, ws2_32, binkw32, d3d8) live on the consolidated **main IAT at `0x6A6000`** — these are real and complete.

Entries 10–26 are SecuROM stolen-byte wrappers scattered at addresses like `0x744CE4`, `0x745154`, `0xF5F83A..`, `0x2AF7EF0..`. They appear in IDA's import list but **most call sites in real game code go through the main IAT**. Wrapper imports show up only in code paths SecuROM specifically tampered with.

Treat any reference to wrapper addresses as suspicious; cross-check with main IAT.

## Game tech confirmed

- **DirectX 8** (not 9 — earlier docs incorrectly assumed D3D9).
- **DirectInput8**, **DirectSound**, **Bink Video** (`binkw32.dll`).
- **ws2_32** present — possibly for online features / multiplayer / debug telemetry; verify.

## Dump completeness (verified via stub RE 2026-05-05)

The current PE-sieve dump (`process_22276/`, taken at main menu) is **complete** for `rr01`. Verification: full RE of the SecuROM stub at `0x02EF16A0` ([`securom7-stub-re.md`](securom7-stub-re.md)) showed it's just aPLib decompression + BCJ-86 byte filter + custom IAT resolver. No per-page lazy decryption, no encryption at all. By the time any game code runs, the entire `rr01` section is plaintext — and our pe-sieve dump captures it.

The earlier "Known incompleteness" claim (that DirectInput init code was missing because lazy-decrypted) turned out to be wrong. The DInput call site is missing because **the game doesn't actually call `DirectInput8Create`** — it's a linker-included unused import. Verified specifically: zero references to the IAT slot at `0x6A6020` from any code path in `rr01`. (Wilbur.exe's keyboard/mouse handling is WndProc-based; only `Launcher.exe` uses DirectInput, for keymap probing.)

For the menu-time dump (`process_22276/`):
- 12,555 functions — bulk of the engine, fully usable.
- The dump is the working artefact; no re-dumping needed.

**Stage A redump procedures (page-touch, multi-state, TinyTracer) in [`full-unpack-procedure.md`](full-unpack-procedure.md) are over-engineered for problems that don't exist on this build.** They remain documented for use on other (more heavily protected) SecuROM 7 builds, but are not the right path for Meet the Robinsons. Skip directly to Stage B (Scylla rebuild) using the existing `process_22276/` dump.
