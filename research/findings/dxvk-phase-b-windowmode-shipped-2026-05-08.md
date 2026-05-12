# DXVK migration Phase B — native FullscreenWindowMode SHIPPED

Date: 2026-05-08

## What this is

Phase B of the multi-week DXVK migration plan
([dxvk-migration-plan-2026-05-08.md](dxvk-migration-plan-2026-05-08.md)).
Replicates dxwrapper's `EnableWindowMode = 1 + FullscreenWindowMode = 1`
behavior natively in `mtr-asi`, so dxwrapper can be removed entirely
(Phase C) without losing the bordered → borderless-fullscreen behavior
the user has been running with.

## Why we needed to replicate this in mtr-asi

User's current `Game/dxwrapper.ini` had:

```
EnableD3d9Wrapper      = 1
EnableWindowMode       = 1
WindowModeBorder       = 0
FullscreenWindowMode   = 1
```

This translates to: at `IDirect3D9::CreateDevice` time, dxwrapper:

  - rewrites `pp.Windowed = TRUE` so D3D9 does NOT change the desktop mode,
  - resizes / restyles the window to borderless monitor-sized at (0,0),
  - intercepts `ChangeDisplaySettings*` so the game can't ask the OS to
    switch desktop mode underneath.

DXVK does not provide this feature natively. If we remove dxwrapper
before re-implementing it, the game launches into exclusive fullscreen
+ a desktop mode change to (probably) 4:3 800x600 — losing HDR, multi-
monitor layout, alt-tab speed, and several mtr-asi conveniences that
assume a windowed swapchain.

## What landed (build = 550912 bytes)

### New module

- **[src/mtr-asi/include/mtr/windowmode.h](../../src/mtr-asi/include/mtr/windowmode.h)** —
  public API: `install()`, `enabled()`, `set_enabled(bool)`,
  `apply_for_create_device(pp, focus_window)`, `apply_for_reset(pp)`,
  read-only diagnostics (`create_device_rewrites()`,
  `reset_rewrites()`, `change_display_settings_blocks()`,
  `last_styled_window()`, `last_monitor_w()`, `last_monitor_h()`),
  and INI persistence (`load_ini()` / `save_ini()`).

- **[src/mtr-asi/src/windowmode.cpp](../../src/mtr-asi/src/windowmode.cpp)** —
  implementation. ~330 lines. Three responsibilities, all gated on
  the master toggle `g_enabled` (default ON):

    1. **`do_rewrite(pp, focus_window)`** mutates the
       `D3DPRESENT_PARAMETERS` so `pp.Windowed = TRUE`,
       `pp.BackBufferWidth/Height` are set to monitor dims if the
       caller passed zero (we don't override a non-zero value from
       the engine — the user might have a sub-monitor render res
       from `-dxresolution` that we should honor), and
       `pp.FullScreen_RefreshRateInHz = 0`.

    2. **`apply_borderless(wnd, x, y, w, h)`** strips
       `WS_OVERLAPPEDWINDOW + WS_CAPTION + WS_THICKFRAME +
       WS_BORDER + WS_DLGFRAME + WS_SYSMENU + WS_MINIMIZEBOX +
       WS_MAXIMIZEBOX` and `WS_EX_CLIENTEDGE + WS_EX_WINDOWEDGE +
       WS_EX_DLGMODALFRAME + WS_EX_STATICEDGE`, adds `WS_POPUP`,
       then `SetWindowPos(monitor_x, monitor_y, monitor_w,
       monitor_h, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW)`.
       Idempotent.

    3. **MinHook detours on `user32!ChangeDisplaySettingsA/W/ExA/ExW`**
       — when toggle is ON, return `DISP_CHANGE_SUCCESSFUL` without
       dispatching to the original. Increments a counter so the
       Display tab can show how often the game tried.

  Multi-monitor aware: `MonitorFromWindow(focus, MONITOR_DEFAULTTONEAREST)`
  picks the right monitor for the user's window, instead of always
  picking the primary like dxwrapper did.

### Wire-up

- **[src/mtr-asi/src/d3d9_hook.cpp](../../src/mtr-asi/src/d3d9_hook.cpp)** —
  added forward decls for `mtr::windowmode::apply_for_create_device` /
  `apply_for_reset`, called PRE-orig in `hk_CreateDevice` (with the
  `hFocusWindow` arg) and `hk_Reset` (no separate focus arg — falls
  back to `pp->hDeviceWindow` inside `do_rewrite`).
  Logs pp before AND after the rewrite so the runtime log shows
  exactly what the engine asked for vs what the device got.

- **[src/mtr-asi/CMakeLists.txt](../../src/mtr-asi/CMakeLists.txt)** —
  added `src/windowmode.cpp` to the source list.

- **[src/mtr-asi/src/dllmain.cpp](../../src/mtr-asi/src/dllmain.cpp)** —
  installed `mtr::windowmode::install()` BEFORE
  `mtr::d3d9hook::install()` in the `mtr_init` thread, so the
  `ChangeDisplaySettings` hooks are armed before any code path that
  might call them (dxwrapper installs its CDS hooks during DLL load
  for the same reason). The d3d9 vtable hooks wait for `d3d9.dll`
  to load via a deferred thread, so this ordering doesn't race the
  device creation path either.

