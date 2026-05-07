# Roadmap

This document is the single source of truth for *what we're doing and in what order*.
Entries are listed in dependency order — earlier work unblocks later work.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done · `[!]` blocked

---

## M0 — Bootstrap (this session)

- [x] PE-analyze `Wilbur.exe`, `Launcher.exe`, `rus.exe`. Confirm SecuROM on Wilbur.
- [x] Inspect `rus.exe` 7zSFX payload (read-only). Confirm no Wilbur replacement inside.
- [x] Initialize project structure, docs, git scaffolding.
- [x] Move IDA databases to `ida/`, document workflow.
- [x] Write SecuROM unpacking procedure ([SECUROM.md](SECUROM.md)).
- [x] Distill WSGF widescreen prior-art ([prior-art/wsgf-widescreen-hack.md](prior-art/wsgf-widescreen-hack.md)).

## M1 — Clean disassembly of Wilbur.exe ✅ DONE

> Done via PE-sieve `/imp 3` against a running process (2026-05-04). 45 MB unpacked image, 12,555 functions, Hex-Rays works.

- [x] PE-sieve unpack of running Wilbur.exe → `ida/dumps/process_22276/400000.Wilbur.exe`.
- [x] IDA database `ida/400000.Wilbur.exe.i64` built from the dump; auto-analysis recovered 12,555 functions.
- [x] Sanity-checked: strings, imports, function count are real. Hundreds of functions decompiled cleanly via Hex-Rays.
- [x] Reproducible procedure documented in [`../research/findings/unpack-state.md`](../research/findings/unpack-state.md).

**Known incompleteness (open):** dump was taken at the main menu. Code paths that only run during in-game 3D scenes (notably DirectInput init) are still in their pre-trigger SecuROM lazy-decrypt state and don't appear as instructions in our image. See [`../research/findings/lessons-learned.md`](../research/findings/lessons-learned.md) §L2 for the verified evidence (zero `FF 15 20 60 6A 00` bytes anywhere in the image despite `DirectInput8Create` being called at runtime).

### M1.5 — Full unpack to a static `Wilbur_unpacked.exe`  ✅ DONE 2026-05-05

After full RE of the SecuROM 7 stub ([`../research/findings/securom7-stub-re.md`](../research/findings/securom7-stub-re.md)), discovered the protection has no real encryption — just aPLib compression + BCJ-86 byte filter + custom IAT resolver. PE-sieve already produced a structurally valid PE; only `AddressOfEntryPoint` needed updating from the stub entry to the real OEP.

- [x] Reverse-engineer the stub fully — confirmed no encryption, identified real OEP `0x0062B48A` (`_WinMainCRTStartup`).
- [x] Write [`tools/build_standalone_exe.py`](../tools/build_standalone_exe.py) — 4-byte AddressOfEntryPoint patch + high-bit clears.
- [x] Run it: produced `ida/Wilbur_unpacked.exe` (43 MB, gitignored).
- [x] Verify structurally via `tools/verify_unpacked_pe.py` — all checks pass.
- [ ] Smoke test: copy `Wilbur_unpacked.exe` over `Game/Wilbur.exe` (back up original first), launch via `Game/run.bat`, confirm game runs through to main menu without the SecuROM stub. **Pending user-side test — needs the user at the keyboard.**
- [ ] Stage C — fresh IDA database from `Wilbur_unpacked.exe`, migrate symbols via IDC export from existing IDB. **Optional, ergonomic — current `400000.Wilbur.exe.i64` is already complete.**
- [ ] Stage D — if Stage C is done, switch ida-pro-mcp endpoint to the new IDB.

Stage A (coverage capture) was skipped — not needed since the dump was already complete. Documented in [`../research/findings/full-unpack-procedure.md`](../research/findings/full-unpack-procedure.md) for use on other (more heavily protected) SecuROM 7 builds.

Output: `ida/Wilbur_unpacked.exe` (gitignored — analysis-only, never committed).

## M2 — Skeleton ASI mod loadable into the game

Goal: prove the toolchain end-to-end with a no-op mod that just shows it's alive.

- [ ] Pick a clean Wilbur.exe build target (the dumped one) — but the **mod loads
      against the original `Wilbur.exe`**, not the dump. Verify the live image base
      is `0x00400000`.
- [ ] Wire MinHook + Ultimate ASI Loader into `src/mtr-asi/`. Build `mtr-asi.asi`.
- [ ] DllMain hooks one well-known import (e.g. `kernel32!Sleep`) and writes to
      `mtr-asi.log`. Drop into Game folder, launch, observe the log.
- [ ] Add a build-time check that aborts loading if the host `Wilbur.exe` has the
      wrong size / hash (don't trash other people's installs).

## M2.5 — Dear ImGui overlay (the mod's primary UI)

> **This unblocks M7/M8 without depending on M6.** With an ImGui overlay we can
> ship the resolution/AR/FPS toggles before reverse-engineering the native menu.
> Keep M6 as an *optional* later milestone for the polished, native-look version.

