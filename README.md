# Meet the Robinsons (2007 PC) — Reverse Engineering & ASI Mod

Research-grade reverse engineering of the 2007 PC port of *Disney's Meet the Robinsons*
(Avalanche Software / Buena Vista Games), with a working ASI mod that fixes
the original engine's display limitations on modern hardware.

> **Purpose:** game preservation, technical research, quality-of-life modding.
> All work targets a legally owned retail copy of the game.

---

## Guiding principles

This project is built under 7 architectural principles distilled from
MTA's README + source ([reference/mtasa-blue/](reference/mtasa-blue/) —
the canonical "retrofit multiplayer onto a single-player game via
hook-only mod", 22+ years of experience). The principles below apply to
every non-trivial design decision. Full detail with MTA source quotes in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

1. **No modification of original game files.** Runtime hooks/patches
   only. `Wilbur.exe`, `.sx`, `.dbl`, assets untouched on disk.
2. **Engine-extension paradigm.** mtr-asi is an engine on top of MTR's
   engine, not "a few hooks". Modules own lifecycles; clean APIs.
3. **Parallel class hierarchy mirroring engine structures.** For coop:
   `MtrRemotePlayer` (ours, owns network/interp/input) + orphan entity
   (engine class, owns rendering/animation), connected by pointer. MTA
   shape: `CClientPed::m_pPlayerPed → CPlayerPed*` (CClientPlayer subclasses
   CClientPed for remote-player flavour).
4. **Targeted crash fixes, not broad suppression.** Each NULL-deref or
   single-player-assumption gets its own targeted patch/init/route at
   that specific call site. NEVER a broader "unlink all" mechanism.
5. **Minimum viable subset.** Coop scope is "two players walk + interact
   through existing levels". See [docs/COOP_SCOPE.md](docs/COOP_SCOPE.md).
6. **Augment SP, never replace it.** Where coop meets SP, prefer
   per-player routing inside SP over bypassing SP. Players hit SP first.
7. **Engine-wrapper layer ≠ gameplay/network layer.** MTA splits engine
   wrappers (`Client/game_sa/`) from gameplay/network logic
   (`Client/mods/deathmatch/logic/`). Mirror this discipline in our
   `src/coop/` subtree — files don't mix engine VA dereferences with
   network state.

Plus two top-level rules in [CLAUDE.md](CLAUDE.md):
- **RULE №1 — no crutches**: always pick the proper root-cause fix,
  weeks or months OK if it's proper.
- **RULE №2 — no migration baggage**: when something is replaced, the
  old code goes fully and immediately. No legacy fallbacks, no
  compatibility flags, no parallel old + new paths.

---

## Recent work (2026-05-07 → 2026-05-10)

The original status table below reflects state as of 2026-05-04. The mod has grown substantially since. Highlights of work shipped in the last week:

- **DXVK migration** (replaces dxwrapper). Wilbur's D3D9 calls now route through DXVK to native Vulkan. Hardware MSAA via [src/mtr-asi/src/msaa.cpp](src/mtr-asi/src/msaa.cpp) (default 16x with cap-check fallback). Native borderless-fullscreen via [windowmode.cpp](src/mtr-asi/src/windowmode.cpp). Tuned DXVK config in `Game/dxvk.conf` (`samplerAnisotropy=16`, `forceMipmapLodBias=-0.5`, `invariantPosition=True`, `cachedDynamicBuffers=True`).
- **Sim/render decoupling complete** (M0–M6 plan). Camera view-matrix interpolation, player+NPC transform interp with save/write/restore fence, halo follow-fix, exhaustive 0.003-dt audit. The game runs smoothly at 240Hz. ([research/findings/decouple-m5-m6-plan-complete-2026-05-07.md](research/findings/decouple-m5-m6-plan-complete-2026-05-07.md))
- **Per-element UI control**. Composite-key matcher pinning by `state_key + uv_bucket + screen_context + bbox_quadrant + sort_key + widget_name_hash`. Click-to-pick, gizmo overlay, ini persistence, auto-grouping. Cross-screen Specialize fix dropped `screen_context` from pinning so the same texture role matches across screens. ([research/findings/sprite-per-element-architecture.md](research/findings/sprite-per-element-architecture.md))
- **World-space debug overlays** (NEW). 3D-projected via the engine's view+proj matrices, drawn through ImGui's foreground draw list:
  - **Trigger-box overlay** ([trigger_overlay.cpp](src/mtr-asi/src/trigger_overlay.cpp)) — wireframe AABBs with homogeneous parametric clip in clip space.
  - **NPC overlay Phase 1** ([npc_overlay.cpp](src/mtr-asi/src/npc_overlay.cpp)) — walks `dword_724DE4` transform list, reads entity name + pos, draws labels at projected screen coords. Shared math: [include/mtr/overlay_math.h](src/mtr-asi/include/mtr/overlay_math.h).
