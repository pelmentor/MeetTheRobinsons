# Full unpack procedure — produce a static `Wilbur_unpacked.exe` for analysis

**Goal:** rebuild a runnable, statically-loadable copy of `Wilbur.exe` with the SecuROM stub removed, so IDA loads it directly without needing to dump from a live process. Produces the most complete disassembly achievable on this build.

**Scope:** local analysis only. The output (`ida/Wilbur_unpacked.exe`) is gitignored; it must never be committed to this or any public repo. Project licence and `docs/ROADMAP.md` "Out of scope" both prohibit redistribution of unpacked binaries. This procedure is for reverse-engineering the user's legally-owned retail copy on the user's own machine, the same way `tools/skip_intros.py` modifies the user's own files.

> **2026-05-05 simplification.** The SecuROM stub on this build of Wilbur.exe was reverse-engineered in [`securom7-stub-re.md`](securom7-stub-re.md). It is **just aPLib compression + BCJ-86 byte filter + custom IAT resolver** — no real encryption, no lazy decryption, no anti-debug post-OEP. Our existing menu-time PE-sieve dump already contains 100% of `rr01` plaintext. Stages A.1–A.6 below (the page-touch design and the multi-state fallback) were over-engineered for problems that don't exist on this build. **Skip directly to Stage B** — Scylla rebuild — using the existing dump. Stage A is preserved for documentation / use on other (more heavily protected) SecuROM 7 builds.
>
> **Updated OEP**: **`0x0062B48A`** (= `_WinMainCRTStartup`).
>
> **2026-05-05, second simplification.** Scylla GUI work isn't even needed. PE-sieve already produced a structurally-correct rebuilt PE (sections with right RawSizes, valid Import Directory). The ONLY thing wrong is the AddressOfEntryPoint, which still points at the SecuROM stub. A 4-byte patch is enough. Use [`tools/build_standalone_exe.py`](../../tools/build_standalone_exe.py) — it parses the dump's PE headers, rewrites AddressOfEntryPoint, mirrors the high-bit clears the stub does at runtime (no-op if pe-sieve already captured them cleared), and saves to `ida/Wilbur_unpacked.exe`. ~30 seconds, no GUI, fully scriptable. **Stage B (Scylla) is now also optional / educational** — see Stage B' below for the one-script path.

---

## Why "Option 3" over multi-state pe-sieve

A PE-sieve dump is a **memory snapshot** at a single moment. Multi-state dumping (running the game through every state, dumping repeatedly, merging) only captures **executed** code — hidden debug functions, dead branches, error paths that never run during normal play stay encrypted. To analyze the result, IDA has to parse a non-standard layout (zero RawSize on `rr01`, OEP inside the SecuROM stub).

A page-touch-decrypted, Scylla-rebuilt static EXE:
- Captures **all** encrypted code in one pass, including never-executed debug functions and dead branches (Stage A — see below).
- Loads in IDA without special handling — sections have correct RawSize, OEP points at the real game entry, import directory is reconstructed.
- Doesn't need the SecuROM stub at runtime — DVD check is gone, anti-debug is gone, lazy-decrypt machinery is gone.
- Encodes the entire decrypted snapshot in a single artefact you can re-analyze any time.
- One-time cost (the procedure below); permanent benefit.

The combined "page-touch + Scylla rebuild" produces the closest thing to "100% disassembly" that's possible on this binary — limited only by what code SecuROM physically encrypted (i.e. all of `rr01`, which is everything game-code).

---

## Pre-requisites

| Tool | Purpose | Source |
|---|---|---|
| **PE-sieve** (hasherezade) | Memory dump utility. Already in repo at `D:\Projects\Programming\MeetTheRobinsons\pe-sieve.exe`. | https://github.com/hasherezade/pe-sieve |
| **x64dbg + bundled Scylla** | PE rebuild tool. Use the Scylla *bundled with x64dbg* (Plugins → Scylla menu), NOT the standalone NtQuery/Scylla v0.9.8 (stale since 2014). | https://x64dbg.com |
| **PE-bear** (hasherezade) | Manual PE header inspection / fixup. Optional but useful when Scylla's "Fix Dump" misses something. | https://github.com/hasherezade/pe-bear |
| `mtr-asi.asi` with `MTR_FORCE_DECRYPT` enabled | Our own page-touch walker. Built from `src/mtr-asi/src/force_decrypt.cpp` (template in Stage A.1). Single-purpose; doesn't need anti-debug bypass tools. | this repo |
| **`Wilbur.exe`** | The user's legally-owned retail binary at `Game/Wilbur.exe`. SHA256 `269C8FE…0A50`. | — |