- [ ] Hook `IDirect3DDevice9::EndScene` (and `Reset`) via the vtable.
- [ ] Initialise ImGui (`ImGui_ImplWin32_Init` + `ImGui_ImplDX9_Init`) on the
      first EndScene. Tear down on `Reset`/`Lost`.
- [ ] Hook the host `WndProc` (via `SetWindowLongPtr`) so ImGui receives input.
- [ ] Toggle hotkey: `Insert` (configurable in `mtr-asi.ini`).
- [ ] Render an empty placeholder window — proves the pipeline. Real menu
      content lands in M7/M8 (`src/ui/menu.cpp`).

## M3 — Reverse the renderer setup path

> Required for the resolution / aspect-ratio menu.

- [ ] Find `Direct3DCreate9` and `IDirect3D9::CreateDevice` call sites in Wilbur.exe.
- [ ] Identify the `D3DPRESENT_PARAMETERS` struct fill — that's where the resolution,
      backbuffer format, vsync, and (indirectly) FPS cap come from.
- [ ] Find the function(s) that set the **viewport** and **projection matrix** —
      these are the aspect-ratio knob.
- [ ] Document each in `research/findings/renderer.md` with: function VA, Hex-Rays
      pseudocode, observed inputs/outputs, fields of any related structs.
- [ ] Push function names + comments back into `ida/Wilbur.exe.i64` via MCP.

## M4 — Reverse the camera

- [ ] Locate the active camera structure (likely a global, or a member of a `Player`
      / `World` singleton). Identify the FOV field, position/orientation fields,
      near/far plane fields.
- [ ] Trace **all** functions that read or write the camera struct. The ones that
      write the projection matrix are our hook targets.
- [ ] Confirm the `1.333333f` aspect-ratio constant referenced by the WSGF hack
      is loaded by one of these functions. Cross-reference its xref to lock down
      which call path applies the aspect.
- [ ] Document in `research/findings/camera.md`.

## M5 — Reverse the main game tick

> Required for the FPS limiter.

- [ ] From `WinMain`, follow the message-pump → game-loop chain. The "tick" is
      typically `if (!PeekMessage) GameUpdate(); GameRender();` or similar.
- [ ] Identify whether the engine has a single tick function or a split
      simulate/render pair. Document timing-related globals (delta-time, fixed-step
      flag, last-frame-time).
- [ ] Document in `research/findings/main-loop.md`.

## M6 — Reverse the native main menu *(optional polish)*

> No longer a hard prerequisite. Once M2.5 lands, the ImGui overlay carries the
> mod menu. M6 is for the "native-look" version where our entries appear in the
> game's own options screen.

- [ ] Find the main menu state machine. Likely a Lua-script-driven UI (the SFX
      payload showed `data\lang\*.dct` + scripted fonts) — but the C++ side that
      *registers* menu entries is what we hook.
- [ ] Identify the function that builds the option list (resolution dropdown
      currently lives only in `Launcher.exe`, but the in-game menu must have its
      own builder).
- [ ] Decide on injection strategy:
   1. **Hook the menu-build function** and append our entries (cleanest).
   2. **Replace an existing-but-rarely-used entry** (lowest risk).
- [ ] Document in `research/findings/main-menu.md`.

## M7 — Implement: resolution & aspect-ratio (ImGui overlay)

- [ ] Add resolution + AR dropdowns to the ImGui overlay (M2.5 host).
- [ ] Persist user choice in `mtr-asi.ini`.
- [ ] On apply: hook D3D9 device reset / re-creation (M3) to push the new
      backbuffer dimensions; hook the projection-matrix builder (M4) to push the
      new aspect ratio.
- [ ] Test on 1080p, 1440p, 4K, 21:9 ultrawide.
- [ ] (M6 follow-up) Mirror the same controls into the native menu when M6 lands.

## M8 — Implement: fixed FPS limiter

- [ ] Add FPS dropdown (60 / 120 / 144 / 240 / unlimited) to the ImGui overlay.
- [ ] Hook the per-frame tick (M5). Decide between:
   - **Sleep-based** limiter: lower CPU but jittery.
   - **Spin-wait** limiter: rock-solid frame timing, burns one core.
   - **Hybrid** (sleep until ~1 ms before deadline, spin the rest).
- [ ] Verify the simulation isn't frame-rate-coupled. If it is, decouple
      simulation step from render rate (likely scope creep — document and decide).

## M9 — Polish & release-quality

- [ ] Logging discipline: a single `mtr-asi.log` with levels.
- [ ] Crash-safe init: every hook installation guarded; if a hook fails the mod
      degrades to no-op rather than crashing the game.
- [ ] Version-detection so the mod refuses to load against unknown Wilbur.exe builds.
- [ ] User-facing README with screenshots and `mtr-asi.ini` reference.
- [ ] Optional: `Launcher.exe` patch so the launcher's resolution selector and
      `-dxresolution` flag are no longer the only way to set resolution.

---

## Out of scope (for now)

- Cracking / DRM removal redistribution. We unpack to enable analysis only.
  No SecuROM-stripped binary is committed or distributed from this repo.
- Asset extraction / mod tools beyond what's needed for M1–M8.
- 64-bit port. Wilbur.exe is i386 and we hook in-process; recompilation is not
  in scope.
