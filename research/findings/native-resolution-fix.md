# Native-resolution fix (game runs at monitor size from frame 0)

**Status: SOLVED, no crutches (2026-05-04).** Implementation: `src/mtr-asi/src/cmdline_hook.cpp`. The mod hooks `GetCommandLineA/W` very early in `DllMain` and rewrites `-dxresolution=WxH` (or injects it if absent) to the primary monitor's dimensions. The game then **initialises itself at native resolution from boot** — backbuffer, window, viewport, projection, all consistent.

## Approach summary

```
Launcher.exe                                       Wilbur.exe boot
─────────────────                                  ─────────────────
build cmdline:                                     ┌── DLL_PROCESS_ATTACH (our mod) ──┐
"-dxfullscreen -dxadapter=0                        │ MH_Initialize                    │
 -dxresolution=640x480-dxdiskdrive…                │ cmdline::install()               │
 -launchit"                                        │   MH_CreateHook GetCommandLineA  │
                              CreateProcess →      │   MH_CreateHook GetCommandLineW  │
                                                   └─ ↓ DllMain returns ──────────────┘
                                                   CRT __getmainargs()
                                                     calls GetCommandLineA
                                                     ↳ HOOKED — sees "-dxresolution=640x480",
                                                       rewrites to "-dxresolution=2560x1440",
                                                       returns modified static buffer
                                                   game parses cmdline → 2560x1440
                                                   game asks D3D for 2560x1440 backbuffer
                                                   game's window opens at 2560x1440
                                                   game_window_show_and_update_client_size
                                                     populates g_window_client_width/height
                                                     with 2560/1440
                                                   game_GetCameraAspect returns 1.778…
```

Everything downstream of cmdline parsing inherits the correct resolution. No post-hoc backbuffer overrides, no D3D-level patching of `D3DPRESENT_PARAMETERS`.

## Why we DIDN'T do this with a `CreateDevice` backbuffer rewrite

Previously the mod hooked `IDirect3D9::CreateDevice` (vtable[16]) and rewrote `pp->BackBufferWidth/Height` to monitor size, plus `SetWindowPos`'d the focus window. This worked for the backbuffer dimensions but left **everything else inside the game thinking it was running at 800×600**:

- `g_window_client_width/height` were set from `GetClientRect` AFTER our resize, so they showed 2560×1440 — but the game had already cached layout decisions in 4:3.
- Aspect-ratio path (`game_camera_setup`) ran with a 4:3 cached value because the **timing** of when the camera struct's `*(this+12)` field gets set vs. when the window is resized was problematic.
- HUD, menus, mouse cursor, Bink dest rect — all sized to 800×600 because that's what the game was told to render.

Result: stretched 4:3 output upscaled to 16:9 backbuffer. Visually wrong (faces too wide).

The cmdline-hook approach removes this entire class of problem by **never letting the game think it's 800×600 in the first place**.

## Cmdline rewrite logic

In `src/mtr-asi/src/cmdline_hook.cpp`:

1. **Find** `-dxresolution=WxH` token in the command line.
2. **Parse** WxH into integers.
3. **Replace** WxH (in place) with `MONITOR_W x MONITOR_H` from `MonitorFromPoint(0,0)` + `GetMonitorInfoA`.
4. If the token is **absent** (e.g. user runs `Wilbur.exe -launchit` directly without going through the launcher), **append** ` -dxresolution=MONITOR_W x MONITOR_H` to the cmdline.
5. Cache the result in a static buffer; subsequent `GetCommandLineA` calls return the cached buffer.
6. Same logic mirrored for `GetCommandLineW` (wide variant) — game uses the ANSI variant in retail but Windows hooks should cover both.

A separate static buffer per variant (A and W). 2 KB each, with overflow guard that falls back to passthrough.

### Important quirk: launcher emits malformed cmdline

The launcher's `Launcher_LaunchGame` (0x4047C0) builds:

```
"D:\…\Wilbur.exe" -dxfullscreen -dxadapter=0 -dxresolution=640x480-dxdiskdriveletter=c -launchit
```

Note **no space** between `640x480` and `-dxdiskdriveletter`. Our parser stops at the first non-digit after `WxH`, so the resolution tokenises correctly. After rewrite the cmdline becomes:

```
… -dxresolution=2560x1440-dxdiskdriveletter=c -launchit
```

Still glued, but the game's `-dxresolution=` parser also stops at non-digit so it works. The `-dxdiskdriveletter=c` token is consumed normally. Don't try to "fix" the missing space — the game accepts it.

## Why not patch the launcher?

WSGF's guide says to hex-edit `1280x1024` (string) in `Launcher.exe` to your resolution. That works in principle but:
- We don't ship a modded launcher (signed binary, wraps DRM).
- It only addresses the launcher's resolution dropdown — same problem.
- We already have to be inside Wilbur.exe for the rest of the mod, so cmdline rewrite is "free" there.

## Why we hook `GetCommandLineA` instead of just `__argv`

The CRT initialises `__argv` from `GetCommandLineA` once at startup. By the time our DLL gets a chance to run (when DLL_PROCESS_ATTACH is processed by the loader), `__argv` may already be populated. Hooking `GetCommandLineA` itself is more reliable — any caller (CRT, SecuROM stub, custom code) that re-queries the cmdline gets our value.

We must install the hook **synchronously in `DllMain`**, not in a worker thread, because:
1. DLL_PROCESS_ATTACH runs before the game's entry point.
2. The game's CRT calls `GetCommandLineA` very early during `__getmainargs`.
3. If we install the hook from a thread spawned in DllMain, the thread doesn't run until DllMain returns and the loader unwinds — and by that point CRT may have already cached the cmdline.

`MH_Initialize` and `cmdline::install()` are called directly in `DllMain` for this reason. See [`src/mtr-asi/src/dllmain.cpp`](../../src/mtr-asi/src/dllmain.cpp).

## D3D9 hooks today are menu-only

After the cmdline-rewrite approach lands, the D3D9 hooks (`hk_CreateDevice`, `hk_EndScene`, `hk_Reset`) **no longer rewrite anything**. They exist only to:

- `EndScene`: tick ImGui (the in-game menu) and screenshot capture.
- `Reset`: invalidate ImGui device-bound resources before reset, recreate after.
- `CreateDevice`: log what the game asks for (now native because cmdline rewrite worked).

There's no `g_force_native` toggle anymore — there's nothing to toggle. The mod just works.

## Files

- `src/mtr-asi/src/cmdline_hook.cpp` — the actual fix.
- `src/mtr-asi/src/dllmain.cpp` — synchronous hook installation in `DLL_PROCESS_ATTACH`.
- `src/mtr-asi/src/d3d9_hook.cpp` — menu-only hooks; `hk_CreateDevice` is logging-only.
- `research/findings/launcher-internals.md` — the launcher cmdline construction details.
