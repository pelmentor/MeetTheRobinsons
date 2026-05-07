# IAT anchors — known import addresses in the dumped Wilbur.exe

> Main IAT at `0x6A6000..0x6A634x`. All addresses below are **VAs**; valid in
> both the dumped binary and the live process (ImageBase = 0x00400000, no ASLR).

## Window / display (relevant for borderless-fullscreen mod, M-display)

| VA | API | DLL | Purpose for mod |
|---|---|---|---|
| `0x6A6210` | `GetSystemMetrics` | user32 | Read SM_CXSCREEN/CYSCREEN |
| `0x6A6224` | `GetWindowLongA` | user32 | Read current window style |
| `0x6A6248` | `SetWindowLongA` | user32 | **Strip WS_OVERLAPPEDWINDOW, add WS_POPUP** for borderless |
| `0x6A625C` | `ShowWindow` | user32 | SW_SHOW after style change |
| `0x6A6264` | `CreateWindowExA` | user32 | **Hook here**: override dwStyle/x/y/width/height |
| `0x6A6268` | `AdjustWindowRect` | user32 | Bypass for borderless |
| `0x6A626C` | `RegisterClassA` | user32 | Window class registration |
| `0x6A6278` | `SetWindowPos` | user32 | Reposition window to (0,0,monW,monH) |
| `0x6A627C` | `GetWindowInfo` | user32 | |
| `0x6A6280` | `GetMonitorInfoA` | user32 | Read monitor working-area rect |
| `0x6A6298` | `ChangeDisplaySettingsA` | user32 | **NOP this** — we don't want exclusive fullscreen |
| `0x6A62B0` | `GetDesktopWindow` | user32 | |
| `0x6A62B4` | `GetWindowRect` | user32 | |

## Renderer (D3D8)

| VA | API | DLL | Purpose for mod |
|---|---|---|---|
| `0x6A6340` | `Direct3DCreate8` | d3d8 | Hook returns IDirect3D8*; via vtable hook `CreateDevice` |
| `0x745154` | `DebugSetMute` | d3d8 | (in wrapper region — ignore) |

`IDirect3DDevice8::CreateDevice` is reached via the IDirect3D8 vtable, not directly from IAT. Hook it via vtable patching after intercepting `Direct3DCreate8`.

## Main loop / input

| VA | API | DLL |
|---|---|---|
| `0x6A6244` | `PeekMessageA` | user32 |
| `0x6A623C` | `DispatchMessageA` | user32 |
| `0x6A6240` | `TranslateMessage` | user32 |
| `0x6A6234` | `PostMessageA` | user32 |
| `0x6A6230` | `CallWindowProcA` | user32 |
| `0x6A62A4` | `DefWindowProcA` | user32 |
| `0x6A6294` | `GetMessageA` | user32 |
| `0x6A6020` | `DirectInput8Create` | dinput8 |

## Timing (relevant for FPS limiter)

| VA | API | DLL |
|---|---|---|
| `0x6A6094` | `QueryPerformanceFrequency` | kernel32 |
| `0x6A6098` | `QueryPerformanceCounter` | kernel32 |
| `0x6A6090` | `GetTickCount` | kernel32 |
| `0x6A6084` | `Sleep` | kernel32 |
| `0x6A62BC` | `timeGetTime` | winmm |

## Config (game reads/writes its INI through these)

| VA | API |
|---|---|
| `0x6A60B8` | `GetPrivateProfileStringA` |
| `0x6A60BC` | `WriteProfileStringA` |
| `0x6A60C0` | `GetProfileStringA` |
| `0x6A60E0` | `GetPrivateProfileIntA` |

Useful for finding out the game's config schema by xref'ing these.

## Game internals (renamed in IDA database 2026-05-04)

| Address    | Renamed to                              | What it is                                                |
|------------|-----------------------------------------|-----------------------------------------------------------|
| `0x56F990` | `game_MainWndProc`                      | main window procedure                                     |
| `0x56F6E0` | `game_MessageLoop`                      | PeekMessage/GetMessage/Translate/Dispatch loop            |
| `0x56FFA0` | `game_App_ctor`                         | GameApp singleton constructor (writes vtable 0x6C7848)    |
| `0x561C10` | `game_GetCameraAspect`                  | returns camera aspect ratio (4:3 dynamic or constant)     |
| `0x564550` | `game_camera_setup`                     | only caller of `game_GetCameraAspect`                     |
| `0x567ED0` | `game_InitD3D8`                         | calls Direct3DCreate8 + sets up device                    |
| `0x56D8C0` | `game_CreateDevice_thunk_securom`       | thunk to SecuROM-decoded CreateDevice (untraceable static)|
| `0x56D1D0` | `ORPHAN_unused_d3d_init_DEAD_CODE`      | looked perfect but has 0 xrefs anywhere — DEAD            |
| `0x562E00` | `game_UnregisterMainWindowClass`        | UnregisterClassA wrapper for "Meet The Robinsons"         |
| `0x6852D0` | `auxtext_CreateWindow`                  | creates the *debug* "AuxTextWin" window (NOT main)        |
| `0x685240` | `auxtext_RegisterClass`                 | registers "AuxTextWin" class                              |
| `0x684FD0` | `auxtext_CenterOnScreen`                | centers the AuxText window                                |

| Global address | Renamed to                          | What it is                                              |
|----------------|-------------------------------------|---------------------------------------------------------|
| `0x72F710`     | `g_App_singleton`                   | pointer to GameApp instance (vtable at 0x6C7848)        |
| `0x6FBC8C`     | `g_MainWindow_class_descriptor`     | game's window-class descriptor (hInstance/WndProc/name) |
| `0x6FBC38`     | `g_window_client_width`             | written from GetClientRect(hwnd).right                  |
| `0x6FBC3C`     | `g_window_client_height`            | written from GetClientRect(hwnd).bottom                 |
| `0x6C750C`     | `g_camera_aspect_constant_4_3`      | 4 bytes float `1.333333f` (`AB AA AA 3F`)               |

The address `0x02987BEE` previously written in this doc as the WSGF aspect constant
**was wrong** — those bytes are part of a string `\system32\DRIVE...`, not a float.
The real aspect float lives at `0x6C750C` as documented above.

## What the various display/aspect/resolution paths look like

See:
- `aspect-ratio-fix.md` — projection matrix override via D3D9 SetTransform
- `native-resolution-fix.md` — IDirect3D9::CreateDevice hook
- `imgui-menu-architecture.md` — menu / WndProc / input
- `dxwrapper-integration.md` — D3D8 → D3D9 wrapper architecture
- `lessons-learned.md` — meta-lessons