- **Autonomous validation pipeline** (NEW). The mod can launch the game itself, drive engine state via DI keypress injection, exercise a feature, write structured per-frame log lines, and shut down. An offline PowerShell validator re-runs the projection math and asserts correctness within a fixed pixel tolerance. Crash-handler integration writes a result-JSON sentinel + minidump on SEH so engine crashes surface as clear failures rather than launch failures. Usage: `pwsh tools/run-test.ps1 -Scenario <name> -Redeploy`. Detail: [docs/AUTONOMOUS_TESTING.md](docs/AUTONOMOUS_TESTING.md).
- **Crash handler** ([crash_handler.cpp](src/mtr-asi/src/crash_handler.cpp)) — `SetUnhandledExceptionFilter` installed in DllMain, writes minidump (`Game/mtr-asi-crash-*.dmp`) + `[CRASH]` log line + scenario result-JSON sentinel for any unhandled SEH.
- **Freecam smoothness fix**. Replaced `GetTickCount64` (15.625 ms resolution = choppy at 240 Hz) with `QueryPerformanceCounter` for sub-frame-accurate dt.

Full per-shipment notes: [research/findings/](research/findings/) (sorted by date suffix).

---

## Status (2026-05-04)

| Component | State |
|---|---|
| `Wilbur.exe` unpack | **Done and complete.** SecuROM 7 stub fully reverse-engineered ([research/findings/securom7-stub-re.md](research/findings/securom7-stub-re.md)) — just aPLib compression + BCJ-86 byte filter + custom IAT resolver, no real encryption. PE-sieve dump at `ida/dumps/process_22276/400000.Wilbur.exe` is 100% complete; `ida/400000.Wilbur.exe.i64` has 12,555 functions, Hex-Rays works. **Standalone `Wilbur_unpacked.exe` built** via `python tools/build_standalone_exe.py` — 4-byte patch (AddressOfEntryPoint dword), 30 seconds, no GUI, no Scylla. Verified via `tools/verify_unpacked_pe.py`. |
| `Launcher.exe` reverse engineering | **Done.** 170+ functions named, [research/findings/launcher-internals.md](research/findings/launcher-internals.md). |
| ImGui mod menu (Insert key, F12 screenshots) | **Done.** Hooked through D3D9 EndScene. Polling input bypasses DirectInput exclusive. |
| Native-resolution rendering | **Done — no crutches.** `GetCommandLineA/W` hook rewrites/injects `-dxresolution=NATIVE` so the game initialises at monitor size from boot. [research/findings/native-resolution-fix.md](research/findings/native-resolution-fix.md) |
| Aspect-ratio override (live, 4:3 / 16:10 / 16:9 / 21:9 / Custom) | **Done — no crutches.** Two function-level hooks substitute aspect at the game's projection-matrix builder + invalidate the camera cache each frame. [research/findings/aspect-ratio-fix.md](research/findings/aspect-ratio-fix.md) |
| Borderless windowed | **Done.** `dxwrapper` `FullscreenWindowMode=1, WindowModeBorder=0` + native backbuffer. |
| Logo intro skip | **Done (rename workaround).** `tools/skip_intros.py` renames the five logo `.BIK` files; in-game cutscenes untouched. Runtime `BinkOpen` hook designed and documented in [research/findings/bink-integration.md](research/findings/bink-integration.md), not yet implemented. |
| Stable shutdown (no zombie / mutex leak) | **Done.** `DllMain DLL_PROCESS_DETACH` is a no-op when `lpvReserved != NULL` (process termination); avoids loader-lock contention. `tools/run_game.py` also cleans up any stale zombies before each launch. |
| Frustum culling at 16:9 (slight edge popping) | **Root cause located, one-line fix prepared, NOT applied.** `game_camera_build_view_frustum` (`0x004DF2C0`) takes aspect via `*(camera+12)` but only rebuilds when `*(camera+144)` is dirty — `hk_CameraCompute` currently dirties only `+112`. See [aspect-ratio-fix.md](research/findings/aspect-ratio-fix.md) and [known-issues.md §8](research/findings/known-issues.md). |
| FPS limiter | **Designed, not implemented.** Hook point identified (`hk_EndScene` we already own); engine clock confirmed time-based (low risk of game-speed regression). Full design + sleep strategy + validation plan in [research/findings/frame-pacing.md](research/findings/frame-pacing.md). dxwrapper's `LimitPerFrameFPS` is the fallback. |
| Russian translation integration | Not started. |
| Debug features / hidden flags catalog | **Discovered 2026-05-05.** 8 game-specific `-dxXXX` flags + `-letitsnow` Easter egg, complete `Scn_Cheats_*` unlock infrastructure (ChargeBall courts/opponents, ConceptArt, TransmogrifierRecipes), FreeCam + Wireframe + ToggleDebugPanel, full FirstPerson mode parameters, in-game teleport menu, four-level Verbosity log system. Catalog: [research/findings/debug-features.md](research/findings/debug-features.md). |
| Engine console / REPL | **Scaffolding fully present, restoration planned.** Avalanche shipped a complete cvar console (processor `console_set_context_cmd` at `0x00588210`, print sink `console_printf` at `0x005873A0`, full output text cluster) but never wired the input UI. Restorable by attaching ImGui input to existing engine API: ~5-8 hours work. Plan in [research/findings/debug-features.md §Console restoration](research/findings/debug-features.md). |