**Tools NOT needed for Stage A** (despite docs/SECUROM.md mentioning them historically):
- Intel PIN, TinyTracer — only needed if Stage A page-touch fails for an unusual reason and we have to fall back to dynamic-instrumentation tracing.
- ScyllaHide / TitanHide — anti-anti-debug. Page-touch is initiated from inside the running process via our own DLL; SecuROM's anti-debug never fires because there's no debugger.

---

## Stage A — Force-decrypt every page (page-touch technique)

**Goal:** decrypt **every** page in the SecuROM-protected range, including code that never normally executes (debug functions, error paths, dead branches). One pass; no multi-state playthrough.

### Why page-touch beats multi-state dumping

SecuROM 7 protects code pages with `PAGE_GUARD` (or equivalent first-access trap). The trap fires on **any** access — read, write, or execute. Reading a single byte from a guarded page triggers the SecuROM page-fault handler, which decrypts the page and replaces the guard with normal `PAGE_EXECUTE_READ`. Once decrypted, the page stays decrypted for the rest of the process lifetime.

So: if we read one byte from every 4 KB page in `rr01`, SecuROM decrypts every page. No need to drive game code through every state. Hidden debug menus, dev paths, error handlers — all decrypted, all dumpable. This is a known unpacking technique sometimes called *Code Page Touch* or *Memory Walk*; it isn't DRM circumvention, just provoking the legitimate page-fault handler to do its job for every page rather than only on natural execution.

### A.1 — Build the force-decrypt module

Compile this as `force_decrypt.asi` (or fold into `mtr-asi` behind a config flag). Put it in `Game/` next to `mtr-asi.asi`.

```cpp
// force_decrypt.cpp — single-shot SecuROM page-decrypt walker.
// Place at src/mtr-asi/src/force_decrypt.cpp; install hook from dllmain.cpp
// behind a DEBUG_FORCE_DECRYPT compile flag.

#include <Windows.h>
#include "mtr/log.h"

namespace mtr::force_decrypt {

namespace {
    constexpr SIZE_T kRr01Base = 0x00401000;
    constexpr SIZE_T kRr01Len  = 0x024F8000;   // VirtualSize of rr01
    constexpr SIZE_T kPageSize = 0x1000;

    DWORD WINAPI walker(LPVOID) {
        // Wait for SecuROM stub to finish initial setup.
        // 8 sec is generous; once we've reached main menu, the stub is done.
        Sleep(8000);

        BYTE* base = reinterpret_cast<BYTE*>(kRr01Base);
        volatile BYTE sink = 0;
        SIZE_T touched = 0;
        SIZE_T errors  = 0;

        MTR_LOG("force_decrypt: walking %u pages from 0x%p",
                (unsigned)(kRr01Len / kPageSize), base);

        for (SIZE_T off = 0; off < kRr01Len; off += kPageSize) {
            __try {
                sink ^= base[off];   // one read per page = full page decrypt
                ++touched;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                ++errors;            // page not committed; skip
            }

            // anti-anti-dump: don't burn through 9700 pages in milliseconds.
            // 1ms every 256 pages = ~38 ms total overhead, but lets SecuROM's
            // book-keeping settle if any per-page state matters.
            if ((off & 0xFFFFF) == 0) Sleep(1);
        }

        MTR_LOG("force_decrypt: done — %u touched, %u errors. Now safe to pe-sieve.",
                (unsigned)touched, (unsigned)errors);
        return 0;
    }
}

void install() {
    HANDLE h = CreateThread(nullptr, 0, walker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

} // namespace mtr::force_decrypt
```

