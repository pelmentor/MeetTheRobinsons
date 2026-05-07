# `tools/` — utility scripts

All scripts are pure Python (3.11+) using only stdlib + `ctypes` for Windows API calls. No external dependencies. Invoke via `python tools/<name>.py [args]`.

For day-to-day playing/dev: just double-click [`Game/run.bat`](../Game/run.bat). It dispatches to `run_game.py`.

## Inventory

### `run_game.py` — clean state + deploy + launch

Single entry point for the dev cycle. Replaces the old PowerShell `reset-deploy.ps1`.

```
python tools/run_game.py                # default: clean + deploy + launch Wilbur.exe directly
python tools/run_game.py --launcher     # clean + deploy + launch via Launcher.exe
python tools/run_game.py --no-launch    # clean + deploy only
python tools/run_game.py --clean        # clean state, no deploy or launch
```

What "clean state" does:

1. `TerminateProcess` any live `Wilbur.exe` / `Launcher.exe`.
2. Find any process holding the `Global\Disney_s_Meet_The_Robinsons` mutex (via `find_mutex_holder.py` logic) and `TerminateProcess` it. This kills lingering zombies that block subsequent launches.
3. Free the locked `Game\mtr-asi.asi` (rename-trick: move to `*.zombie-<timestamp>`).

Deploy step copies `src/mtr-asi/build/Release/mtr-asi.asi` to `Game/mtr-asi.asi`.

Direct-launch mode invokes `Wilbur.exe -dxfullscreen -dxadapter=0 -launchit`. The mod's cmdline hook injects `-dxresolution=NATIVE` (no `-dxresolution` arg → injected at append). Bypasses Launcher.exe — useful when the launcher's mutex check trips on a zombie that survives our clean-up.

### `find_mutex_holder.py` — name → process holding it

Python rewrite of an earlier failed PowerShell version. Walks `NtQuerySystemInformation(SystemExtendedHandleInformation)`, filters to handles whose `ObjectTypeIndex` matches the Mutant kernel type (probed at startup by creating our own mutex and locating it in the table), then duplicates each handle and resolves its `ObjectNameInformation`. Per-handle `NtQueryObject` runs in a worker thread with a 200 ms timeout to avoid hanging on uncooperative kernel objects (e.g. ALPC ports).

```
python tools/find_mutex_holder.py                          # default: Disney_s_Meet_The_Robinsons
python tools/find_mutex_holder.py "MyOtherMutex"           # arbitrary substring
```

Reports each holder process by PID, image path, and the full kernel object path. Used by `run_game.py`. Standalone for diagnosis when something else is holding a named mutex.

### `find_aspect_rva.py` — locate aspect floats in unpacked Wilbur.exe

Searches for `AB AA AA 3F` (= `1.333333f` little-endian, the 4:3 constant) inside the PE-sieve-unpacked dump at `ida/dumps/process_*/400000.Wilbur.exe`. Reports each match with its file offset, section, RVA, and runtime VA (computed assuming `ImageBase = 0x400000`, no ASLR).

```
python tools/find_aspect_rva.py
```

This is how the aspect-ratio constant `0x6C750C` (and the four orphan candidates) were originally found. See [research/findings/aspect-ratio-fix.md](../research/findings/aspect-ratio-fix.md) for the full investigation.

### `patch_aspect.py` — disk-patch fallback (NOT primary)

WSGF guide says to hex-edit `AB AA AA 3F` → `39 8E E3 3F` at file offset `0x8EFEE` in retail Wilbur.exe. **On this build that doesn't work** — that file location is inside the SecuROM-encrypted `rr02` section, and the runtime bytes don't match. Kept around in case a future build (or a non-DRM re-release) makes it relevant.

```
python tools/patch_aspect.py                       # auto-detect monitor aspect
python tools/patch_aspect.py --aspect 16:9         # explicit ratio
python tools/patch_aspect.py --aspect 2.388889     # explicit float
python tools/patch_aspect.py --restore             # restore from .bak
python tools/patch_aspect.py --check               # report current state of the EXE
```

Always backs up to `Wilbur.exe.bak` before patching. The actual aspect-ratio fix lives in the mod's `aspect_patch.cpp` / `d3d9_hook.cpp`, not here.

### `build_standalone_exe.py` — turn the PE-sieve dump into a runnable standalone EXE

Reads `ida/dumps/process_22276/400000.Wilbur.exe`, applies a 4-byte patch to `AddressOfEntryPoint` (changes it from the SecuROM stub at `0x02EF16A0` to the real game OEP at `0x0062B48A` = `_WinMainCRTStartup`), mirrors the high-bit clears the stub does at runtime, and writes `ida/Wilbur_unpacked.exe`. Output is gitignored (analysis-only).

This replaces the Scylla GUI workflow for this build of Wilbur.exe. The SecuROM stub here is just aPLib compression + BCJ filter + IAT resolver — pe-sieve already does most of what Scylla "Fix Dump" does (sections, import directory). Only the OEP needs flipping.