Full living list: [research/findings/known-issues.md](research/findings/known-issues.md).

---

## Architecture in one paragraph (current, 2026-05-10)

Wilbur.exe is a 2007 D3D8 game. We route the rendering through DXVK's `d3d8.dll → d3d9.dll → Vulkan` pipeline (vendored in `Game/` from a built DXVK binary; configured via `Game/dxvk.conf`). The mod is loaded as `mtr-asi.asi` via Ultimate ASI Loader (`Game/dinput8.dll`). In `DllMain` it installs the crash handler, the cmdline rewriter, the cvar registration logger, and an early hook on `Direct3DCreate9` so when the engine's `WinMain` reaches D3D init the CreateDevice vtable is already hooked (required for cold-launch overrides like MSAA / windowmode to apply). After `DllMain` returns, a worker thread installs the rest: ~30 modules covering camera/projection/sprite-batcher hooks, ImGui menu, sim/render decouple, world-space debug overlays (trigger boxes + NPC labels), per-element UI control, and an autonomous test harness.

The runtime overlay layer (ImGui menu + per-frame foreground-draw-list overlays) sits at `EndScene` after the engine has rendered the 3D scene; the menu, FPS overlay, trigger boxes, and NPC labels all share the same `ImGui::NewFrame` → `Render` lifecycle. Engine matrix globals are read at fixed VAs (`view@0x00724C10`, `world@0x00724C50`, `proj@0x00745AA0`) and are SEH-guarded.

Detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/AUTONOMOUS_TESTING.md](docs/AUTONOMOUS_TESTING.md). Migration history: [research/findings/dxvk-migration-plan-2026-05-08.md](research/findings/dxvk-migration-plan-2026-05-08.md). Original dxwrapper integration is retired but the doc is kept for context: [research/findings/dxwrapper-integration.md](research/findings/dxwrapper-integration.md).

