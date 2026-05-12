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

**Earlier "lazy-decrypt" hypothesis SUPERSEDED 2026-05-05:** initial concern was that code paths only running during in-game 3D scenes (notably DirectInput init) were missing because SecuROM lazily decrypts those blocks. This was wrong. Full RE of the SecuROM 7 stub ([`../research/findings/securom7-stub-re.md`](../research/findings/securom7-stub-re.md)) confirmed the protection has no live encryption — just aPLib decompression at startup. The DInput call site really is missing for a different reason: Wilbur.exe doesn't actually call `DirectInput8Create`; only `Launcher.exe` does. See [`../research/findings/unpack-state.md`](../research/findings/unpack-state.md) §"Dump completeness".

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

## M10 — Render-stack modernization ✅ DONE 2026-05-08+

- [x] Migrate from dxwrapper (D3D8 → D3D9 wrapping) to DXVK (D3D9 → Vulkan).
      ([dxvk-migration-plan-2026-05-08.md](../research/findings/dxvk-migration-plan-2026-05-08.md))
- [x] Native borderless-fullscreen via [windowmode.cpp](../src/mtr-asi/src/windowmode.cpp)
      (replaces dxwrapper's `EnableWindowMode` + `FullscreenWindowMode`).
- [x] DXVK config tuned (`Game/dxvk.conf`): 16x anisotropic, `forceMipmapLodBias=-0.5`,
      `invariantPosition=True`, `cachedDynamicBuffers=True`, `floatEmulation=Strict`,
      `maxFrameLatency=1`.
- [x] MSAA at the API level via [msaa.cpp](../src/mtr-asi/src/msaa.cpp): hooks
      `IDirect3D9::CreateDevice` + Reset to override `D3DPRESENT_PARAMETERS::MultiSampleType`.
      Default ON @ 16x with cap-check fallback (16→8→4→2→NONE). Routes through DXVK to
      native Vulkan multisample.
- [x] `Direct3DCreate9` early hook from DllMain so CreateDevice-time overrides apply on cold launch.

## M11 — Sim/render decoupling ✅ DONE 2026-05-07

- [x] M0–M6 sim/render decouple landed. View-matrix slerp+lerp, player+NPC interp,
      halo follow-fix, 0.003-dt-fix exhaustive sweep.
      ([decouple-m5-m6-plan-complete-2026-05-07.md](../research/findings/decouple-m5-m6-plan-complete-2026-05-07.md))
- [x] dt-correctness: `flt_6FFCBC` rewrite for ~150 subsystems integrating against
      hardcoded 0.003s; alt-pump dt correction.
      ([dt-correctness-root-cause-2026-05-07.md](../research/findings/dt-correctness-root-cause-2026-05-07.md))

## M12 — UI granularity (per-element control) ✅ DONE 2026-05-06+

- [x] Per-element sprite control: composite-key matcher
      (`state_key + uv_bucket + screen_context + bbox_quadrant + sort_key + widget_name_hash`).
- [x] Click-to-pick + gizmo overlay; auto-naming from texture loader; ini persistence.
      ([sprite-per-element-architecture.md](../research/findings/sprite-per-element-architecture.md))
- [x] Cross-screen Specialize fix: drops `screen_context` from pinning so the same texture
      role matches across screens (2026-05-09 evening).

## M13 — World-space debug overlays ✅ DONE 2026-05-09

- [x] Trigger-box overlay: 3D-projected wireframe AABBs via ImGui foreground draw list.
      Homogeneous parametric clip in clip space (6 D3D9 frustum planes). Math validated
      offline via `tools/validate-overlay-frames.ps1` — 60 frames × 12 edges × 0.5 px tol = PASS.
      ([trigger-box-overlay-plan-2026-05-09.md](../research/findings/trigger-box-overlay-plan-2026-05-09.md))
- [x] NPC overlay (Phase 1): walks `dword_724DE4` transform list, reads entity name (+0x50)
      and pos (`*(entity+0x48)+0x10` → fallback `+0x58`), projects to screen, draws label
      via ImGui foreground draw list. Walker SEH-guarded; full 6-plane point-frustum cull;
      strict ASCII validation on names. Shared math header `include/mtr/overlay_math.h`.
      ([npc-overlay-plan-2026-05-09.md](../research/findings/npc-overlay-plan-2026-05-09.md))
- [ ] NPC overlay Phase 2: anim state from `entity+0x158` (gated on RE — needs vtable
      whitelist for non-Player classes).
- [ ] NPC overlay Phase 3: kv_get registry dump on click-pin (gated on click-handler
      integration with `sprite_picking`).
- [ ] Trigger overlay Phase 3: entity walker for `triggerbox` / `trigger_volume` /
      `triggeraoe` classes (currently only renders a hardcoded test box).

## M14 — Autonomous validation pipeline ✅ DONE 2026-05-09

- [x] In-mod scenario runner ([test_harness.cpp](../src/mtr-asi/src/test_harness.cpp)) with
      DI-keyboard-injection-driven engine state navigation, JSON results, clean WM_CLOSE
      shutdown. Scenarios: `boot-to-main-menu`, `verify-main-menu-visible`, `widget-probe`,
      `hold-at-menu`, `overlay-phase1-verify`, `npc-overlay-phase1-verify`.
- [x] PowerShell orchestrator [`run-test.ps1`](../tools/run-test.ps1) with 3-layer watchdog
      (in-mod timeout, log-stall, hard timeout) and per-scenario validator dispatch.
- [x] Multi-scenario overnight orchestrator [`run-overnight.ps1`](../tools/run-overnight.ps1)
      adds a 4th layer (outer process kill).
- [x] Crash handler ([crash_handler.cpp](../src/mtr-asi/src/crash_handler.cpp)) with SEH
      filter writing minidump + result-JSON sentinel so crashes surface as exit 1 (fail)
      not exit 4 (launch failure).
- [x] BMP→PNG thumbnail conversion ([`bmp-to-png-thumb.ps1`](../tools/bmp-to-png-thumb.ps1))
      so archived screenshots are chat-shareable (~1 MB PNG vs ~11 MB BMP).
- [x] Usage guide: [AUTONOMOUS_TESTING.md](AUTONOMOUS_TESTING.md).

## M15 — Coop multiplayer (LAN, authoritative-host) 🟡 PHASE 0 ✅ DONE 2026-05-11

Long-running effort. v1 ETA was ~8–10 months for 2-player LAN coop; **Phase 0 RE
revealed major engine scaffolding that reduces the total by ~10 wk** (see
"v2 plan revisions" below).

Phase ordering is dependency-strict — Phase 1 (transport) MUST NOT start before
all of Phase 0 lands. Phase 0 is now closed; Phase 1 design is in
[coop-phase1-design-2026-05-11.md](../research/findings/coop-phase1-design-2026-05-11.md).

### Plan-level Phase 0 — RE prerequisites ✅ DONE

- [x] **0A — Entity factory RE** ✅ 2026-05-10 + derisk 2026-05-11. `entity_factory_construct` @ 0x5B96F0.
      Derisk spawned a player2 entity from the mod, all 9 breadcrumbs fire, clean teardown.
      [coop-phase-0b-breadcrumb-trail-2026-05-10.md](../research/findings/coop-phase-0b-breadcrumb-trail-2026-05-10.md).
- [x] **0C — .sx script command catalog** ✅ 2026-05-11. 7,640 identifiers from 184 SLNG-bytecode
      files. **Major finding**: engine has vestigial multi-player scaffolding (`IsMultiPlayer`,
      `ActorGetNetMaster`, `GenericNetActor`, `Players_*`, `transremote`, `recv`, 4P placements).
      [coop-phase-0c-sx-command-catalog-2026-05-11.md](../research/findings/coop-phase-0c-sx-command-catalog-2026-05-11.md).
- [x] **0D — Two-process test harness skeleton** ✅ 2026-05-11. `-mtrasi-coop-port=<N>` +
      [tools/run-coop-test.ps1](../tools/run-coop-test.ps1). Mock / single-process / dual-machine.
- [x] **Replication primitive RE** ✅ 2026-05-11. Found functional DistributedState publish/receive
      pair (`entity_publish_distributed_state` @ 0x5AFDB0, `entity_receive_distributed_state` @ 0x5AFE90)
      + transform-replication (`entity_publish_netactor_transforms` @ 0x5B06A0). 10 entity vtables
      participate, including **Protagonist** (player) + wilbur (avatar). Manager pointer at
      entity+216 is null in SP. [coop-phase0-replication-primitive-found-2026-05-11.md](../research/findings/coop-phase0-replication-primitive-found-2026-05-11.md).
- [x] **MP install site + activation gate RE** ✅ 2026-05-11. `entity_install_network_manager` @ 0x5B0C70.
      Activation gate `g_mp_coordinator_ptr` @ 0x745BE8 (12 readers, 0 writers in binary — MP layer was
      stripped at activation). Protagonist::vtable[49] = `sub_474E90` null stub. Three-tier path to enable
      MP from mod: write coordinator → patch/hook vtable[49] OR install fn → implement NetworkManager
      (vtable[10/11/12]). [coop-phase0-mp-install-site-2026-05-11.md](../research/findings/coop-phase0-mp-install-site-2026-05-11.md).
- [x] **0B — Save-system architecture** ✅ 2026-05-11 (partial — coop-relevant subset). Save dispatcher
      RE'd; `save_pump_dispatcher` @ 0x575D60 + handlers identified. **Key finding**: "DistributedState"
      xref scan confirms save and replication use independent serialization paths — coop work won't
      interfere with save/load. Full save-format RE deferred to Phase 2 prerequisite.
      [coop-phase0b-save-system-architecture-2026-05-11.md](../research/findings/coop-phase0b-save-system-architecture-2026-05-11.md).
- [x] **Probe stack shipped (build 681,984)** ✅ 2026-05-11. mtr-asi module `coop_mp_probe` with PRE-logger
      on install fn + armable MultiplayerCoordinator stub at 0x745BE8 (vtable[5/9] wired) + armable
      NetworkManager stub installable at entity+216 via POST hook (vtable[3/10/11/12] wired). Cmdline
      auto-arm (`-mtrasi-coop-mp-armall` etc.) and Debug-tab toggles. Used in 4 live tests 2026-05-11.
      **DELETED 2026-05-12 per RULE №2** (no migration baggage; rejected-architecture stub for replication
      via singleton NetworkManager — real coop will use per-entity managers, see Phase 1 plan). Findings
      preserved in `research/findings/coop-phase0-mp-install-site-2026-05-11.md` and the live-test docs below.
- [x] **Live-test pass shipped** ✅ 2026-05-11. 4 autonomous tests of the probe stack via
      `tools/run-test.ps1 -ExtraArgs`. Key findings: (F1) `entity_install_network_manager` is DORMANT
      in normal load-save play; ~~(F2) engine has built-in input separation primitive at `sub_5A2480`~~
      **F2 RETRACTED 2026-05-11 evening** — table is a cheat-code dispatcher, not gameplay input;
      (F3) only vtable[5] is exercised from 5 caller sites, all in cheat-code path. Engine never crashed.
      [coop-phase0-live-test-findings-2026-05-11.md](../research/findings/coop-phase0-live-test-findings-2026-05-11.md) +
      [coop-phase0-finding-f2-corrected-2026-05-11.md](../research/findings/coop-phase0-finding-f2-corrected-2026-05-11.md).

### v2 plan revisions (cumulative — includes 2026-05-11 live-test findings)

| Phase | v2 estimate | Revised | Why |
|---|---|---|---|
| 0B (save RE) | ~1 wk | hours (partial) | save vs replication confirmed decoupled; full format RE moved to Phase 2 prereq |
| Phase 1 (transport) | 4 wk | ~5 wk | consumer interface (coordinator + manager classes) is now known, but +1wk for implementing them |
| Phase 1d (install vector) | (bundled) | 1-1.5 wk | live test 2026-05-11 found `entity_install_network_manager` is dormant in normal play → need different install vector |
| Phase 2 (input separation) | 4 wk | ~~~1.5 wk~~ **~4 wk (no built-in primitive)** | ~~live test 2026-05-11 found built-in input-separation primitive at `sub_5A2480`~~ — F2 RETRACTED: `sub_5A2480` is a cheat-code dispatcher disable, not input separation. Phase 2 reverts to v2 estimate. See [coop-phase0-finding-f2-corrected-2026-05-11.md](../research/findings/coop-phase0-finding-f2-corrected-2026-05-11.md) |
| Phase 3 (replication) | 4 wk | **~1 wk** | publish/receive code exists; just implement the manager |
| Phase 5 (script-VM replication) | 8 wk | ~2-3 wk | primitives likely shared via string-keyed channel naming |

Net: **~9-10 wk reduction** from original v1 estimate (8–10 mo → ~7 mo). Was claimed ~11 wk; revised down after F2 correction.

### Plan-level Phase 1 — UDP transport (next milestone)

Design: [coop-phase1-design-2026-05-11.md](../research/findings/coop-phase1-design-2026-05-11.md).

Scope summary: build the UDP transport, implement the MultiplayerCoordinator + NetworkManager
classes against the known engine interface, wire to the publish/receive primitives. Phase 1
absorbs the work that was originally split between Phase 1 (transport) and Phase 3 (replication)
in the v2 plan.

### Plan-level Phases 2–7 — not started

Phase 2 = 2nd player spawn + input separation, Phase 4 = NPC + prop replication,
Phase 5 = script VM replication, Phase 6 = save/pause/UI, Phase 7 = stability.

---

## Out of scope (for now)

- Cracking / DRM removal redistribution. We unpack to enable analysis only.
  No SecuROM-stripped binary is committed or distributed from this repo.
- Asset extraction / mod tools beyond what's needed for M1–M8.
- 64-bit port. Wilbur.exe is i386 and we hook in-process; recompilation is not
  in scope.