```
python tools/build_standalone_exe.py                           # default: in→ ida/Wilbur_unpacked.exe
python tools/build_standalone_exe.py --in <dump> --out <exe>   # explicit paths
python tools/build_standalone_exe.py --zero-stub               # also wipe the obsolete stub region
```

Verify the result with `verify_unpacked_pe.py`.

### `inspect_pe.py` — dump the PE headers of any image

Quick PE inspector. Prints DOS header, COFF header, optional header (including AddressOfEntryPoint), data directories, and section table. Used during stub RE / verification work; useful for any 32-bit PE.

```
python tools/inspect_pe.py ida/Wilbur_unpacked.exe
python tools/inspect_pe.py ida/dumps/process_22276/400000.Wilbur.exe
```

### `scan_strings.py` — string-level sanity comparison between two PEs

Side-by-side ASCII + cp1251 string scan. Used to verify that the unpacked EXE actually contains decompressed plaintext (not just SecuROM-compressed bytes that happen to look printable). Run against `Game/Wilbur.exe` (protected) and `ida/Wilbur_unpacked.exe` to see the dramatic difference: 88k garbage strings vs 162k real strings.

```
python tools/scan_strings.py Game/Wilbur.exe ida/Wilbur_unpacked.exe
```

Output categorises strings into debug markers, asserts, format strings, source file paths, and cyrillic content.

### `hunt_debug.py` — first-pass debug-feature hunter

Scans an unpacked PE for command-line flags, cheat keywords, debug markers, console hints, and identifier-shaped strings. First-pass tool used to discover the cheat unlock system and the `-letitsnow` Easter egg. See [research/findings/debug-features.md](../research/findings/debug-features.md) for the full catalog.

```
python tools/hunt_debug.py ida/Wilbur_unpacked.exe
```

### `hunt_debug_focus.py` — targeted drill-downs

Companion to `hunt_debug.py` but with category-specific regex filters: FreeCam, FirstPerson, Teleport, Verbosity, Cheats, etc. Use after `hunt_debug.py` flags an interesting category to surface only the relevant strings.

```
python tools/hunt_debug_focus.py ida/Wilbur_unpacked.exe
```

### `diff_dumps.py` — compare two PE-sieve dumps

Reports byte regions that differ between an old (less-complete) dump and a new (more-complete) one. Used in Stage A.4 of the [full-unpack procedure](../research/findings/full-unpack-procedure.md) to verify a fresh in-game dump captured previously-encrypted code paths.

Includes coverage probes for known-missing markers (e.g. `FF 15 20 60 6A 00` = `call dword ptr [DirectInput8Create]`). When that probe flips from "0 occurrences" to "N occurrences", DirectInput init has been recovered.

```
python tools/diff_dumps.py ida/dumps/process_22276/400000.Wilbur.exe \
                           ida/dumps/full/process_<new-pid>/400000.Wilbur.exe
```

### `verify_unpacked_pe.py` — sanity-check a Scylla-rebuilt EXE

Runs PE-structure checks on a freshly-unpacked `Wilbur_unpacked.exe` before launching it. Catches common Scylla pitfalls (wrong OEP, missing import descriptors, executable section with `RawSize=0`) without requiring a crash to learn about them.

```
python tools/verify_unpacked_pe.py ida/Wilbur_unpacked.exe
```

Used in Stage B.5 of the full-unpack procedure.

### `skip_intros.py` — bypass logo intros

Renames the five logo `.BIK` files (`BinkLegal`, `legal`, `dsny`, `avlogo`, `bvg`) to `*.intro_skip`. The game's Bink open call fails for those names; it proceeds to the main menu without crashing. In-game cutscenes (egy, sti, dfi, end, credits, etc.) are NOT touched.

```
python tools/skip_intros.py             # rename to .intro_skip
python tools/skip_intros.py --restore   # restore originals
python tools/skip_intros.py --check     # report state
```

This is a runtime sledgehammer — works because Bink is permissive about open failures. The "proper" alternative — hooking `BinkOpen` in `binkw32.dll` and short-circuiting only known intro filenames — has been designed (see [research/findings/bink-integration.md](../research/findings/bink-integration.md)) but not yet implemented. Once implemented in the mod, this script becomes legacy / fallback.

## Adding new tools

Conventions:

- Pure Python stdlib + `ctypes`. No `pip install` step.
- Single-file scripts under `tools/`. Use a top-of-file docstring documenting the script's job, args, and any prerequisites (e.g. "needs IDA database open").
- Windows-API access via `ctypes.WinDLL(...)`. Don't take dependencies on `pywin32`, `psutil`, etc.
- Output formatting: ANSI colour escapes (the runtime enables VT processing on the console handle); plain text fallback if colours fail.
- Anything destructive (file writes, kills, registry edits) requires a `--check` or `--restore` complement so we can dry-run / undo.
