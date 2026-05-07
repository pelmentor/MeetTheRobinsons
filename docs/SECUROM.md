# SecuROM 7 — analysis & unpacking procedure for `Wilbur.exe`

This document explains *why* IDA Pro can't auto-analyze `Wilbur.exe` out of the box,
*how* we know it's protected, and the agreed procedure for producing a clean
unpacked binary that IDA can analyze.

> **2026-05-05 update.** M1 (initial unpack via PE-sieve `/imp 3`) is **done** —
> we have a working IDA database at `ida/400000.Wilbur.exe.i64` with 12,555 functions.
> But that dump was taken at the **main menu**, before in-game DirectInput init / world
> streamer / certain effect setups had run. SecuROM 7 lazily decrypts those code blocks
> on first execution, so they aren't in our image. **Verified evidence:** zero
> instances of `FF 15 20 60 6A 00` (the call sequence to `DirectInput8Create`) anywhere
> in the dump — the IAT slot is populated by the loader, but no instruction in the
> entire image references it.
>
> **The chosen next step is "Option 3" — full unpack to a static EXE.** This means a
> richer Stage-A coverage capture (TinyTracer + multi-state pe-sieve while playing
> through the game), then Scylla rebuild of the in-memory image into a runnable
> standalone PE. The complete procedure lives at
> [`../research/findings/full-unpack-procedure.md`](../research/findings/full-unpack-procedure.md).
> Output is local-only (`ida/Wilbur_unpacked.exe`, gitignored) — never committed.
>
> The rest of this document is preserved as the historical analysis / Method A
> (PE-sieve only) / Method B (x64dbg + Scylla, classic) reference. Stage B of the
> full-unpack procedure is essentially Method B applied to a richer dump.

---

## Evidence that Wilbur.exe is SecuROM-protected

Output of our PE-header parse (`research/pe-analysis.md` has the full transcript):

```
Section[0]  rr01    VAddr=0x00001000  VSize=0x024F8000  ROff=0x00000400  RSize=0x00000000  Char=0xE0000080
Section[1]  rr02    VAddr=0x024F9000  VSize=0x005F9000  ROff=0x00000400  RSize=0x005F8A00  Char=0xE0000040
Section[2]  .rsrc   VAddr=0x02AF2000  VSize=0x00007000  ROff=0x005F8E00  RSize=0x00006200  Char=0xC0000040

EntryPoint(VA): 0x02EF16A0   (inside rr02)
ImageBase:      0x00400000
SizeOfImage:    0x02AF9000   (~43 MB virtual!)
```

**Three independent smoking guns:**

1. **Section names `rr01` / `rr02`.** Every Visual C++ executable has `.text`, `.rdata`,
   `.data`, `.rsrc`, etc. Non-standard section names — and especially the prefix `rr` —
   are the documented signature of **SecuROM 7.x** (Sony DADC). The original game is
   from a publishing era (Disney/Buena Vista, 2007) where SecuROM 7 was the standard
   protection.
2. **`rr01` has `VirtualSize=0x024F8000` (~38 MiB) but `RawSize=0`.** That is, the
   section is *defined* in the address space but has *no bytes on disk*. This is
   the unpacking buffer: SecuROM allocates it at load time and decrypts the original
   game code into it. Without running the game, that 38 MB is just zero-filled.
3. **Entry point at `0x02EF16A0` lives inside `rr02`** (which holds the SecuROM stub
   itself), not in any sane original-game section. The entry point will execute the
   unpacker, which mutates `rr01` and eventually transfers control to the original
   `OEP` somewhere inside the unpacked region.

This is why IDA's auto-analyzer gives nothing useful. There is *literally no game
code in the file on disk*.

---

## Strategy: runtime dump + IAT reconstruction

Standard, well-documented technique. **No DRM circumvention is required for analysis purposes** —
we just need a snapshot of memory after SecuROM has done its job, so IDA has something to
disassemble. The original `Wilbur.exe` stays exactly as it is on disk.

The pipeline:

