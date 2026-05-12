# mtr-asi — ASI mod for *Disney's Meet the Robinsons* (2007 PC)

Runtime mod that injects features into Wilbur.exe.

- Loaded via [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (or via `dxwrapper` acting as the d3d8 proxy in this project — see [../../research/findings/dxwrapper-integration.md](../../research/findings/dxwrapper-integration.md))
- Inline hooks via [MinHook](https://github.com/TsudaKageyu/minhook)
- In-game overlay rendered with [Dear ImGui](https://github.com/ocornut/imgui) on top of the game's D3D9 swap chain (translated from D3D8 by `dxwrapper`)
- Toggle menu: **Insert**, screenshot: **F12**

## What it does

### Display & camera

| Feature | Mechanism | Doc |
|---------|-----------|-----|
| Native-resolution rendering | Hook `GetCommandLineA/W` and rewrite `-dxresolution=…` to monitor dims so the game initialises at native from boot. | [native-resolution-fix.md](../../research/findings/native-resolution-fix.md) |
| Live world aspect-ratio override (16:9 default; 4:3 / 16:10 / 21:9 / custom) | `hk_BuildProjMatrix` substitutes aspect param at projection-build time; `hk_CameraCompute` invalidates the per-camera cache each frame. | [aspect-ratio-fix.md](../../research/findings/aspect-ratio-fix.md) |
| Live FOV override | Same hook substitutes FOV when override is active. | — |
| FreeCam (F3 toggle) | PRE-hook `sub_4C1BA0` (per-camera apply) — single funnel for every camera class. Overwrites view matrix at outer+0x34 before engine propagates to D3D. WASD + mouse-look (window-center recenter), Space/C up/down, Shift boost, mouse wheel speed. | [camera-subsystem.md](../../research/findings/camera-subsystem.md) |
| Player input suppression while FreeCam / menu / console open | DInput exclusive defeats WndProc; intercept `IDirectInputDevice8::GetDeviceState` and zero the buffers. | — |

### World/scene tuning

| Feature | Mechanism | Status |
|---------|-----------|--------|
| Draw distance (camera frustum far plane) | Per-frame data-write into engine's cached frustum buffer at `outer+0xD4` (far plane equation + far corners). | **WORKS** |
| Fog disable | Write 0 to fogEnabled cvar BYTE @ 0x745279, pump each frame, also filter `D3DRS_FOGENABLE` in SetRenderState. | **WORKS** |
| Side-cull / Periphery cull / vis_test bypass | Multiple knobs deployed for the user-reported "corner culling" symptom; **all currently dead** — actual cull system still being investigated. Static RE on `(scene+104) bit 0` ruled it out (all 8 writers script/init-driven, none camera). Top remaining suspect: render-context list populator (statically untraceable). | **DIAGNOSTIC** — see [optimization-systems.md](../../research/findings/optimization-systems.md) |
| LOD distance overrides (MeshLOD.{FocusDist, HighDist, MediumDist}, ActorLOD.LODScale) | Direct writes to engine cvar globals at 0x745B38..0x745B90. | not yet runtime-tested |

### RE infrastructure

| Feature | Mechanism | Doc |
|---------|-----------|-----|
| Cvar dump | Hook all 8 typed-X registration functions (`console_register_var_typed_a..h`); record (group, name, address, caller); button writes `Game/mtr_cvars.txt` (~13.3k entries). | [cvar-system.md](../../research/findings/cvar-system.md) |
| `vis_test_probe` (IAT-slot diagnostic) | Patch `*(0xF92F34)` (the stolen-byte IAT thunk's indirect target) to a wrapper. Counts pass/fail per call site, optional force-pass. | [optimization-systems.md](../../research/findings/optimization-systems.md), [render-pipeline.md](../../research/findings/render-pipeline.md) |
| Multi-site `force_vis` | 5-byte call-site rewrite (`mov al,1; nop*3`) at all 4 sites of `sub_4E0B90` (vis_test). | [optimization-systems.md](../../research/findings/optimization-systems.md) |
| `scene_vis_log` (visibility-write tracker) | Hooks `scene_set_visible` (sub_4AABC0) + `script_set_instance_hidden` (sub_5E3DC0). Per-frame counters: hides, shows, script invocations + hide/show classification via post-call bit-0 inspection. Sticky log of last 8 distinct scene addrs hidden this frame. Resolves whether the corner-cull is script-driven (counters spike at corners) or upstream (counters flat). | [optimization-systems.md](../../research/findings/optimization-systems.md) |
| Sprite-batcher matrix override (UI aspect) | Hooks `transform_apply_scale_via_stack` (sub_562AA0) and `transform_apply_translate_via_stack` (sub_562AE0). Per-arg multipliers for sx/sy/sz and tx/ty/tz. Auto-pillarbox buttons (4:3 / 16:10) compute correct factor; auto-from-rules mode drives sx/tx from `ui_aspect_rules` per-screen. Off by default → zero risk. | [ui-render-investigation.md](../../research/findings/ui-render-investigation.md) |
| Screen stack mirror | Hooks `screen_manager_push_by_name` (sub_604310) and `screen_manager_pop_top` (sub_604C90). 16-deep mirror correctly tracks top-of-stack across navigation + back-out. Powers per-screen UI aspect rules. | — |

### UI / overlay

| Feature | Mechanism | Doc |
|---------|-----------|-----|
| In-game ImGui menu (Insert) | Hook `EndScene` / `Reset`; subclass WndProc; poll input via `GetAsyncKeyState`/`GetCursorPos` (DInput exclusive). | [imgui-menu-architecture.md](../../research/findings/imgui-menu-architecture.md) |
| Console restoration (F2) | RE'd the engine's console UI; re-enable + display via ImGui. | [debug-features.md](../../research/findings/debug-features.md) |
| Screenshot to BMP (F12 + menu button) | Capture backbuffer via `GetRenderTargetData`; write 24-bit BMP to `Game/screenshots/`. After menu draws, so what you see is what you get. | — |
| Per-screen UI aspect rules (infrastructure) | Rules table (`ui_aspect_rules.cpp`) consulted by `hk_BuildOrtho`; "current top screen" mirrored from `screen_manager_push_by_name`. | [ui-render-investigation.md](../../research/findings/ui-render-investigation.md) |
| UI aspect override (sprite-batcher path) | Static RE confirmed `render_sprite_batcher` (sub_4E8D30) builds projection + view via `transform_apply_scale_via_stack` (matrix4_make_scale at 0x63C4E3) + `transform_apply_translate_via_stack` (matrix4_make_translate at 0x63C573), bypassing `build_ortho_matrix` entirely. mtr-asi hooks both helpers; menu has "Auto-pillarbox to 4:3" button + "Drive sx/tx from ui_aspect_rules" auto-mode. **Pending only Test 7b/7c/7d runtime confirmation.** | [ui-render-investigation.md](../../research/findings/ui-render-investigation.md) |
| Stable shutdown | `DllMain DLL_PROCESS_DETACH` is a no-op when `lpvReserved != NULL` (process termination). | [known-issues.md](../../research/findings/known-issues.md) §1, §6 |

### Hotkeys

| Key | Action |
|-----|--------|
| Insert | Toggle mtr-asi menu |
| F2 | Toggle console |
| F3 | Toggle FreeCam |
| F12 | Screenshot |

Sister tooling lives in [`tools/`](../../tools/README.md): `run_game.py` for the dev cycle (deploy + clean + launch), `find_mutex_holder.py` and `skip_intros.py` for state hygiene, and `find_aspect_rva.py` / `patch_aspect.py` for binary-level investigations.

## Layout

```
src/mtr-asi/
├── CMakeLists.txt
├── third_party/
│   ├── minhook/        # git submodule
│   ├── imgui/          # git submodule
│   └── README.md
├── include/
│   └── mtr/
│       └── version.h   # MTR_ASI_VERSION (mod version string)
└── src/
    ├── dllmain.cpp           # entry point — synchronous cmdline + aspect init in DllMain
    ├── log.cpp               # shared-write log to Game\mtr-asi.log
    ├── cmdline_hook.cpp      # GetCommandLineA/W hook → -dxresolution=<monitor>
    ├── d3d9_hook.cpp         # CreateDevice/EndScene/Reset (menu) + perspective/ortho hooks + per-camera apply
    ├── aspect_patch.cpp      # mtr::aspect / fov / draw_dist / scene / lod / force_vis namespaces
    ├── menu.cpp              # ImGui overlay (Camera/Display/World/Tools tabs)
    ├── screenshot.cpp        # backbuffer capture to BMP
    ├── console.cpp           # engine console restoration (F2)
    ├── freecam.cpp           # FreeCam controller (F3)
    ├── input_hook.cpp        # WH_MOUSE_LL low-level wheel hook
    ├── dinput_hook.cpp       # DirectInput8 GetDeviceState filter (player suppression)
    ├── screen_push.cpp       # screen-manager push hook + current-top-screen mirror
    ├── level_select.cpp      # level transition wrapper (artifact)
    ├── ui_aspect_rules.cpp   # per-screen UI aspect rules table (infrastructure)
    ├── cvar_dump.cpp         # 8-typed-X registration hooks + dump-to-file
    ├── vis_test_probe.cpp    # IAT-slot patch at 0xF92F34 + per-site counters
    └── scene_vis_log.cpp     # scene_set_visible + script_set_instance_hidden trackers
```

## Building

```powershell
git submodule update --init --recursive          # first time only

cmake -B src/mtr-asi/build -S src/mtr-asi -A Win32 -G "Visual Studio 17 2022"
cmake --build src/mtr-asi/build --config Release

# Output: src/mtr-asi/build/Release/mtr-asi.asi
```

x86 (Win32) only — Wilbur.exe is 32-bit, so the ASI must match.

## Installing

```
Game/
├── dxwrapper.dll      <-- elishacloud/dxwrapper d3d8→d3d9 + .asi loader (vendored)
├── dxwrapper.ini      <-- config; D3d8to9=1, EnableD3d9Wrapper=1, FullscreenWindowMode=1
└── mtr-asi.asi        <-- this build's output
```

`dxwrapper` is the d3d8 proxy that translates the game's D3D8 calls to D3D9; in this project it also serves as the .asi loader. (We don't use Ultimate ASI Loader on top of it.) Detailed wiring in [dxwrapper-integration.md](../../research/findings/dxwrapper-integration.md).

For one-shot dev iteration use `Game\run.bat` (which calls `tools/run_game.py`) — handles build deploy + zombie cleanup + launch.

## Logs

`Game/mtr-asi.log`. Reset on each launch. Opened with `_fsopen(_, "w", _SH_DENYNO)` so other processes can read it while the game is running (`type Game\mtr-asi.log`, Notepad++, `tail -f`-style tools all work).

## Compatibility

This mod targets the **retail DVD build** of Wilbur.exe (originally SecuROM-7 packed; we work against an unpacked IDB — see `docs/SECUROM.md` for the historical procedure). ImageBase `0x00400000`, no ASLR. Function VAs in `aspect_patch.cpp` and `d3d9_hook.cpp` are baked-in constants that match this build:

| VA           | Symbol                                  |
|--------------|-----------------------------------------|
| `0x00562B20` | `game_BuildPerspectiveMatrix`           |
| `0x00562B70` | `build_ortho_matrix`                    |
| `0x00562AA0` | `transform_apply_scale_via_stack`       |
| `0x00562AE0` | `transform_apply_translate_via_stack`   |
| `0x00564600` | `game_camera_recompute_projection`      |
| `0x005625E0` | `wrap_SetTransform`                     |
| `0x004C1BA0` | `sub_4C1BA0` (per-camera apply)         |
| `0x004D22D0` | `render_frame_top_level`                |
| `0x004E8D30` | `render_sprite_batcher`                 |
| `0x004AABC0` | `scene_set_visible`                     |
| `0x004E4370` | `anim_evaluate_track`                   |
| `0x00604310` | `screen_manager_push_by_name`           |
| `0x00604C90` | `screen_manager_pop_top`                |
| `0x004E0B90` | `sub_4E0B90` (vis_test 1-instr stolen-byte IAT thunk) |
| `0x006C750C` | dead-code aspect float (NOT patched at runtime) |

Symbol map: [research/findings/symbol-table.md](../../research/findings/symbol-table.md). For a different build, you'd need to find the corresponding RVAs in your own unpacked dump and update the constants — there's no auto-detection.

The mod doesn't currently version-check Wilbur.exe before installing hooks. If the bytes at `kBuildProjMatrixVA` etc. don't look like a known function start, MinHook will likely fail to install the hook (visible in `mtr-asi.log`); the mod will still load but without aspect override.
