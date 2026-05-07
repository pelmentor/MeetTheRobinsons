# Architecture

How the moving parts of this project relate to each other.
For *what* we're trying to achieve, see [ROADMAP.md](ROADMAP.md). For *the game's*
internal architecture as we understand it, see [research/findings/](../research/findings/).

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
│  Wilbur.exe (D3D8 game, SecuROM-protected, x86)                       │
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

## The runtime stack (current state, 2026-05-04)

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

## Source layout (`src/mtr-asi/`)

```
src/mtr-asi/
├── CMakeLists.txt              Win32-only build, CXX20, /W4
├── third_party/
│   ├── minhook/                git submodule (vendored)
│   └── imgui/                  git submodule, v1.91.5
├── include/
│   └── mtr/
│       └── version.h           MTR_ASI_VERSION macro
└── src/
    ├── dllmain.cpp             DllMain → worker thread → hooks
    ├── log.cpp                 simple file logger to mtr-asi.log
    ├── menu.cpp                ImGui state, WndProc subclass, polling input
    └── d3d9_hook.cpp           dummy device → vtable capture → MinHook
```

Build:

```powershell
cmake -S src/mtr-asi -B src/mtr-asi/build -A Win32
cmake --build src/mtr-asi/build --config Release
```

Output: `src/mtr-asi/build/Release/mtr-asi.asi` (~290 KB).

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