- **[src/mtr-asi/src/menu.cpp](../../src/mtr-asi/src/menu.cpp)** —
  added a "Borderless fullscreen window" section at the TOP of the
  Display tab. Includes:
  - master toggle `Borderless windowed at monitor size`,
  - tooltip explaining what each side of the toggle does and that
    the OFF state means "exclusive fullscreen + display mode change",
  - read-only diagnostics: last styled HWND, monitor size, and counts
    of CreateDevice rewrites / Reset rewrites / CDS blocks (helpful
    when verifying the toggle is firing).

### Persistence

- INI file: `Game/mtr-asi-ui.ini`, section `[windowmode]`, key `enabled = 0|1`.
- Default first-run state: ON. Writes the file out on first launch
  if it doesn't exist, so the user has a baseline they can edit.
- `set_enabled(bool)` saves immediately on transition (single tiny
  `WritePrivateProfileStringA`) — no debounce machinery.

## What this does NOT do (yet)

These can stay deferred until they show up as actual problems:

- **Window class detection / multi-window support**. We restyle the
  *focus window* CreateDevice was called with. If Wilbur ever creates
  a separate D3D device for a sub-window, that sub-window won't be
  rewritten. (No evidence Wilbur does this — single device is the
  norm for D3D8 titles of this era.)
- **Refresh rate selection**. We hard-zero
  `pp.FullScreen_RefreshRateInHz`. dxwrapper had an `OverrideRefreshRate`
  knob — we can add it later if a user with a non-60Hz monitor
  reports an issue.
- **WindowModeBorder toggle**. dxwrapper had a "borderless vs
  bordered window" knob. We always go borderless (the dxwrapper
  config has `WindowModeBorder = 0`, so it matches). If anyone ever
  wants a draggable bordered window, we add a second toggle.
- **Initial window position offset**. dxwrapper had
  `SetInitialWindowPosition + InitialWindowPositionLeft/Top`. We
  always position at the monitor's top-left corner. Adding a
  per-monitor offset is a 5-minute change if anyone asks for it.

## How to verify at runtime

After deploying the new ASI to `Game/`:

1. Launch Wilbur (with dxwrapper still in place — see "Phase ordering"
   below). Open the Insert menu → Display tab. Confirm the
   "Borderless fullscreen window" section appears at the top.
2. The toggle should be ON. The diagnostics row should show:
   - `Last styled hwnd : <non-null>` (the game window)
   - `Monitor size    : <your monitor W x H>`
   - `CreateDevice rewrites : 1` (or higher if Reset has fired)
3. The window should be borderless and fill the monitor. Alt-tab
   should be instant (no exclusive-fullscreen mode change).
4. Toggle OFF → restart the game (the toggle takes effect on the
   *next* CreateDevice / Reset, not retroactively). Game should
   launch into bordered windowed (no display mode change either,
   since dxwrapper is still active and overrides ours via its own
   FullscreenWindowMode).

## Phase ordering — why this is shipped BEFORE dxwrapper retirement

We're keeping dxwrapper active for now (Phase C is the retirement
step). Two reasons to ship Phase B first:

1. **No regression risk** — with dxwrapper still active, dxwrapper's
   own `FullscreenWindowMode` runs FIRST. By the time `IDirect3D9::CreateDevice`
   reaches our hook, dxwrapper has already rewritten `pp.Windowed = TRUE`
   and restyled the window. Our `apply_for_create_device` runs the
   rewrite again — idempotent, no harm. So shipping Phase B early
   gives us testing time without breaking anything.

2. **Verifies the hook chain BEFORE we lose the safety net** —
   if our rewrite has a bug (wrong monitor size on multi-monitor,
   wrong style flags, etc.), we see it BEFORE we yank dxwrapper. If
   we shipped C first and Phase B was wrong, the user would be stuck
   in exclusive fullscreen with a busted desktop mode and no way to
   alt-tab to fix it.

When the user is ready, Phase C is straightforward:

- Delete `Game/dxwrapper.dll` and `Game/dxwrapper.ini`.
- Rebuild DXVK from `third_party/dxvk` via `tools/build-dxvk.sh`
  (Phase A artifact). It deploys `d3d8.dll` and `d3d9.dll` to `Game/`.
- Launch. mtr-asi's Phase B now is the *only* thing handling
  fullscreen-windowed; if the diagnostics counters increment and
  the window is borderless monitor-sized, we're done.

## Code stats

- New: 1 header (62 lines), 1 cpp (~330 lines), 1 findings doc.
- Modified: `d3d9_hook.cpp` (+10 lines), `menu.cpp` (+50 lines for
  the new section + forward decls), `dllmain.cpp` (+2 lines),
  `CMakeLists.txt` (+1 line).
- Build: 550912 bytes (was 546304, +4608 bytes for new module +
  menu section).
