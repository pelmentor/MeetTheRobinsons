// Native borderless-fullscreen window manager.
//
// Replaces dxwrapper's `EnableWindowMode = 1` + `FullscreenWindowMode = 1`
// behavior so we can retire dxwrapper for the DXVK migration (Phase B of
// the multi-week DXVK plan — research/findings/dxvk-migration-plan-2026-05-08.md).
//
// What this module does when enabled (default):
//
//   1. At IDirect3D9::CreateDevice / IDirect3DDevice9::Reset time,
//      mutates the caller's D3DPRESENT_PARAMETERS so the device is
//      created in **windowed** mode at monitor resolution. This matches
//      what dxwrapper did internally:
//          pp.Windowed                  = TRUE
//          pp.BackBufferWidth           = monitor_w  (if pp's value was 0)
//          pp.BackBufferHeight          = monitor_h  (if pp's value was 0)
//          pp.FullScreen_RefreshRateInHz = 0
//
//   2. Restyles the focus window to **borderless popup** filling the
//      monitor at (0,0):
//          GWL_STYLE   ~= remove WS_OVERLAPPEDWINDOW + WS_CAPTION + WS_THICKFRAME
//                        + WS_BORDER + WS_DLGFRAME + WS_SYSMENU
//                        + WS_MINIMIZEBOX + WS_MAXIMIZEBOX
//                       += WS_POPUP
//          GWL_EXSTYLE ~= remove WS_EX_CLIENTEDGE + WS_EX_WINDOWEDGE
//                        + WS_EX_DLGMODALFRAME + WS_EX_STATICEDGE
//          SetWindowPos(monitor_x, monitor_y, monitor_w, monitor_h,
//                       SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW)
//
//   3. No-ops ChangeDisplaySettingsA/W/ExA/ExW. Wilbur asks the OS to
//      yank the desktop into 4:3 / 800x600 / etc when launched without
//      a wrapper; under windowed mode that's both pointless and
//      disruptive (loses HDR, breaks multi-monitor layouts, leaves the
//      desktop in a wrong mode if the game crashes).
//
// Toggle defaults ON to match the user's existing dxwrapper config
// (FullscreenWindowMode = 1). Off-state means the original game pp is
// passed through verbatim and ChangeDisplaySettings is a passthrough —
// the equivalent of running with no wrapper at all (exclusive
// fullscreen + desktop mode change). User can flip via the Display tab.
//
// Persisted to mtr-asi-ui.ini under [windowmode].

#pragma once

#include <windows.h>

struct _D3DPRESENT_PARAMETERS_;

namespace mtr::windowmode {

// Install ChangeDisplaySettings hooks + load persisted state from INI.
// Call from dllmain after MH_Initialize and before any d3d9 device is
// created (the d3d9 deferred thread waits for d3d9.dll, so we have time
// even if installed in the same init thread).
void install();

// Master toggle. Default ON.
bool enabled();
void set_enabled(bool on);

// Apply borderless-fullscreen rewrite to the caller's present params and
// the focus window. Called from d3d9_hook.cpp's hk_CreateDevice (PRE-orig)
// when this module is enabled.
//
//   pp           : the IN/OUT D3DPRESENT_PARAMETERS the engine passed.
//                  We mutate it in place. Safe to pass null (no-op).
//   focus_window : the HWND CreateDevice was called with. Safe to pass
//                  null — restyle is then attempted on pp->hDeviceWindow.
//
// Idempotent: calling on an already-borderless window at monitor size
// re-issues SetWindowPos but the user-visible result is unchanged.
void apply_for_create_device(_D3DPRESENT_PARAMETERS_* pp, HWND focus_window);

// Same rewrite, but called from d3d9_hook.cpp's hk_Reset PRE-orig. Reset
// is what the engine calls on alt-tab return / window-size change /
// device-lost recovery. We rewrite pp the same way so the device stays
// in windowed-borderless after the reset.
void apply_for_reset(_D3DPRESENT_PARAMETERS_* pp);

// Diagnostics — exposed to the menu so the user can see what's been
// done. Counters reset on toggle-off → toggle-on.
unsigned long long create_device_rewrites();
unsigned long long reset_rewrites();
unsigned long long change_display_settings_blocks();
HWND               last_styled_window();
int                last_monitor_w();
int                last_monitor_h();

// Persistence (Win32 INI, [windowmode] section).
void load_ini();
void save_ini();
void request_save();

// Per-render-frame enforcement hook. Called from d3d9_hook's EndScene. When
// `-mtrasi-keep-dxresolution` is set, the engine may re-maximize the window
// during its own init (after our CreateDevice hook fires). Re-applying the
// requested windowed style+size for the first ~300 frames pins the window
// at the user-requested dims regardless of what the engine attempts. No-op
// when keep-dxresolution is not on the cmdline or the engine has settled.
void on_end_scene_tick();

} // namespace mtr::windowmode