Wire from `dllmain.cpp` immediately after `MH_Initialize` (or behind a `#ifdef MTR_FORCE_DECRYPT` flag if you don't want it always-on).

The cleanest packaging: keep it in `mtr-asi` so the existing dxwrapper-based ASI loader picks it up; toggle the walker thread on/off with an environment variable (`MTR_FORCE_DECRYPT=1`) so production builds don't waste 38 KB of reads at startup.

### A.2 — Run with force-decrypt enabled

```powershell
# Make sure mtr-asi is built with MTR_FORCE_DECRYPT defined (or env-var path):
$env:MTR_FORCE_DECRYPT = "1"

# Launch the game normally — main menu is enough; you don't need to play.
.\Game\run.bat
```

Wait until `Game\mtr-asi.log` shows:

```
force_decrypt: walking 9464 pages from 0x00401000
force_decrypt: done — 9464 touched, 0 errors. Now safe to pe-sieve.
```

(Errors > 0 means some pages weren't committed by SecuROM at startup — usually fine; a handful of trailing pages aren't real. If errors > a few hundred, something's wrong; investigate.)

### A.3 — Dump

In another terminal, while the game is still running on the main menu:

```powershell
Get-Process Wilbur | Select Id

& "D:\Projects\Programming\MeetTheRobinsons\pe-sieve.exe" `
    /pid <PID> /imp 3 /shellc 0 `
    /dir "D:\Projects\Programming\MeetTheRobinsons\ida\dumps\full"
```

Output: `ida/dumps/full/process_<pid>/400000.Wilbur.exe`. This dump contains every decrypted byte of `rr01` — including code that never naturally executes.

Exit the game cleanly.

### A.4 — Verify coverage gain over the menu-time dump

```powershell
python tools/diff_dumps.py `
    ida/dumps/process_22276/400000.Wilbur.exe `
    ida/dumps/full/process_<new-pid>/400000.Wilbur.exe
```

Expected:
- `differing bytes (total)` should be ON THE ORDER of tens of MB (most of `rr01` was encrypted in the menu-time dump).
- Coverage probe `call dword ptr [DirectInput8Create]` flips from 0 to N>0.
- Other coverage probes flip too.
- `code-shaped regions` should be in the thousands.

If the diff is small (< 1 MB), force-decrypt didn't actually decrypt — fall back to Stage A.5 (multi-state, the old approach).

### A.5 — Fallback: multi-state pe-sieve  *(only if A.1–A.4 produce a near-empty diff)*

The old approach. Run game through every reachable state, dump multiple times, take the union. Ugly but guaranteed to capture executed code paths. **Loses code that never executes** — debug functions, dead branches — which is exactly why we tried page-touch first.

### A.6 — If page-touch fails outright (returns garbage instead of decrypted code)

Some advanced SecuROM 7 builds use **EIP-keyed decryption** — page decryption derives a key from the executing instruction pointer, not just the page address. Reading from outside the executing context yields plausible-looking but wrong bytes.

For Meet the Robinsons (2007 retail, simpler end of SecuROM 7) this is unlikely, but possible. Symptom: A.4 reports lots of differing bytes, but the new bytes don't disassemble as valid x86 (they're random-looking). In that case, fall back to Stage A.5.

The "heavy artillery" alternative is to RE the SecuROM stub's decryption routine in `rr02` (it's already in our dump) and run it offline against the encrypted `rr01` bytes. Days of work, not hours; document at this point but don't attempt without strong reason.

---

## (Removed) Old Stage A — multi-state dump

Previously this was the recommended Stage A. It's now A.5 (fallback) because page-touch reliably gets *all* encrypted code in one pass, including never-executed debug paths. Multi-state dumping only captures executed paths.

---

## Stage B' — PE rebuild via Python script (recommended path)

```powershell
python tools/build_standalone_exe.py
python tools/verify_unpacked_pe.py ida/Wilbur_unpacked.exe
```

That's it. The first script:

