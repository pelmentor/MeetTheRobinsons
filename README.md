# Meet the Robinsons (2007 PC) — Reverse Engineering & ASI Mod

Research-grade reverse engineering of the 2007 PC port of *Disney's Meet the Robinsons*
(Avalanche Software / Buena Vista Games), with a working ASI mod that fixes
the original engine's display limitations on modern hardware.

> **Purpose:** game preservation, technical research, quality-of-life modding.
> All work targets a legally owned retail copy of the game.

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

## Architecture in one paragraph

Wilbur.exe (D3D8 game) imports `d3d8.dll`. Our `Game/` directory has a hijacked `d3d8.dll` (`dxwrapper`) that translates D3D8 → D3D9. dxwrapper also acts as the .asi loader and pulls in `mtr-asi.asi`. Our mod hooks `GetCommandLineA/W` synchronously in `DllMain` (rewrite resolution before CRT reads it), then in a deferred thread hooks D3D9 vtables (`IDirect3D9::CreateDevice` for diagnostics, `IDirect3DDevice9::EndScene/Reset` for ImGui menu) and two game-side functions in Wilbur.exe at fixed VAs (`0x00562B20` for projection-matrix construction, `0x00564600` for cache invalidation — these own the live aspect-ratio override). Both proxies (dxwrapper's d3d8 stub and our mod) coexist because dxwrapper does the translation and we hook D3D9 downstream of it.

Detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [research/findings/dxwrapper-integration.md](research/findings/dxwrapper-integration.md).

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
