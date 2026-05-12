# Architecture

How the moving parts of this project relate to each other.
For *what* we're trying to achieve, see [ROADMAP.md](ROADMAP.md). For *the game's*
internal architecture as we understand it, see [research/findings/](../research/findings/).

---

## Guiding principles (architectural philosophy)

Distilled from MTA's README (cloned at [reference/mtasa-blue/](../reference/mtasa-blue/)).
MTA has 22+ years of "retrofit multiplayer onto a single-player game via
mod hooks" experience; the principles below are the load-bearing ones for
mtr-asi too, adapted to our scope (2P LAN coop through MTR's existing
single-player content, not a multiplayer platform with scripting).

### 1. No modification of original game files

> *"Multi Theft Auto is based on code injection and hooking techniques
> whereby the game is manipulated without altering any original files
> supplied with the game."* — MTA README

Concretely for mtr-asi:
- `Wilbur.exe` is untouched on disk. All changes are at runtime via
  `dinput8.dll` ASI loader injection + MinHook / vtable patches.
- Engine VAs (function and data addresses) are read via the disassembly
  but referenced from our code only — never patched on disk.
- This rule extends to game data files: no `.sx` script edits, no `.dbl`
  rewrites, no asset substitution on disk. Everything is in-memory.

### 2. Engine-extension paradigm — we ARE a game engine on top of theirs

> *"The software functions as a game engine that installs itself as an
> extension of the original game, adding core functionality such as
> networking and GUI rendering while exposing the original game's engine
> functionality."* — MTA README

Concretely for mtr-asi:
- mtr-asi is not "a few hooks that change behaviour"; it's an extension
  game engine layered on top of MTR. It owns: rendering hooks, input
  routing (eventually), networking (eventually), UI (ImGui menu), per-tick
  decoupling logic, save/load (eventually).
- Subsystems inside mtr-asi own their lifecycle: install once at DLL load,
  tear down at unload. Internal modules talk to each other via clean APIs
  (e.g. `coop_spawn_probe::live_orphan_entity()`), not by reaching into
  globals.
- The engine extension stays **behind well-defined boundaries** with the
  base engine: hook here, patch there, otherwise stay out.

### 3. Parallel class hierarchy mirroring the engine's structures

> *"Since the class design of our game framework is based upon Grand Theft
> Auto's design, we are able to insert our code into the original game."*
> — MTA README ("Blue framework" concept)

MTA built `CClientPed` to wrap GTA's `CPed` (verified at
`reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.h:629` —
`CPlayerPed* m_pPlayerPed`). The MTA-side class owns network state,
interp, lifecycle; the engine-side ped (constructed via the real engine
ctor) owns rendering and animation. The two **point at each other**:
`CClientPed::m_pPlayerPed` → engine ped, and the engine ped's
`m_pStoredPointer` slot is set back to the CClientPed via
`SetStoredPointer(this)` (CClientPed.cpp:3641, 3766). Remote-player
flavour is a subclass: `CClientPlayer : public CClientPed`
(CClientPlayer.h:32).

Concretely for mtr-asi (looking forward to coop Phase 2+):
- Future `MtrRemotePlayer` class will own: network-supplied position/state,
  interp state, per-player input buffer, lifecycle vs the remote peer.
- The engine-side `orphan` wilbur entity (constructed via
  `entity_factory_construct`) owns: rendering, animation, the engine's
  per-frame tick.