```
        ┌────────────────────┐
        │ Wilbur.exe (disk)  │   3 sections, encrypted
        └─────────┬──────────┘
                  │ run under x32dbg / Intel PIN
                  ▼
        ┌────────────────────┐
        │ SecuROM stub runs  │   allocates rr01, decrypts code,
        │ (in rr02 / heap)   │   resolves imports, builds IAT
        └─────────┬──────────┘
                  │ at OEP (Original Entry Point of game code):
                  │ this is what we want to capture
                  ▼
        ┌────────────────────┐
        │ Process memory     │
        │ image @ 0x400000   │   <-- dump THIS
        └─────────┬──────────┘
                  │ "Dump" + "Fix IAT"
                  ▼
        ┌────────────────────┐
        │ Wilbur_dumped.exe  │   normal .text/.rdata/.data, valid IAT
        └────────────────────┘   <-- this is what we feed to IDA
```

There are **two viable toolchains in 2026**. Method A is the modern recommendation;
Method B is the classic one with broader documentation. Pick the one you're more
comfortable with — both produce the same kind of artefact.

---

## Method A — PE-sieve + TinyTracer  *(recommended in 2026)*

Uses [hasherezade's](https://hasherezade.github.io/) toolchain on top of
[Intel PIN](https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-dynamic-binary-instrumentation-tool.html).
Much faster than the manual ESP-trick dance and **immune to most anti-debug**
because PIN is a dynamic binary instrumentation framework, not a debugger —
SecuROM's `IsDebuggerPresent` / NtQueryInformationProcess / timing checks
return clean values for free.

Reference tutorial: [hshrzd.wordpress.com/2025/03/22/unpacking-executables-with-tinytracer-pe-sieve/](https://hshrzd.wordpress.com/2025/03/22/unpacking-executables-with-tinytracer-pe-sieve/)
(March 2025 — the closest thing to a current canonical guide).

### Pre-requisites

- **Intel PIN** for Windows IA32 (32-bit) — https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-dynamic-binary-instrumentation-tool.html
- **TinyTracer** — https://github.com/hasherezade/tiny_tracer (drop the prebuilt
  `TinyTracer.dll` into PIN's tools folder). Latest release as of 2026 includes
  IAT-Tracer and a ready-made unpacking flow.
- **PE-sieve** — https://github.com/hasherezade/pe-sieve (single executable,
  no install). Used to scan the live process and dump every PE module — including
  reconstructed IAT — to disk.
- A working game install with SecuROM able to validate (DVD or ImgDrive / Alcohol
  120% / DAEMON Tools mount, with **SecuROM disc emulation enabled**).

### Procedure

1. Run Wilbur.exe under PIN with TinyTracer:

   ```cmd
   pin.exe -t TinyTracer.dll -- Game\Wilbur.exe
   ```

   PIN bypasses anti-debug; the SecuROM stub runs to completion. TinyTracer
   produces `Wilbur.exe.tag` listing all `VirtualAlloc` / `VirtualProtect` calls
   plus the section transition where execution first enters the freshly-decrypted
   `rr01` region. **That transition is the OEP** — it's printed verbatim in the
   tag log (look for `[SECTION] rr01 -> ...` or the TinyTracer `OEP` annotation).
2. While the game is still running (or use TinyTracer's `pause-at-OEP` mode to
   snapshot before the game continues), run PE-sieve on the process:

   ```cmd
   pe-sieve.exe /pid <wilbur_pid> /imp 3 /shellc 0 /ofilter 1 /dir dumps\
   ```

   `/imp 3` tells PE-sieve to **rebuild the IAT** during the dump. Output:
   `dumps\process_<pid>\Wilbur.exe` — fully unpacked, IAT fixed, ready for IDA.
3. **Validate**: open `Wilbur.exe` from the dump folder in CFF Explorer or run
   `tools\pe-info.ps1` on it. Expect `.text`/`.rdata`/`.data`/`.rsrc` sections,
   a sane SizeOfImage (~5–10 MB), and an entry point inside `.text`.
4. Rename to `ida/Wilbur_dumped.exe` and load in IDA. Save as `ida/Wilbur.exe.i64`,
   replacing the broken database.

### Why this beats the classic method

- **Anti-debug becomes a non-issue.** SecuROM 7 has IsDebuggerPresent, ProcessHeap
  flags, NtSetInformationThread(HideFromDebugger), CheckRemoteDebuggerPresent,
  timing/RDTSC checks, and CRC checks on its own code (the latter would normally
  detect a debugger setting an INT3 breakpoint). PIN doesn't trigger any of these
  because there's no debugger attached — PIN re-JITs the binary in-process.
- **OEP detection is automatic.** TinyTracer logs every section-to-section control
  transfer; the first jump from `rr02` (stub) into `rr01` (decrypted code) is the
  OEP, no ESP-trick needed.
- **IAT reconstruction is automatic.** PE-sieve walks the loaded module's import
  table at dump time, even when SecuROM has substituted thunks with stolen-byte
  wrappers, and reconstructs valid `.rdata` import entries.

---

## Method B — x32dbg + bundled Scylla + ScyllaHide  *(classic, more tutorials available)*

The well-trodden path. Use this if you're already comfortable with x32dbg or want
to follow one of the many existing SecuROM tutorials.

> **Verified against x64dbg snapshot `2025-08-19`.** Folder layout, file presence,
> and invocation method below are what the official snapshot actually ships —
> not what older tutorials describe.

### Pre-requisites

- **x32dbg** — download the official snapshot from https://x64dbg.com/ (big
  Download button) or from https://github.com/x64dbg/x64dbg/releases. The ZIP
  extracts to:
  ```
  x64dbg_snapshot_<date>/
  ├── x96dbg.exe                ← launcher (lets you pick 32 vs 64-bit)
  ├── x32/
  │   ├── x32dbg.exe            ← what you actually run for Wilbur (i386)
  │   ├── Scylla.dll            ← Scylla library (built-in, no plugin file needed)
  │   └── plugins/              ← drop ScyllaHide here (see below)
  └── x64/
      └── …
  ```
  **Heads-up: Scylla is no longer a plugin in modern x64dbg builds.** It's
  loaded as a library (`Scylla.dll`) and invoked from the main UI. There is no
  `Scylla.dp32` file and no `Plugins → Scylla` menu entry — see "Invoking
  Scylla" below. Confirmed in
  [x64dbg discussion #3446](https://github.com/orgs/x64dbg/discussions/3446).
- **ScyllaHide** — separate download from
  https://github.com/x64dbg/ScyllaHide/releases. **Required for SecuROM** —
  without it the stub will detect the debugger and either exit silently or
  never reach OEP. Extract these files into `x32/plugins/`:
  - `ScyllaHideX64DBGPlugin.dp32`
  - `HookLibraryx86.dll`
  - `scylla_hide.ini`
- **Optional**: [ergrelet/Scylla](https://github.com/ergrelet/Scylla) — actively
  maintained fork with Python bindings. Useful if you want to script the IAT
  work as a standalone tool; not required for the basic procedure.
- A working DRM-validating game install (see Method A pre-reqs).

### Russian-language quirk

x32dbg's Russian translation labels the **Plugins** menu as **"Модули"**, which
collides with the Russian word for *Modules* (the loaded-DLL view). If you can't
find the Plugins menu: switch to English via `Options → Preferences → Misc →
Language`. Or look for "Модули" in the top menu — that's it.

### Invoking Scylla (since it's not in the Plugins menu)

Two options:

1. **Toolbar button** — the toolbar at the top of x32dbg has a Scylla icon
   (literal "S" on a tile). Click it.
2. **Command bar** — the input field at the very bottom of the x32dbg window
   (the same one where you type `bp`, `g`, etc.). Type:
   ```
   StartScylla
   ```
   and press Enter.

The standard "press Ctrl+I to open Scylla" instruction from older guides may or
may not still be wired up — if it doesn't open the window, fall back to the two
options above.

### Procedure

> Underlying technique: classic packer-unpacking methodology (the "ESP trick").
> Same basic idea works for SecuROM 7, UPX, ASPack, and most non-VM packers.

1. **Configure ScyllaHide first** — `Plugins → ScyllaHide → Options`. Enable
   the `VMProtect` profile (strong all-round anti-anti-debug). Also tick
   `NtSetInformationThread` and `KillAntiAttach`. Save and set as default.
2. Open `Game/Wilbur.exe` in x32dbg (`File → Open` or drag-drop). The debugger
   pauses at the system breakpoint inside `ntdll`. Press **F9** (Run) once — it
   stops at Wilbur.exe's EntryPoint (the SecuROM stub at VA `0x02EF16A0`).
3. **Find OEP using the ESP trick**:
   - Right-click `ESP` in the registers panel → **Follow in Dump**.
   - In the dump window, select the first 4 bytes (a DWORD) → right-click →
     **Breakpoint → Hardware, Access → DWORD**.
   - Press **F9**. The hardware BP fires when the stub unwinds the saved
     registers and is about to `ret` into the OEP.
   - Single-step out; you should land on a normal `_WinMainCRTStartup`-style
     prologue (`push ebp; mov ebp, esp; …` or VC++ CRT init).
4. **Open Scylla** (toolbar icon or `StartScylla` command — see "Invoking
   Scylla" above):
   - In the process drop-down at the top, select the running `wilbur.exe`.
   - Set **OEP** to the address you found.
   - **IAT Autosearch** → **Get Imports**.
   - Sanity-check the import list (kernel32, user32, d3d9, dinput8, …).
     Mark any "invalid" entries → **Cut Thunks**.
   - **Dump** → save as `ida/Wilbur_dumped.exe`.
   - **Fix Dump** → pick the dumped file. Scylla emits `Wilbur_dumped_SCY.exe`.
5. Validate as in Method A (sections, SizeOfImage, strings).
6. Load the dump in IDA, save as `ida/Wilbur.exe.i64`.

### Pitfalls

| Symptom | Likely cause / fix |
|---|---|
| Plugins menu shows only ScyllaHide, no Scylla entry | Expected. Scylla isn't a plugin in current x64dbg — see "Invoking Scylla" above. |
| Game won't run under x32dbg even with ScyllaHide | Try the `Themida v2.x` ScyllaHide preset, or enable **Hardware Breakpoint Protection**. SecuROM 7 has CRC checks on its own code that detect INT3 breakpoints — use hardware BPs only on the stub. |
| Hardware BP never fires | You're on the wrong stack slot. Try `[esp+4]` or `[esp+8]`. Or switch to the **VirtualProtect breakpoint** technique: set a conditional BP on `kernel32!VirtualProtect` filtered to addresses in `0x00400000..0x02000000`; the stub calls it right before transferring to OEP. |
| OEP found but imports are garbage | SecuROM uses **stolen-bytes wrappers** for some imports — short stubs in heap memory wrapping the real API. Right-click the bad entry → **Trace via wrapper**, or **Cut Thunk** if trace fails. For unrecoverable wrappers, manually patch the call site to point at the real IAT slot. |
| Dump runs standalone but crashes | IAT thunks ended up at the wrong RVA. Re-run Scylla with **Add new section** ticked. |

---

## Method C *(specialist, only if A and B both fail)*

Some SecuROM 7 builds have aggressive **memory-CRC self-checks** that corrupt
their own code if a debugger has stepped through it, plus **CPUID-based timing
checks** and **stolen-byte virtualisation** of selected APIs. The community has
reusable scripts for these, documented in the
["Breaking SecuROM 7 - A Dissection"](https://lostfilearchives.github.io/08/28/Dissection/)
walkthrough — search for *"Securom 7.x CRC Check Fixer"*, *"Jump Bridge & Crypted
Code Fixer"*, *"CPUID Fixer"*. The walkthrough's tool list (OllyDbg + SoftICE +
LordPE) is dated; the *techniques* still apply if you adapt them to x32dbg's
script engine. Treat this as a last resort — the *Meet the Robinsons* SecuROM
stub is on the simpler end of the SecuROM 7 spectrum.

---

## After the dump (both methods converge here)

Sanity-check the dump:

- File size much smaller than the live process (~5–15 MB typical, vs ~43 MB virtual).
- Sections look like `.text` / `.rdata` / `.data` / `.rsrc` (plus an IAT-fix
  section appended by Scylla / PE-sieve).
- IDA's strings panel shows real engine strings (`"camera"`, `"main_menu"`,
  `"d3d"`, `.lua` references, …). Gibberish = wrong OEP, redo.
- The float `AB AA AA 3F` (the WSGF aspect-ratio constant) appears in `.rdata`.
  Find it via `find_bytes` — the xref to it lands you straight in the camera
  code (M4).

Load in IDA Pro:

- File → Open → `ida/Wilbur_dumped.exe` (or whichever was the IAT-fixed output).
  Accept defaults: PE, x86, image base `0x400000`.
- Save as `ida/Wilbur.exe.i64`, overwriting the broken database (the previous
  one was built from the still-protected file and is useless).
- Let auto-analysis finish. Hex-Rays should now decompile freely.

---

## What about the WSGF widescreen hack? Doesn't that prove Wilbur.exe is patchable as-is?

The published [WSGF widescreen hack](prior-art/wsgf-widescreen-hack.md) tells you to
hex-edit the float `AB AA AA 3F` (= `1.333333` = 4:3 aspect ratio) inside `Wilbur.exe`
to a different aspect ratio. **This works** — and it's a useful clue:

It means the float constant `1.333333f` is stored in a **plain-text region of `rr02`**
(the SecuROM stub leaves data constants alone if they're not used by the stub itself),
or in the `.rdata` *equivalent* that lives within `rr02` after unpacking.

For our purposes that means:
- Some data in `rr02` is on-disk plain text.
- All *executable code* of the original game still lives in encrypted `rr01` until runtime.

The hex-hack approach is fine for a one-off static patch; **it's the wrong primitive for
an ASI mod that wants to inject a runtime menu**, because:
- We need to *call* game code (e.g. the menu builder, the resolution applicator), and
  those targets exist only at runtime.
- We want to read game state structures (e.g. the camera) — also runtime.

So: the hex-hack is a useful sanity check / prior art, not a substitute for the dump.

---

## Files this procedure produces

- `ida/Wilbur_dumped.exe`              — raw memory dump (kept for forensics; gitignored).
- `ida/Wilbur_dumped_SCY.exe`          — IAT-fixed dump (the input to IDA; gitignored).
- `ida/Wilbur.exe.i64`                 — IDA database built from the fixed dump (gitignored).
- `research/findings/securom-dump-notes.md` — one-time writeup with **your** OEP value,
   Scylla settings, and any quirks specific to your dump. Commit this; it documents
   reproducibility.

---

## References

### Game-specific
- [PCGamingWiki — Meet the Robinsons](https://www.pcgamingwiki.com/wiki/Meet_the_Robinsons)
- [WSGF — Disney's Meet the Robinsons](https://www.wsgf.org/dr/disneys-meet-robinsons/en)

### Tools (current as of May 2026)
- [x64dbg](https://x64dbg.com/) — actively maintained debugger; latest CalVer release `2026.04.20+`. Bundles a kept-current Scylla.
- [ScyllaHide](https://github.com/x64dbg/ScyllaHide) — anti-anti-debug plugin for x64dbg.
- [hasherezade/pe-sieve](https://github.com/hasherezade/pe-sieve) — modern process scanner / dumper with automated IAT reconstruction.
- [hasherezade/tiny_tracer](https://github.com/hasherezade/tiny_tracer) — Intel PIN tool for tracing API calls and finding OEPs.
- [Intel PIN](https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-dynamic-binary-instrumentation-tool.html) — DBI framework that hosts TinyTracer.
- [ergrelet/Scylla](https://github.com/ergrelet/Scylla) — actively maintained Scylla fork with Python bindings.

### Avoid (stale)
- ❌ Standalone [NtQuery/Scylla](https://github.com/NtQuery/Scylla) (`v0.9.8`, last release ~2014). Use the Scylla bundled inside x64dbg, or the ergrelet fork.
- ❌ OllyDbg / SoftICE / LordPE — historically what SecuROM tutorials used; superseded by x64dbg + the hasherezade tooling.

### Walkthroughs
- [hasherezade — Unpacking executables with TinyTracer + PE-sieve (2025-03)](https://hshrzd.wordpress.com/2025/03/22/unpacking-executables-with-tinytracer-pe-sieve/) — the canonical 2025+ tutorial.
- ["Breaking SecuROM 7 — A Dissection" on lostfilearchives](https://lostfilearchives.github.io/08/28/Dissection/) — SecuROM-7-specific anti-dump tricks (CRC, CPUID, stolen-byte virtualisation). Old toolchain in the article, but the techniques and the CRC-fixer scripts still apply.
- [Guided Hacking — How to Dump Packed Executables with Scylla](https://guidedhacking.com/threads/how-to-dump-a-packed-executable-with-scylla.15937/) — generic Scylla refresher.
- [Goggle-Headed Hacker — Unpacking Executables: The ESP Trick](https://goggleheadedhacker.com/blog/post/6) — the ESP trick in detail.