---

## Repository layout

```
.
├── docs/                  Architecture, RE workflow, roadmap, SecuROM notes
├── research/
│   └── findings/          Per-topic writeups
│       ├── aspect-ratio-fix.md       Live aspect override; two-hook architecture; cull root cause
│       ├── bink-integration.md       BinkOpen hook design (replaces rename intro skip)
│       ├── frame-pacing.md           Main loop + engine clock + FPS limiter design
│       ├── dinput-cleanup.md         DirectInput hook chain to release exclusive cooperative level
│       ├── full-unpack-procedure.md  Build a runnable static Wilbur_unpacked.exe via Scylla
│       ├── securom7-stub-re.md       Full RE of the SecuROM 7 stub (aPLib + BCJ + IAT resolver, no encryption)
│       ├── debug-features.md         Hidden launch flags, cheats, FreeCam, dev modes — catalogued from unpacked EXE
│       ├── native-resolution-fix.md  GetCommandLine hook approach
│       ├── imgui-menu-architecture.md
│       ├── launcher-internals.md     Launcher.exe RE
│       ├── dxwrapper-integration.md
│       ├── unpack-state.md           PE-sieve procedure
│       ├── known-issues.md           Living issue list
│       ├── lessons-learned.md        Cross-session meta-notes
│       ├── symbol-table.md           IDA name ↔ our name ↔ address map
│       ├── pe-analysis.md
│       ├── offsets.md                Offsets / RVAs we rely on
│       └── iat-anchors.md            IAT layout notes
├── ida/                   IDA Pro databases (.i64). Gitignored.
├── src/
│   └── mtr-asi/           The ASI mod (CMake + MinHook + ImGui)
│       ├── third_party/
│       │   ├── minhook/   git submodule
│       │   └── imgui/     git submodule (v1.91.5)
│       ├── include/mtr/   public headers
│       └── src/
│           ├── dllmain.cpp        synchronous cmdline + aspect init
│           ├── log.cpp            shared-read log
│           ├── cmdline_hook.cpp   GetCommandLine override
│           ├── d3d9_hook.cpp      menu hooks + aspect hooks
│           ├── aspect_patch.cpp   atomic target storage
│           ├── menu.cpp           ImGui overlay
│           └── screenshot.cpp     backbuffer → BMP
├── third_party/
│   └── dxwrapper/         git submodule, built from source
├── tools/                 Helper scripts (Python). See tools/README.md.
├── Game/                  Local install (gitignored — copyrighted binaries).
│   └── run.bat            Double-click to clean + deploy + launch.
└── reference/             External reference material (WSGF guide etc.)
```

---

## Required tooling