1. Reads `ida/dumps/process_22276/400000.Wilbur.exe` (the existing PE-sieve dump).
2. Parses the PE headers to locate `AddressOfEntryPoint` (file offset `0x1E8` for this PE32 layout).
3. Overwrites the dword from `0x02AF16A0` (RVA of the SecuROM stub entry) to `0x0022B48A` (RVA of `_WinMainCRTStartup`).
4. Mirrors the stub's runtime byte-clears at file offsets `0x2DF` and `0x307` (no-op if pe-sieve already captured them cleared).
5. Writes `ida/Wilbur_unpacked.exe` (~43 MB, gitignored).

The second script runs structural sanity checks: PE32 magic, ImageBase, OEP value, section RawSizes, presence of all 10 main IAT DLLs, presence of known IAT slots. Output ends with `All checks passed. Try launching the unpacked binary.`

Total runtime: <30 seconds. No GUI, no manual steps, no Scylla, no x64dbg. The ~4-byte diff between input and output:

```
0x000001E8: A0 16 AF 02 -> 8A B4 22 00   ; AddressOfEntryPoint
```

Scripts are in [`tools/`](../../tools/). Procedure is fully reproducible and re-runnable on any future PE-sieve dump (e.g. if you take a richer in-game dump for educational purposes — though the current menu-time dump is already complete per the stub RE).

### Why this works (and why Scylla GUI is overkill for this build)

PE-sieve does most of what Scylla "Fix Dump" does, automatically: section RawSizes are populated from VirtualSizes, the Import Directory at RVA `0x02AF9000` (size `0x1F5B`) is reconstructed by pe-sieve walking the runtime IAT, the import descriptors' DLL names + IAT RVAs are written, and section table entries are valid for static loading. The only thing left over from the SecuROM era is the SecuROM stub still being the OEP — which is one dword in the optional header.