- The two are connected by an MTR-side pointer (`MtrRemotePlayer.m_engine_entity
  → orphan`), mirroring `CClientPed::m_pPlayerPed → CPlayerPed*`. Whether
  to add a back-pointer from the engine entity to MtrRemotePlayer (MTA-style
  via a free slot like `m_pStoredPointer`, or a sidecar map if no free
  slot exists on MTR's wilbur layout) is a Phase 1 design decision.

### 4. Targeted crash fixes, not broad suppression

> *"The game is then heavily extended by providing new game functionality
> (including tweaks and crash fixes) as well as a completely new graphical
> interface, networking and scripting component."* — MTA README

MTA's "crash fixes" are SPECIFIC byte patches at SPECIFIC engine VAs
where the single-player code makes assumptions that break under multiplayer
(e.g. `0x609FF2` force-singleton on player-info getter,
`0x541DD0` → `RET` to disable native pad reader). Each fix is targeted at
one call site or one assumption.

Concretely for mtr-asi (the b7.10/b7.11 lesson):
- **Avoid broad suppression workarounds** like the `coop_orphan_filter` —
  unlinking 18/40 subscribers from a list-walker masks 18 different crashes
  with one mechanism. That's an architectural anti-pattern.
- **Each crash site gets its own targeted fix.** Patch the call site,
  populate the missing state, or route the data — whatever fits at that
  specific location.
- Example (current): `sub_4CD7B0 → sub_58D330(NULL)` should be a targeted
  patch or route at exactly that call, not a broader subscriber filter.

### 5. Minimum viable subset

> *"By default, Multi Theft Auto provides the minimal sandbox style
> gameplay of Grand Theft Auto."* — MTA README

MTA intentionally doesn't support every SP feature in multiplayer (missions
don't tick, save game is dead code). They picked the minimum game-state
that's useful and made *that* work well.

Concretely for mtr-asi (coop scope decision):
- Coop scope is "two players walk + interact through the existing levels".
  Not: split-screen, not: networked mini-games, not: shared inventory
  resolution, not: shared save game.
- Per-player state coverage should be the minimum needed to satisfy the
  scope, expanded only when scope expands.
- This rule guides every "should we replicate X?" question: only if X is
  in scope.

### 6. Augment SP, never replace it

> MTA installs `HOOK_CRunningScript_Process` at `0x469F00` to stub mission
> script ticking — they intentionally don't replicate SP content.
> See `reference/mtasa-blue/Client/multiplayer_sa/CMultiplayerSA.cpp:633,
> 3657-3784`.

For mtr-asi, the same caution applies in reverse: we're **adding** coop
to SP, so SP scripts MUST keep ticking. We must hook *minimally* on the
SP-content paths; never replace, only augment. Where coop and SP meet
(e.g. a cutscene that assumes one wilbur), prefer per-player routing
within the SP system to bypassing SP wholesale.

**Concrete enforcement trigger:** any hook that conditionally skips an
SP path based on player-id is a principle-6 violation UNLESS the SP path
was also dead-code in SP (i.e. you're not skipping live SP behaviour).

### 7. Engine-wrapper layer ≠ gameplay/network layer

> MTA splits `Client/game_sa/` (engine-side wrappers, one C++ class per
> engine class, no network/gameplay logic) from
> `Client/mods/deathmatch/logic/` (gameplay state, network packets,
> scripting, interp). The boundary is what lets either side change
> without breaking the other.

For mtr-asi (forward-looking — current `src/coop/` mixes both somewhat):

- **Engine-wrapper layer** owns: entity struct layouts, vtable thunks,
  registry primitives (`sub_5CB420` callers, `+0xCCC` walkers), `+0xD04`
  list walkers, engine-VA constants. These files dereference Wilbur VAs
  but hold NO network state.
- **Gameplay/network layer** owns: `MtrRemotePlayer`, packet
  serialisation, input buffers, interp, lifecycle vs the network peer.
  These files hold per-player state but DON'T dereference engine VAs
  directly — they call into the engine-wrapper layer for that.

**Concrete enforcement trigger:** a new file under `src/coop/` that both
(a) dereferences engine VAs AND (b) holds per-player network state is a
principle-7 violation — split it into two files.

Mapping current modules to the (post-refactor) layers:
- Engine-wrapper layer: `coop_spawn_probe`, `coop_registry_mirror`,
  `coop_orphan_filter` (until deleted), `controlmapper_probe`,
  `protagonist_registry`, `viewdriver_tick_probe`.
- Gameplay/network layer: (none yet — `MtrRemotePlayer` + manager land in
  Phase 1).

---

## High-level system view

```
┌─────────────────────────────────────────────────────────────────────┐
│                      Reverse engineering loop                        │
├─────────────────────────────────────────────────────────────────────┤
│   Claude Code (CLI)  ◄──MCP/SSE──►  IDA Pro 9.2 + ida-pro-mcp        │
│         │                                  │                          │
│         ▼                                  ▼                          │
│   docs/ + research/                  ida/*.i64 (DB)                   │
│   (narrative)                        (code-level facts)               │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼ (informs hook addresses, types,
                                       memory layouts)
┌─────────────────────────────────────────────────────────────────────┐
│                          Game runtime stack                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  Wilbur.exe (D3D8 game, x86; on-disk SecuROM-7 packed, runtime is     │
│              plain x86 once the loader's aPLib stub finishes)         │
│   │                                                                   │
│   ├──imports──► d3d8.dll  ─── (in Game/) dxwrapper stub               │
│   │                          │                                        │
│   │                          └──loads──► dxwrapper.dll                │
│   │                                       │                           │
│   │                                       └──translates D3D8──►       │
│   │                                                                   │
│   ├──imports──► dinput8.dll ─── (in Game/) Ultimate ASI Loader        │
│   │                          │                                        │
│   │                          └──loads─►  mtr-asi.asi  (this project)  │
│   │                                       │                           │
│   │                                       └──hooks via vtable patch   │
│   │                                          (MinHook on real d3d9)   │
│   │                                                                   │
│   └─────►  System d3d9.dll                                            │
│              ▲                                                         │
│              │ vtables hooked here:                                   │
│              │   IDirect3D9::CreateDevice          (vtable[16])        │
│              │   IDirect3DDevice9::Reset           (vtable[16])        │
│              │   IDirect3DDevice9::EndScene        (vtable[42])        │
│              │   IDirect3DDevice9::SetTransform    (vtable[44])        │
│              │                                                         │
│   dxwrapper's d3d8to9 internally calls these                           │
│                                                                       │
└─────────────────────────────────────────────────────────────────────┘
```

---

## The reverse-engineering loop

Driven by **Claude Code + IDA Pro MCP**:

- The user opens a binary's `.i64` database in IDA Pro.
- The `ida-pro-mcp` plugin exposes a JSON-RPC server over SSE at
  `http://127.0.0.1:13337/sse`.
- Claude can: list/decompile/disassemble functions, look up xrefs, set names,
  set comments, define types, inspect bytes — everything needed to annotate
  the database collaboratively. See [ida-workflow.md](ida-workflow.md).
- Findings are persisted in two places:
  - **`ida/*.i64`** — function names, types, comments. Authoritative for
    code-level details. Functions are renamed from `sub_XXXXXX` to descriptive
    names like `game_GetCameraAspect`, `game_MainWndProc` etc. as we identify them.
  - **`research/findings/*.md`** — narrative explanations, why-it-works, cross-cutting
    notes, dead-end analysis. Authoritative for *understanding*.

Don't let one drift from the other.

---

## The runtime stack (current state, 2026-05-10)

> **2026-05-08+ migration: dxwrapper → DXVK.** The original stack used dxwrapper
> (D3D8 → D3D9 translation via the system d3d9.dll). The current stack routes
> Wilbur's D3D9 calls through DXVK (d3d9 → Vulkan). dxwrapper has been retired;
> the section below describing it is kept for historical context. Current
> entry points: [research/findings/dxvk-migration-plan-2026-05-08.md](../research/findings/dxvk-migration-plan-2026-05-08.md),
> [research/findings/dxvk-call-inventory-2026-05-09.md](../research/findings/dxvk-call-inventory-2026-05-09.md),
> and `Game/dxvk.conf` (tuned config: `samplerAnisotropy=16`, `forceMipmapLodBias=-0.5`,
> `invariantPosition=True`, `cachedDynamicBuffers=True`, `floatEmulation=Strict`,
> `maxFrameLatency=1`). MSAA is applied at the API level via [src/mtr-asi/src/msaa.cpp](../src/mtr-asi/src/msaa.cpp)
> rather than via DXVK's swapchain-MSAA option (avoids HUD blur).

## The runtime stack (legacy, kept for historical context)

The game is D3D8. Modern Windows still has `d3d8.dll`, but it's slow on modern
GPUs, and its API has gaps that cause issues with old games (cooperative-fullscreen
oddities, refresh rate handling, etc.). So we route D3D8 through **dxwrapper**,
which translates to D3D9, and hook the D3D9 layer for our mod functionality.

### 1. dxwrapper (`Game/d3d8.dll` + `Game/dxwrapper.dll`)

[`elishacloud/dxwrapper`](https://github.com/elishacloud/dxwrapper), open-source
MIT, vendored at `third_party/dxwrapper/`, built from source as part of our
build process.

What it does:
- Wraps `d3d8.dll` exports. When Wilbur.exe imports `d3d8.dll`, our stub
  (a renamed `Game/d3d8.dll`) is loaded instead of the system one and forwards
  to dxwrapper.
- Bundles `crosire/d3d8to9` to translate every D3D8 call to D3D9 using the real
  system `d3d9.dll`.
- Provides windowing options (`EnableWindowMode`, `FullscreenWindowMode`,
  `WindowModeBorder`) configured via `Game/dxwrapper.ini`.

Our config:
```ini
[Compatibility]
D3d8to9                    = 1
EnableD3d9Wrapper          = 1

[d3d9]
EnableWindowMode           = 1
FullscreenWindowMode       = 1
WindowModeBorder           = 0
```

See [research/findings/dxwrapper-integration.md](../research/findings/dxwrapper-integration.md)
for build instructions and config rationale.

### 2. Ultimate ASI Loader (`Game/dinput8.dll`)

Generic ASI plugin loader from [ThirteenAG](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
Deployed as a `dinput8.dll` proxy (Wilbur.exe imports `dinput8.dll!DirectInput8Create`,
loader gets in front, scans Game/ for `*.asi` files, LoadLibrary's each).

We deploy the official binary as-is, no source/build.

### 3. mtr-asi.asi (this project, `Game/mtr-asi.asi`)

Our mod. Built from `src/mtr-asi/`. Activates from `DllMain(DLL_PROCESS_ATTACH)`:

```cpp
DllMain DLL_PROCESS_ATTACH:
   CreateThread(mtr_init)

mtr_init:
   log::init() → Game/mtr-asi.log
   MH_Initialize()
   d3d9hook::install():
       defer until d3d9.dll loaded by dxwrapper
       create dummy IDirect3D9 + dummy IDirect3DDevice9
       grab vtables (shared module-wide in d3d9.dll)
       MH_CreateHook on:
           IDirect3D9::CreateDevice           [16]
           IDirect3DDevice9::Reset            [16]
           IDirect3DDevice9::EndScene         [42]
           IDirect3DDevice9::SetTransform     [44]
       release dummy device + window
       MH_EnableHook(MH_ALL_HOOKS)

In CreateDevice hook:
   override pp.BackBufferWidth/Height to monitor size
   SetWindowPos(hFocusWindow, monitor dims)
   forward to original

In EndScene hook:
   poll input via GetAsyncKeyState/GetCursorPos
     → io.AddXxxEvent (bypasses DirectInput exclusive)
   if first call: ImGui_ImplWin32_Init / ImGui_ImplDX9_Init
   ImGui::NewFrame → menu UI → ImGui::Render → ImGui_ImplDX9_RenderDrawData

In Reset hook:
   ImGui_ImplDX9_InvalidateDeviceObjects
   override pp + SetWindowPos again
   forward to original
   ImGui_ImplDX9_CreateDeviceObjects

In SetTransform hook:
   if state == D3DTS_PROJECTION and widescreen_enabled:
       matrix._11 *= 0.75f      // 4:3 → 16:9 horizontal FOV widening
   forward to original
```

### Coexistence

The two proxies (`d3d8.dll` for dxwrapper, `dinput8.dll` for ASI loader) wrap
**different DLLs**, so they coexist. dxwrapper README explicitly mentions
Ultimate ASI Loader compatibility. Tested working as of 2026-05-04.

---

## Why these specific tools

| Need                              | Choice                          | Why not alternatives                                |
|-----------------------------------|---------------------------------|------------------------------------------------------|
| ASI loader                        | ThirteenAG Ultimate ASI Loader  | Battle-tested, supports multiple proxy targets       |
| Inline hooks                      | TsudaKageyu MinHook             | Tiny, MIT, the *de facto* choice for x86/x64 game mods |
| D3D8 → modern translation         | elishacloud dxwrapper           | dgVoodoo is closed-source + archived; this is open-source equivalent |
| In-game UI                        | Dear ImGui (ocornut/imgui)      | Standard for game mods; backends for both DX9 (we use) and Win32 |
| Decompiler / disassembler         | IDA Pro 9.2 + Hex-Rays          | Already had license; ida-pro-mcp gives us programmatic access |
| Initial unpack                    | hasherezade pe-sieve            | Cleanest output for SecuROM-protected exe; bulletproof |

---

## Source layout (`src/mtr-asi/`, current state 2026-05-10)

The mod has grown substantially. Snapshot of the modules (~50 .cpp files, ~620KB final binary). Each module is independently installable from `dllmain::mtr_init`; install order matters for some (notably `d3d9hook::install_early()` must run from DllMain BEFORE the engine calls Direct3DCreate9 — otherwise CreateDevice-time overrides like MSAA / windowmode miss the cold launch).

```
src/mtr-asi/
├── CMakeLists.txt
├── third_party/                    minhook, imgui (submodules)
├── include/mtr/                    public headers per module
│   ├── crash_handler.h, msaa.h, windowmode.h, ...
│   ├── overlay_math.h              shared projection math (trigger + npc overlays)
│   ├── trigger_overlay.h, npc_overlay.h
│   └── widget_probe.h, sim_decouple.h, ...
└── src/
    ├── dllmain.cpp                 DllMain → install_early hooks + crash_handler;
    │                                spawn mtr_init thread for the rest
    ├── log.cpp                     line-based logger (flushed each line); single
    │                                Game/mtr-asi.log, truncated each launch
    ├── crash_handler.cpp           SetUnhandledExceptionFilter → minidump +
    │                                [CRASH] log line + result-JSON sentinel
    │                                (so run-test.ps1 sees "result":"crash")
    │
    ├── d3d9_hook.cpp               IDirect3D9::CreateDevice + Reset + EndScene
    │                                vtable hooks. install_early() hooks
    │                                Direct3DCreate9 itself from DllMain so
    │                                CreateDevice is hooked BEFORE the engine
    │                                creates its real device.
    ├── d3d9_hook/camera_hooks.cpp  per_camera_apply, build_proj_matrix,
    │                                build_frustum, vis_test, freecam apply
    ├── d3d9_hook/sprite_hooks.cpp  WrapSetTransform, sprite-batcher matrix,
    │                                D3DTS_PROJECTION logger
    ├── d3d9_hook/diag_hooks.cpp    SetClipPlane (diag), SetRenderState
    │                                (CLIPPLANEENABLE filter + fog disable)
    │
    ├── windowmode.cpp              borderless-fullscreen rewrite at
    │                                CreateDevice/Reset; ChangeDisplaySettings
    │                                blocker
    ├── msaa.cpp                    NEW (2026-05-09). Multisample override at
    │                                CreateDevice/Reset with cap-check fallback
    │                                (16→8→4→2→NONE). Default ON @ 16x.
    │                                Routes through DXVK → Vulkan native MSAA.
    ├── aspect_patch.cpp            aspect-ratio override + draw_dist + fov
    │
    ├── menu.cpp                    ImGui frame lifecycle (NewFrame inside
    │                                EndScene PRE; Render gated by `any_draw`
    │                                which OR-aggregates every visible-output
    │                                module)
    ├── menu/menu_helpers.cpp       common ImGui helpers
    ├── menu/tab_camera.cpp         tab_picture.cpp tab_world.cpp
    ├── menu/tab_performance.cpp    tab_tools.cpp tab_debug.cpp
    ├── menu/menu_fps_overlay.cpp   small frame-time overlay
    │
    ├── console.cpp                 in-game cvar console (F2)
    ├── screen_push.cpp             screen-stack mirror + push/pop tracking
    │                                + screen-class registry capture
    ├── screenshot.cpp              backbuffer→BMP capture on F12 / request()
    │                                (post-processed to PNG by run-test.ps1
    │                                via tools/bmp-to-png-thumb.ps1)
    ├── input_hook.cpp              GetAsyncKeyState polling for menu input
    │                                under DI-exclusive
    ├── dinput_hook.cpp             GetDeviceState detour; suppresses input
    │                                when menu open AND can INJECT keypresses
    │                                (menu-driving from inside the mod)
    ├── cmdline_hook.cpp            GetCommandLineA/W rewriter
    │                                (-dxresolution → native; -letitsnow inject)
    ├── cvar_dump.cpp               cvar registration logger
    │
    ├── ui_aspect_rules.cpp         per-screen sprite-aspect rule store
    ├── sprite_probe.cpp            sprite-batcher entry distribution probe
    ├── sprite_split.cpp            sprite split-pass (HUD vs world separation)
    ├── sprite_xform.cpp            per-element sprite control (offsets/scale/
    │                                hides), composite-key matcher, ini I/O
    ├── sprite_picking.cpp          click-to-pick + gizmo overlay for sprite_xform
    │                                (uses WH_MOUSE_LL hook for clicks under
    │                                DI-exclusive)
    ├── widget_probe.cpp            sub_4E9350 PRE+POST naked-stub hook;
    │                                walks caller stack-frame for engine widget
    │                                m_pcName at +0x130; per-frame side-table
    │                                SpriteEntry* → widget_name
    ├── state_key_probe.cpp         asset-memory CSV dump (diagnostic)
    │
    ├── trigger_overlay.cpp         3D-projected wireframe AABB overlay
    │                                (homogeneous parametric clip in clip
    │                                space). Shipped 2026-05-09.
    ├── npc_overlay.cpp             NEW (2026-05-09 evening). Sister overlay:
    │                                walks dword_724DE4 transform list, reads
    │                                entity name (+0x50) + pos
    │                                (*(+0x48)+0x10 → fallback +0x58),
    │                                projects to screen, draws label via ImGui
    │                                foreground draw list.
    │
    ├── peripheral_cull_probe.cpp   per-object cull probe (sphere + corner-
    │                                AABB) with per-plane force-pass overrides
    │                                = "infinite draw distance"
    ├── vis_test_probe.cpp          orphan-function vis-test instrumentation
    ├── scene_vis_log.cpp           (scene+104) bit 0 writer logging
    │
    ├── freecam.cpp                 native freecam (F3); MMB-teleport with
    │                                entity_transform_tick skip-bit hold; QPC
    │                                dt source for sub-frame smoothness
    ├── level_select.cpp            screen-push direct-load helpers
    ├── fps_limit.cpp               spin-wait FPS cap
    ├── windowmode.cpp              (already listed above)
    │
    ├── sim_decouple.cpp            sim/render decouple aggregator
    ├── sim_decouple/sim_decouple_throttle.cpp
    ├── sim_decouple/sim_decouple_telemetry.cpp
    ├── interp.cpp                  view+world matrix snapshot infra
    ├── interp/interp_view.cpp      slerp+lerp on view matrix at render rate
    ├── interp/interp_player.cpp    player-entity transform interp (M4)
    ├── interp/interp_npc.cpp       NPC transform-list walker + interp (M5)
    ├── interp/interp_halo.cpp      halo-component follow fix (M3.3)
    ├── dt_correctness.cpp          flt_6FFCBC dt rewrite for 333Hz subsystems
    │                                + alt-pump dt correction
    │
    └── test_harness.cpp            in-mod scenario runner. Scenarios:
                                    boot-to-main-menu, widget-probe,
                                    verify-main-menu-visible, hold-at-menu,
                                    overlay-phase1-verify (trigger overlay),
                                    npc-overlay-phase1-verify (NPC overlay),
                                    load-save-1-show-ingame.
                                    Drives engine state via dinput_hook
                                    keypress injection; writes
                                    Game/mtr-asi-test-result.json on
                                    pass/fail/timeout/crash. With
                                    -mtrasi-coop-port=<N> on the cmdline,
                                    writes mtr-asi-test-result-<port>.json
                                    instead (Phase 0D of the coop plan).
```

Build:

```powershell
cmake -S src/mtr-asi -B src/mtr-asi/build -A Win32
cmake --build src/mtr-asi/build --config Release
```

Output: `src/mtr-asi/build/Release/mtr-asi.asi` (~620 KB as of 2026-05-10).

---

## DllMain → mtr_init flow (current state, 2026-05-10)

The early-install path is critical for cold-launch overrides (MSAA, windowmode):

```cpp
DllMain DLL_PROCESS_ATTACH:
    log::init() → Game/mtr-asi.log
    log::info("v0.2.0 attach")

    crash_handler::install()      // SetUnhandledExceptionFilter so any
                                   // subsequent fault writes a .dmp + a
                                   // [CRASH] log line + result-JSON sentinel

    MH_Initialize()
    cmdline::install()            // GetCommandLineA/W rewriter
    cvar_dump::install()          // hooks engine cvar registration BEFORE
                                   // WinMain registers cvars
    d3d9hook::install_early()     // hook Direct3DCreate9 in DXVK's d3d9.dll;
                                   // when engine calls Direct3DCreate9 in
                                   // WinMain, our hook installs CreateDevice
                                   // on the returned IDirect3D9's vtable —
                                   // BEFORE the engine calls CreateDevice

    aspect::init()                // patches the aspect constant in code
                                   // before main() runs

    CreateThread(mtr_init)        // rest of init runs after DllMain returns

mtr_init (background thread, runs concurrently with engine WinMain):
    windowmode::install()         // ChangeDisplaySettings blocker first
    d3d9hook::install()           // deferred: waits for d3d9.dll to be
                                   // resolved by the loader, then captures
                                   // device-side vtables (EndScene, Reset,
                                   // SetTransform, SetRenderState,
                                   // SetClipPlane). CreateDevice was already
                                   // hooked early.
    console::install(), screen_push::install()
    input_hook::install(), dinput_hook::install()
    vis_test_probe::install(), scene_vis_log::install()
    peripheral_cull_probe::install(), sprite_probe::install()
    widget_probe::install()
    ui_aspect_rules::install_defaults()
    sim_decouple::install(), dt_correctness::install()
    interp::install()
    freecam::install_transform_skip_hook()
    test_harness::install()       // last; reads -mtrasi-test=<name> from cmdline
    log::info("init thread done")
```

Critical: `d3d9hook::install_early()` runs INSIDE DllMain (before any thread spawn) so the Direct3DCreate9 hook is in place by the time the engine's WinMain reaches its D3D init sequence. The deferred `d3d9hook::install()` running from `mtr_init` is for the device-side vtables which only exist after the engine has created its device.

---

## EndScene per-frame flow (current state, 2026-05-10)

```cpp
hk_EndScene(dev):
    vis_test_probe::frame_tick()       // diagnostic counters
    scene_vis_log::frame_tick()
    peripheral_cull_probe::frame_tick()
    sim_decouple::on_render_frame()    // includes hk_camera_apply chain →
                                        // npc_apply_interp_for_render_frame
                                        // (writes interp pos to
                                        //  entity+0x58 / *(entity+0x48)+0x10)
    dt_correctness::tick_snapshot_log()

    menu::on_end_scene(dev):
        ImGui_ImplDX9_NewFrame()
        ImGui_ImplWin32_NewFrame()
        poll_input_to_imgui()
        console::poll_hotkey()
        freecam::tick()                // QPC dt; mouse-look + WASD; MMB-tp
        test_harness::tick()           // scenario state machine, if -mtrasi-test
        ImGui::NewFrame()
        if (g_visible) draw_menu()      // tabs: Camera/Picture/World/Performance/Tools/Debug
        console::draw()                 // F2 cvar console
        if (fps_overlay_enabled) draw_fps_overlay()
        trigger_overlay::tick(dev)      // wireframe AABB on foreground draw list
        npc_overlay::tick(dev)          // text labels at NPC world positions
        ImGui::EndFrame()
        if (any_draw)                   // any_draw OR-aggregates every output
                                        // module: menu/console/fps/triggers/npcs
            ImGui::Render() + RenderDrawData()
        screenshot::try_capture(dev)    // captures back buffer AFTER ImGui draws

    g_orig_EndScene(dev)
    fps_limit::tick()                  // spin-wait cap, post-EndScene
```

---

## World-overlay subsystem (NEW 2026-05-09)

Two modules sharing one math header. Both project entities/objects from world space to screen space using the engine's view + projection matrices, then draw via ImGui's foreground draw list.

| Module | Purpose | Reads from | Renders |
|---|---|---|---|
| [trigger_overlay.cpp](../src/mtr-asi/src/trigger_overlay.cpp) | Debug AABBs (trigger volumes, dev test boxes) | Hardcoded box list (Phase 1); future: entity walker filtered by `triggerbox`/`trigger_volume`/`triggeraoe` class | Wireframe via `AddLine` × 12 edges per box; homogeneous parametric clip per edge |
| [npc_overlay.cpp](../src/mtr-asi/src/npc_overlay.cpp) | Debug labels at NPC positions | Transform list `dword_724DE4` (entity at `node+0x5C`) | Text label + anchor dot via `AddText` + `AddCircleFilled`; full 6-plane point-frustum cull |

Shared math: [include/mtr/overlay_math.h](../src/mtr-asi/include/mtr/overlay_math.h) — `Vec4`, `row_mul`, `mat_mul`, `plane_dist` (all 6 D3D9 frustum planes), `clip_segment` (homogeneous parametric line clip), `point_in_frustum`, `ndc_to_screen` (with D3D9 Y-flip). Single source of truth so a sign-flip fix lands in both overlays at once.

Engine globals both overlays read:
- View matrix at `0x00724C10` (16 floats)
- World matrix at `0x00724C50` (= inverse(view); row 3 is `cam_pos` directly)
- Projection matrix at `0x00745AA0` (named `kHaloProjMatrixVA` in interp_halo.cpp)

Both overlays SEH-guard the matrix reads. The NPC overlay additionally SEH-wraps the entire walker body (entity mid-destruction can have valid `+0x48` pointer to freed sub-component).

Plan / audit history: [research/findings/trigger-box-overlay-plan-2026-05-09.md](../research/findings/trigger-box-overlay-plan-2026-05-09.md), [research/findings/npc-overlay-plan-2026-05-09.md](../research/findings/npc-overlay-plan-2026-05-09.md).

---

## Autonomous validation pipeline (NEW 2026-05-09)

The mod can validate its own feature work without a human at the keyboard. Used yesterday to validate the trigger overlay's projection + clip pipeline mathematically (60 frames, 180 edges, all within 0.5 px of independently computed values).

```
pwsh tools/run-test.ps1 -Scenario <name> -Redeploy
        │
        ▼
build mtr-asi.asi → deploy → kill any running Wilbur.exe → launch Wilbur.exe
with -mtrasi-test=<name> on the cmdline
        │
        ▼
test_harness::tick() (in-mod, called from EndScene per-frame):
   - drives engine state via dinput_hook keypress injection
     (DIK_RETURN to dismiss title screen, navigate menus)
   - watches screen_push::current_top_name() for target state
   - on reach: enable feature, arm export, take screenshot, drain N frames
   - on success: write Game/mtr-asi-test-result.json (scenario, result,
     elapsed_ms, frames, detail), log TESTHARNESS line, request_shutdown
   - on timeout: same JSON with "result":"timeout"
        │
        ▼
run-test.ps1 watchdog (250 ms poll):
   - JSON appeared → parse, override exit code (pass=0, fail=1, timeout=3,
     crash=1)
   - process exited without JSON → exit 4 (launch failure)
   - log file size hasn't grown for LogStallSec (default 20s) → kill, exit 2
   - hard timeout TimeoutSec exceeded → kill, exit 3
        │
        ▼
archive to tools/test-runs/<UTC-stamp>-<scenario>/:
   - mtr-asi.log (full session log)
   - mtr-asi-test-result.json (top-level pass/fail)
   - Wilbur_d3d9.log (DXVK log)
   - screenshots/*.bmp (full-resolution captures)
   - screenshots/*.png (1024-wide thumbnails via tools/bmp-to-png-thumb.ps1)
   - mtr-asi-crash-*.dmp (if SUEF fired during the run)
   - validation-result.json (if scenario has a per-scenario validator)
   - harness-summary.txt (argv + exit code + start timestamp)
        │
        ▼
per-scenario post-processing (run-test.ps1):
   - overlay-phase1-verify → tools/validate-overlay-frames.ps1 re-runs
     the projection math on logged matrices, asserts edge endpoints match
     within 0.5 px tolerance
   - npc-overlay-phase1-verify → asserts walker survived
        │
        ▼
exit code (pass=0, fail=1, hang=2, timeout=3, launch-fail=4, build-fail=5)
```

For overnight multi-scenario runs: `pwsh tools/run-overnight.ps1 -Scenarios "boot-to-main-menu,overlay-phase1-verify,npc-overlay-phase1-verify"`. Wraps run-test.ps1 in a sequential loop with a 4-layer watchdog (in-mod timeout → run-test log-stall → run-test hard timeout → outer process-level kill). Writes `tools/overnight-runs/overnight-<stamp>.json` for next-morning review.

Crash handler integration: when SEH fires during a scenario, `crash_handler.cpp` writes the result JSON with `"result":"crash"` BEFORE returning to WerFault. run-test.ps1 surfaces this as exit 1 (fail) instead of exit 4 (launch failure). Crash dumps land in `Game/mtr-asi-crash-*.dmp` and are auto-moved to the run archive.

Detailed usage: [docs/AUTONOMOUS_TESTING.md](AUTONOMOUS_TESTING.md).

---

## Source layout (legacy, kept for historical context)

---

## Game files in `Game/` (final state)

| File             | What it is                            | Source                                 |
|------------------|---------------------------------------|----------------------------------------|
| `Wilbur.exe`     | original game (SecuROM)               | retail                                  |
| `Launcher.exe`   | game launcher (we don't touch it)     | retail                                  |
| `Wilbur.ICO`, `Eula.txt`, `Readme.txt`, `binkw32.dll`, `data/`, `data_dx/`, `ereg/` | game assets | retail |
| `dinput8.dll`    | Ultimate ASI Loader                   | binary release (ThirteenAG)            |
| `mtr-asi.asi`    | our mod                               | built from `src/mtr-asi/`              |
| `d3d8.dll`       | dxwrapper stub                        | built from `third_party/dxwrapper/Stub`|
| `dxwrapper.dll`  | dxwrapper main library                | built from `third_party/dxwrapper/`    |
| `dxwrapper.ini`  | dxwrapper config (windowing flags)    | hand-edited                            |
| `mtr-asi.log`    | runtime log of our mod                | written at game start                  |
| `dxwrapper-wilbur.log` | runtime log of dxwrapper        | written at game start                  |

---

## Open issues

See [research/findings/known-issues.md](../research/findings/known-issues.md):

1. Mouse buttons stuck after game crash → run `tools/mouse-wake.ps1`
2. Bink intro plays at 800×600 in corner (cosmetic)
3. Save-slot creation fails (separate IO bug)
4. Mutex zombie blocks relaunch → run `tools/kill-wilbur.ps1`
5. .asi file lock after game crash → handled by build/deploy script via rename
6. Game hangs on shutdown sometimes → WMI Terminate fixes
7. dxwrapper `FullscreenWindowMode` doesn't auto-fill screen → our hook does