| Tool | Purpose |
|---|---|
| **IDA Pro 9.x** + Hex-Rays | Disassembly / decompilation of unpacked Wilbur.exe. |
| **[ida-pro-mcp](https://github.com/mrexodia/ida-pro-mcp)** | Bridge IDA ↔ Claude Code via MCP. Endpoint `http://127.0.0.1:13337/sse`. |
| **CMake 3.20+** + **Visual Studio 2019/2022/2026 BuildTools** | Build the ASI mod and dxwrapper. x86 (32-bit) target. |
| **Python 3.11+** | The `tools/` scripts. Stdlib only; no `pip install` step. |
| **[hasherezade/pe-sieve](https://github.com/hasherezade/pe-sieve)** | Initial unpack of SecuROM-protected Wilbur.exe. Already done — see [research/findings/unpack-state.md](research/findings/unpack-state.md). |

All in-mod dependencies (MinHook, ImGui, dxwrapper) are vendored as git submodules.

---

## Quick start: build + deploy

```powershell
# 1. Clone with all submodules
git clone --recurse-submodules <repo>
# (or: git submodule update --init --recursive)

# 2. Build dxwrapper (D3D8→D3D9 translator; one-time, takes a few minutes)
# Note: may need toolset v143→v145 patch on VS2026 BuildTools.
# See research/findings/dxwrapper-integration.md.
msbuild third_party\dxwrapper\dxwrapper.sln /m /p:Configuration=Release /p:Platform=Win32

# 3. Build mtr-asi mod
cmake -S src/mtr-asi -B src/mtr-asi/build -A Win32
cmake --build src/mtr-asi/build --config Release

# 4. Initial Game/ setup (one time)
Copy-Item third_party\dxwrapper\bin\Release\dx8\d3d8.dll       Game\d3d8.dll      -Force
Copy-Item third_party\dxwrapper\bin\Release\dx8\dxwrapper.dll  Game\dxwrapper.dll -Force
Copy-Item third_party\dxwrapper\bin\Release\dx8\dxwrapper.ini  Game\dxwrapper.ini -Force
# Edit Game/dxwrapper.ini per the flags below.
```

Required `Game/dxwrapper.ini` flags (rest defaulted):

```ini
[Compatibility]
D3d8to9                    = 1
EnableD3d9Wrapper          = 1

[d3d9]
EnableWindowMode           = 1
FullscreenWindowMode       = 1
WindowModeBorder           = 0
```

For day-to-day: **double-click `Game\run.bat`** — it deploys the latest mod build, kills any zombie, and launches Wilbur.exe directly (bypassing the launcher's mutex check).

```
Game\run.bat              # default: direct launch (skips launcher mutex)
Game\run.bat launcher     # launch via Launcher.exe
Game\run.bat clean        # only clean state, no launch
Game\run.bat deploy       # clean + deploy fresh asi, no launch
```

In-game: **Insert** opens the mod menu. **F12** captures a screenshot to `Game/screenshots/`.

---

## Helper scripts

In `tools/` (all Python). Full inventory in [tools/README.md](tools/README.md).

| Script | Role |
|---|---|
| `run_game.py` | Clean state + deploy fresh `.asi` + launch (direct, via launcher, or just clean). Used by `Game/run.bat`. |
| `find_mutex_holder.py` | Walk kernel handle table to find which process holds a named mutex. Standalone for diagnosis. |
| `find_aspect_rva.py` | Locate `AB AA AA 3F` (4:3 float) candidates in the unpacked Wilbur.exe dump. |
| `patch_aspect.py` | Disk-patch fallback (WSGF approach — kept around but doesn't work on this build). |
| `skip_intros.py` | Rename logo `.BIK`s to `*.intro_skip` (skip the pre-menu logos). Restorable. |

---

## Workflow & conventions

- **Two-channel rule**: anything code-level (function names, types, comments) goes in `ida/*.i64`. Anything narrative (why-it-works, dead-ends, cross-cutting notes) goes in `research/findings/*.md`. The cross-reference between the two is `research/findings/symbol-table.md`. They don't drift apart.

- **No game binaries committed**: `Game/`, `*.exe`, `*.i64`, `*.idb`, `_inspect/`, `build/` are all gitignored.

- **No crutches**: classify each fix as crutch (workaround) vs root-cause (binary patch / param substitution / source-level fix) before coding. See [research/findings/lessons-learned.md L0](research/findings/lessons-learned.md).

- **Verify before patching**: dead orphan functions (no xrefs anywhere; or branches gated by always-false conditionals) waste full iterations. [lessons-learned.md L3 / L9](research/findings/lessons-learned.md).

- **Use established libraries**: MinHook, ImGui, dxwrapper, CMake. Don't reinvent the wheel.

[docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md), [docs/ida-workflow.md](docs/ida-workflow.md) for naming and process details.

---

## License & legal

This repository contains **no game binaries** and **no copyrighted assets**. Reverse-engineering artefacts (function names, comments, structure layouts) are original research notes for interoperability and preservation purposes. The ASI mod and helper scripts are original code that operates on a user-owned binary at runtime. Vendored libraries retain their original licenses (MinHook MIT, ImGui MIT, dxwrapper MIT).