(For more heavily protected SecuROM 7 builds where the stub re-encrypts code or the IAT walker leaves wrappers in the IAT slots, Scylla "Fix Dump" with manual import-tree review is still the right tool. For Meet the Robinsons it's not necessary.)

---

## Stage B (legacy) — PE rebuild via Scylla GUI

**Goal:** convert the in-memory decrypted image into a runnable, statically-loadable EXE.

This stage is GUI-driven (Scylla doesn't have a stable CLI). Plan: ~30–60 min, mostly waiting for IAT autosearch.

### B.1 — Attach x64dbg to the running process

(After Stage A.3 you should still have the game running. If not, relaunch under PIN per A.1, briefly play through the same states for partial coverage, and continue.)

1. Start `x32dbg.exe` (the 32-bit variant — Wilbur is i386).
2. **File → Attach** → select `Wilbur.exe`.
3. Pause execution (F12). x64dbg shows the current EIP somewhere inside the game's main loop — we don't care exactly where.

### B.2 — Open Scylla and configure

1. **Plugins → Scylla**. (If menu shows "ScyllaHide" but no "Scylla", you have only the anti-anti-debug component installed; install x64dbg's "Scylla" plugin or download the bundled version.)
2. **Process selector**: pick `Wilbur.exe`.
3. **OEP (Original Entry Point)**: enter **`0x0062B48A`** (= `_WinMainCRTStartup`).

   This is the address SecuROM's stub jumps to once decompression is complete (verified in [`securom7-stub-re.md`](securom7-stub-re.md) — the final `jmp loc_62B48A` at `0x02EF1854`). It's the standard MSVC `_WinMainCRTStartup` for this binary; the C runtime initialiser that eventually calls WinMain at `0x0062B5C0`. Setting OEP correctly is the single most important step; an incorrect OEP produces an EXE that crashes immediately. **Do not use `0x0062B5C0`** — that's the WinMain shim, called BY `_WinMainCRTStartup`, and starting the EXE there skips CRT init.

4. **IAT Autosearch** (button): Scylla scans for the import table. Should find it starting at `0x6A6000` (the main IAT we identified). Confirm the scan reports a populated import table with imports for `kernel32`, `user32`, `d3d8`, `dinput8`, `binkw32`, `dsound`, `winmm`, `gdi32`, `advapi32`, `ws2_32`.

### B.3 — Resolve imports cleanly

1. **Get Imports** (button): Scylla resolves each IAT slot to a DLL!Function name.
2. Inspect the import tree:
   - **Green entries**: cleanly resolved. ✓
   - **Yellow entries**: SecuROM stolen-byte wrappers — Scylla traced through to the real API but flagged for review. Right-click → **Trace via wrapper**. If trace succeeds, the entry goes green.
   - **Red entries**: unresolvable. Right-click → **Cut Thunk** to drop them. (Last resort. Expect 0–5 red entries on this build; SecuROM 7 is on the simpler end.)
3. **Show Invalid** + **Show Suspect** (toggles): expose any items Scylla auto-hid. Investigate; trace or cut.

### B.4 — Dump and Fix Dump

1. **Dump** (button): save the in-memory image. Output: `Wilbur_dumped.exe` (~45 MB). This is the raw memory snapshot — won't run yet because the IAT layout doesn't match what the Windows loader expects.
2. **Fix Dump** (button): point at `Wilbur_dumped.exe`. Scylla rebuilds the PE headers — section RawSizes, OEP, import directory (RVA + size in `IMAGE_DIRECTORY_ENTRY_IMPORT`) — using the import tree from B.3. Output: `Wilbur_dumped_SCY.exe`.
3. Rename: `Wilbur_dumped_SCY.exe` → `Wilbur_unpacked.exe`. Move to `ida/Wilbur_unpacked.exe` (gitignored).

### B.5 — Smoke test the unpacked binary

```powershell
# Backup the protected original
Copy-Item Game\Wilbur.exe Game\Wilbur_protected.exe -Force

# Test the unpacked version standalone
Copy-Item ida\Wilbur_unpacked.exe Game\Wilbur.exe -Force
.\Game\run.bat
```

Expected: game launches without the DVD/disc check (no SecuROM stub to verify). Plays normally up to and including the main menu.

If it crashes:
- **Crash at entry** → wrong OEP. The verified value is `0x0062B48A` (verified by RE'ing the stub's final `jmp` at `0x02EF1854`). If that doesn't work, double-check Scylla actually wrote it into `AddressOfEntryPoint` of the rebuilt PE: `python tools/verify_unpacked_pe.py` checks this.
- **Crash at first DLL call** → import table wrong. In Scylla, re-run "Get Imports" with **Show Invalid** + **Show Suspect** on, fix or cut every yellow/red entry.
- **Crash mid-launch** → CRC self-check residue. Some SecuROM 7 builds have memory CRC checks that fail when the dumped EXE doesn't match the encrypted on-disk one. Workaround: NOP-out the CRC checks (PE-bear → search for the canonical CRC pattern in `rr02`; most are removable). Documented at https://lostfilearchives.github.io/08/28/Dissection/

After verifying it runs, restore the protected original:
```powershell
Copy-Item Game\Wilbur_protected.exe Game\Wilbur.exe -Force
```

(The protected original is what the mod is supposed to load against; the unpacked version is for IDA only.)

---

## Stage C — Fresh IDA database

### C.1 — Build new IDB

1. Open `ida/Wilbur_unpacked.exe` in IDA Pro 9.x.
2. Accept default analysis options. **First analysis pass: 15–40 min** for a 45 MB EXE.
3. Save as `ida/Wilbur_unpacked.exe.i64`.

### C.2 — Migrate symbols from existing IDB

Don't re-do the renames + comments from this session. Migrate them:

1. **File → Produce file → Create IDC file** in `ida/400000.Wilbur.exe.i64`. Outputs `ida/400000.Wilbur.exe.idc` containing every rename + comment we've added.
2. Open `ida/Wilbur_unpacked.exe.i64`. **File → Script file → ida/400000.Wilbur.exe.idc**. Applies all our annotations.
3. Sanity-check: the symbol-table.md addresses should resolve to the same functions in the new IDB. If not, check whether the unpacked EXE's ImageBase rebased anything (it shouldn't — no DYNAMICBASE flag).
4. Save the IDB.

### C.3 — Recompute coverage

Compare function counts:
- Existing IDB (`400000.Wilbur.exe.i64`): 12,555.
- New IDB (`Wilbur_unpacked.exe.i64`): expected 13,500–15,500 if Stage A captured significant in-game code.

The delta is what we recovered. Spot-check by:
1. `find_bytes "FF 15 20 60 6A 00"` — should return matches now (DInput init call sites).
2. Look at `xrefs_to 0x6A6020` (`DirectInput8Create` IAT slot) — should show real game-side callers, not just thunks.
3. Look at `xrefs_to game_bink_play_video` — should show real callers, not zero.

Update [`symbol-table.md`](symbol-table.md) and [`unpack-state.md`](unpack-state.md) with the new coverage numbers.

---

## Stage D — Update repo state

Once C succeeds:

1. **Switch ida-pro-mcp** to point at `ida/Wilbur_unpacked.exe.i64`. (The MCP server reads the active IDB.)
2. **Update** [`research/findings/symbol-table.md`](symbol-table.md) — note the new IDB path, refresh the "addresses are valid in" line.
3. **Update** [`research/findings/unpack-state.md`](unpack-state.md) — replace "Known incompleteness" with "Resolved by Stage A re-dump", note coverage gain.
4. **Update** [`docs/ROADMAP.md`](../../docs/ROADMAP.md) M1 — flip the open re-dump checkbox.
5. **Keep the menu-time IDB** (`ida/400000.Wilbur.exe.i64`) as a checkpoint for now, in case something in the new IDB regresses. After a couple of weeks of working with the new one, archive or delete.

---

## What stays gitignored

- `ida/Wilbur_unpacked.exe` — the rebuilt static EXE.
- `ida/dumps/full/` — Stage A dumps + trace logs.
- `Game/Wilbur_protected.exe` — backup of the user's protected original.
- All IDA databases (`*.i64`, `*.idb`).
- Any `*_SCY.exe`, `*_dumped.exe` from Scylla.

`.gitignore` already covers all of these. Nothing about Option 3 changes what gets committed.

---

## Open risks / things that may trip us up

1. **CRC self-checks.** SecuROM 7 sometimes has memory CRC validators that fire after OEP. If they survive into our dump as live code, the unpacked EXE will detect the modified PE and crash. PE-bear lookup of canonical patterns + NOP usually fixes this. Documented troubleshooting in `docs/SECUROM.md`.
2. **Anti-tamper bound to specific imports.** If SecuROM stub-installed thunks for some imports (the stolen-byte wrappers) and the game code calls them via the wrappers rather than the IAT directly, Scylla's "Cut Thunk" loses those calls. Workaround: leave the wrappers intact in the dump (don't cut them), accept that some imports have indirection.
3. **The Russian translation.** If the user has applied `rus.exe`, some `.dct` files / fonts will live in `Game/data_dx/`. These don't interact with our PE rebuild. The unpacked EXE should still load them correctly.
4. **Wilbur.exe vs Launcher.exe coupling.** The launcher passes `-launchit` to Wilbur. The unpacked Wilbur should still accept this argument. If not, launch directly with `-dxfullscreen -dxadapter=0 -dxresolution=…  -launchit` per `tools/run_game.py` direct-launch mode.

---

## Files this procedure produces

| Path | Status | Purpose |
|---|---|---|
| `ida/dumps/full/process_<pid>/400000.Wilbur.exe` | gitignored | Stage A pe-sieve memory dump. |
| `ida/dumps/full/trace.log` | gitignored | TinyTracer execution log (~MB-GB). |
| `ida/Wilbur_unpacked.exe` | gitignored | Stage B output. The rebuilt static EXE. |
| `ida/Wilbur_unpacked.exe.i64` | gitignored | Stage C output. Fresh IDA database with migrated symbols. |
| Updated `symbol-table.md`, `unpack-state.md`, `ROADMAP.md` | committed | Documentation of the new state. |

## See also

- [`docs/SECUROM.md`](../../docs/SECUROM.md) — original SecuROM analysis + Method A (PE-sieve) and Method B (x64dbg + Scylla) procedures.
- [`research/findings/unpack-state.md`](unpack-state.md) — what the menu-time dump covered, what it missed.
- [`research/findings/lessons-learned.md`](lessons-learned.md) §L2 — three distinct failure modes ("SecuROM-encrypted" really means one of three different things; this procedure addresses the third — lazy-decrypt blocks).
