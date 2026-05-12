// Native borderless-fullscreen window manager.
//
// See include/mtr/windowmode.h for the full design rationale. Short
// version: replaces dxwrapper's `EnableWindowMode = 1 + FullscreenWindowMode = 1`
// behavior so the DXVK migration (Phase B) can drop dxwrapper entirely.
//
// Three responsibilities, all gated on the master toggle:
//   1. Mutate D3DPRESENT_PARAMETERS at CreateDevice / Reset time so the
//      D3D9 device runs windowed at monitor resolution.
//   2. Restyle the focus window to borderless and resize / reposition
//      to fill the monitor at (0,0).
//   3. Block ChangeDisplaySettings* calls so the OS desktop mode stays
//      put (the game would otherwise switch the desktop to its
//      requested 4:3 mode on launch).

#include "mtr/windowmode.h"

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mtr        { HMODULE self_module(); }
namespace mtr::log   { void info(const char* fmt, ...); }

namespace mtr::windowmode {

// Outer-ns forward decls — defined below in this TU. Declared here so the
// anonymous-namespace `apply_windowed` can call `patch_engine_client_dims`
// via unqualified lookup (walks outward to mtr::windowmode where the real
// definition lives, with proper external linkage).
void patch_engine_client_dims(int w, int h);

namespace {

// ---- State ----------------------------------------------------------------

std::atomic<bool> g_enabled{true};   // default ON — matches user's existing dxwrapper config
std::mutex        g_mu;

std::atomic<unsigned long long> g_create_rewrites{0};
std::atomic<unsigned long long> g_reset_rewrites {0};
std::atomic<unsigned long long> g_cds_blocks     {0};

HWND g_last_styled = nullptr;
int  g_last_mon_w  = 0;
int  g_last_mon_h  = 0;
int  g_last_mon_x  = 0;
int  g_last_mon_y  = 0;

// Frame counter for the keep-dxresolution enforcement loop. Set to a small
// positive value by apply_windowed; decremented each EndScene. When > 0,
// on_end_scene_tick re-applies the windowed style+size if the engine has
// resized the window away from our requested dims.
std::atomic<int> g_enforce_frames{0};

// save_ini() is declared in mtr/windowmode.h (included above), so it's
// already in scope for set_enabled() below — no extra forward decl needed.

// ---- ChangeDisplaySettings hook trampolines -------------------------------

using PFN_CDSA   = LONG (WINAPI*)(DEVMODEA*, DWORD);
using PFN_CDSW   = LONG (WINAPI*)(DEVMODEW*, DWORD);
using PFN_CDSExA = LONG (WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
using PFN_CDSExW = LONG (WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);

PFN_CDSA   g_orig_cds_a   = nullptr;
PFN_CDSW   g_orig_cds_w   = nullptr;
PFN_CDSExA g_orig_cds_ex_a = nullptr;
PFN_CDSExW g_orig_cds_ex_w = nullptr;

// ---- INI helpers ----------------------------------------------------------

bool resolve_ini_path(char* out, size_t out_size) {
    if (!out || out_size < MAX_PATH) return false;
    HMODULE self = mtr::self_module();
    char modpath[MAX_PATH] = {0};
    DWORD got = GetModuleFileNameA(self, modpath, sizeof(modpath));
    if (got == 0 || got >= sizeof(modpath)) return false;
    char* slash = std::strrchr(modpath, '\\');
    if (!slash) slash = std::strrchr(modpath, '/');
    if (!slash) return false;
    *(slash + 1) = 0;
    int n = std::snprintf(out, out_size, "%smtr-asi-ui.ini", modpath);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

// ---- Monitor lookup -------------------------------------------------------

// Get the monitor that the focus window is on (multi-monitor aware). Falls
// back to the primary monitor if the window is null or off-screen.
void monitor_dims_for_window(HWND wnd, int& w, int& h, int& x, int& y) {
    HMONITOR mon = nullptr;
    if (wnd && IsWindow(wnd)) {
        mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    }
    if (!mon) {
        mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi{ sizeof(mi) };
    if (mon && GetMonitorInfoA(mon, &mi)) {
        w = mi.rcMonitor.right  - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top;
    } else {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
        x = 0;
        y = 0;
    }
}

// ---- Window restyle -------------------------------------------------------

// Strip the bordered styles, add WS_POPUP, reposition / resize to monitor.
// Idempotent: applying twice yields the same final state.
void apply_borderless(HWND wnd, int x, int y, int w, int h) {
    if (!wnd || !IsWindow(wnd)) return;

    constexpr LONG_PTR kStripStyle =
        WS_OVERLAPPEDWINDOW |   // umbrella; below ones are explicit guards
        WS_CAPTION    | WS_THICKFRAME | WS_BORDER     | WS_DLGFRAME |
        WS_SYSMENU    | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    constexpr LONG_PTR kStripExStyle =
        WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME |
        WS_EX_STATICEDGE;

    LONG_PTR style    = GetWindowLongPtrA(wnd, GWL_STYLE);
    LONG_PTR ex_style = GetWindowLongPtrA(wnd, GWL_EXSTYLE);

    LONG_PTR new_style    = (style & ~kStripStyle) | WS_POPUP;
    LONG_PTR new_ex_style = ex_style & ~kStripExStyle;

    bool style_changed = (new_style != style) || (new_ex_style != ex_style);

    if (style_changed) {
        SetWindowLongPtrA(wnd, GWL_STYLE,   new_style);
        SetWindowLongPtrA(wnd, GWL_EXSTYLE, new_ex_style);
        mtr::log::info(
            "windowmode: restyle hwnd=%p  style 0x%08lX -> 0x%08lX  ex 0x%08lX -> 0x%08lX",
            wnd,
            (unsigned long)style,    (unsigned long)new_style,
            (unsigned long)ex_style, (unsigned long)new_ex_style);
    }

    // SWP_FRAMECHANGED forces the new style to take effect (recompute
    // non-client area). We always set position + size: idempotent, and
    // catches the case where Reset is called after the user dragged or
    // resized the window. SWP_NOACTIVATE so we don't steal focus on an
    // alt-tab return.
    SetWindowPos(wnd, HWND_TOP,
                 x, y, w, h,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ---- Core rewrite, shared by CreateDevice + Reset entry points ------------

// True if `-mtrasi-keep-dxresolution` is present on the cmdline. Mirrors the
// cmdline_hook check; used here to opt out of the borderless-monitor resize
// (the dual-local LAN test needs two true-windowed Wilburs that don't fight
// for fullscreen focus).
bool has_keep_dxres_cmdline() {
    LPSTR line = GetCommandLineA();
    return line && std::strstr(line, "-mtrasi-keep-dxresolution") != nullptr;
}

// Parse the cmdline-requested `-dxresolution=WxH` into (out_w, out_h). Used by
// the keep-dxresolution path so the swapchain backbuffer + window client area
// can both honor the user's explicit dims. Returns false if not present or
// malformed.
bool parse_cmdline_dxres(int& out_w, int& out_h) {
    LPSTR line = GetCommandLineA();
    if (!line) return false;
    const char* needle = "-dxresolution=";
    const char* p = std::strstr(line, needle);
    if (!p) return false;
    const char* q = p + std::strlen(needle);
    int w = 0;
    while (*q >= '0' && *q <= '9') { w = w * 10 + (*q - '0'); ++q; }
    if (*q != 'x' && *q != 'X') return false;
    ++q;
    int h = 0;
    while (*q >= '0' && *q <= '9') { h = h * 10 + (*q - '0'); ++q; }
    if (w <= 0 || h <= 0) return false;
    out_w = w;
    out_h = h;
    return true;
}

// True windowed mode: force the focus window into "normal app" style with
// caption + system menu + thick resize frame + min/max boxes, size its
// CLIENT area to (w x h), and position at top-left of monitor.
//
// Engine creates TWO D3D9 devices on boot: a small loader device on a
// window with native windowed styles, and the real game device on a
// window with WS_POPUP (no chrome — engine's "fullscreen" mode). To get
// a draggable window for the game's actual device, we have to ADD the
// windowed styles (and strip WS_POPUP) on whichever window we get.
void apply_windowed(HWND wnd, int mon_x, int mon_y, int w, int h) {
    if (!wnd || !IsWindow(wnd)) return;

    LONG_PTR style    = GetWindowLongPtrA(wnd, GWL_STYLE);
    LONG_PTR ex_style = GetWindowLongPtrA(wnd, GWL_EXSTYLE);

    // WS_OVERLAPPEDWINDOW = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
    //                       WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
    // i.e. the "ordinary draggable resizable app window" style set.
    // WS_OVERLAPPED is 0; OR'ing it is a no-op but documents intent.
    constexpr LONG_PTR kForceStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    constexpr LONG_PTR kStripStyle = WS_POPUP;

    LONG_PTR new_style    = (style & ~kStripStyle) | kForceStyle;
    LONG_PTR new_ex_style = ex_style;
    const bool style_changed = (new_style != style);

    if (style_changed) {
        SetWindowLongPtrA(wnd, GWL_STYLE,   new_style);
        SetWindowLongPtrA(wnd, GWL_EXSTYLE, new_ex_style);
    }

    // Compute outer rect including non-client area (caption + frame) using
    // the NEW style so cx/cy in SetWindowPos sizes the client area exactly
    // to w x h.
    RECT r{0, 0, w, h};
    AdjustWindowRectEx(&r, static_cast<DWORD>(new_style), FALSE,
                       static_cast<DWORD>(new_ex_style));
    const int outer_w = r.right  - r.left;
    const int outer_h = r.bottom - r.top;

    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (style_changed) flags |= SWP_FRAMECHANGED;
    SetWindowPos(wnd, HWND_TOP, mon_x, mon_y, outer_w, outer_h, flags);

    // Sync engine's cached client dims so its viewport / aspect math uses
    // the actual window size, not pre-resize monitor dims.
    patch_engine_client_dims(w, h);

    mtr::log::info(
        "windowmode: windowed style hwnd=%p  style 0x%08lX -> 0x%08lX  "
        "client=%dx%d outer=%dx%d at (%d,%d)",
        wnd,
        (unsigned long)style, (unsigned long)new_style,
        w, h, outer_w, outer_h, mon_x, mon_y);
}

void do_rewrite(D3DPRESENT_PARAMETERS* pp, HWND focus_window) {
    if (!g_enabled.load()) return;

    HWND wnd = focus_window;
    if (!wnd && pp) wnd = pp->hDeviceWindow;

    int mon_w = 0, mon_h = 0, mon_x = 0, mon_y = 0;
    monitor_dims_for_window(wnd, mon_w, mon_h, mon_x, mon_y);

    const bool keep_dxres = has_keep_dxres_cmdline();
    int dxres_w = 0, dxres_h = 0;
    const bool have_dxres = keep_dxres && parse_cmdline_dxres(dxres_w, dxres_h);

    if (pp) {
        // Force windowed mode. The game (or DXVK_d3d8 translating from a
        // game-requested fullscreen pp8) may still pass Windowed=FALSE
        // here; we override so DXVK creates a windowed swapchain.
        pp->Windowed = TRUE;

        if (keep_dxres && have_dxres) {
            // True windowed mode: swapchain at the cmdline-requested dims.
            // Overrides any stale engine-side value too (engine sometimes
            // passes 0x0 expecting us to fill it; we fill with the
            // user-requested dims, not monitor dims).
            pp->BackBufferWidth  = static_cast<UINT>(dxres_w);
            pp->BackBufferHeight = static_cast<UINT>(dxres_h);
        } else {
            // Borderless-fullscreen path (default). Backbuffer = monitor
            // dims unless engine already filled in something smaller.
            if (pp->BackBufferWidth  == 0) pp->BackBufferWidth  = static_cast<UINT>(mon_w);
            if (pp->BackBufferHeight == 0) pp->BackBufferHeight = static_cast<UINT>(mon_h);
        }

        // Refresh rate is ignored in windowed mode but some drivers
        // complain if it's non-zero with Windowed=TRUE. Zero it.
        pp->FullScreen_RefreshRateInHz = 0;
    }

    // Window placement. Two paths:
    //
    // (default) Borderless-fullscreen — strip native styles (caption etc.),
    //           add WS_POPUP, size + position to fill the monitor. Matches
    //           dxwrapper's `EnableWindowMode = 1 + FullscreenWindowMode = 1`.
    //
    // (-mtrasi-keep-dxresolution) True windowed — keep engine's native styles
    //           (WS_CAPTION, WS_THICKFRAME, sysmenu, min/max boxes), size
    //           CLIENT area to the cmdline dxres. The user can drag by the
    //           title bar and resize via the frame.
    if (wnd) {
        if (keep_dxres) {
            const int w = have_dxres ? dxres_w : 1280;
            const int h = have_dxres ? dxres_h : 720;
            apply_windowed(wnd, mon_x, mon_y, w, h);
            // Arm the per-frame enforcement loop. ~300 frames at 60-240 Hz
            // is 1.25-5s — enough for engine init to settle. The loop is a
            // no-op if the window already matches our requested dims.
            g_enforce_frames.store(300, std::memory_order_relaxed);
        } else {
            apply_borderless(wnd, mon_x, mon_y, mon_w, mon_h);
        }
        std::scoped_lock lk(g_mu);
        g_last_styled = wnd;
        g_last_mon_w  = mon_w;
        g_last_mon_h  = mon_h;
        g_last_mon_x  = mon_x;
        g_last_mon_y  = mon_y;
    }
}

// ---- ChangeDisplaySettings hooks -----------------------------------------

LONG WINAPI hk_CDSA(DEVMODEA* dm, DWORD flags) {
    if (g_enabled.load()) {
        g_cds_blocks.fetch_add(1);
        return DISP_CHANGE_SUCCESSFUL;  // pretend we did the mode change
    }
    return g_orig_cds_a ? g_orig_cds_a(dm, flags) : DISP_CHANGE_FAILED;
}

LONG WINAPI hk_CDSW(DEVMODEW* dm, DWORD flags) {
    if (g_enabled.load()) {
        g_cds_blocks.fetch_add(1);
        return DISP_CHANGE_SUCCESSFUL;
    }
    return g_orig_cds_w ? g_orig_cds_w(dm, flags) : DISP_CHANGE_FAILED;
}

LONG WINAPI hk_CDSExA(LPCSTR dev, DEVMODEA* dm, HWND wnd, DWORD flags, LPVOID param) {
    if (g_enabled.load()) {
        g_cds_blocks.fetch_add(1);
        return DISP_CHANGE_SUCCESSFUL;
    }
    return g_orig_cds_ex_a ? g_orig_cds_ex_a(dev, dm, wnd, flags, param) : DISP_CHANGE_FAILED;
}

LONG WINAPI hk_CDSExW(LPCWSTR dev, DEVMODEW* dm, HWND wnd, DWORD flags, LPVOID param) {
    if (g_enabled.load()) {
        g_cds_blocks.fetch_add(1);
        return DISP_CHANGE_SUCCESSFUL;
    }
    return g_orig_cds_ex_w ? g_orig_cds_ex_w(dev, dm, wnd, flags, param) : DISP_CHANGE_FAILED;
}

void install_cds_hooks() {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) {
        mtr::log::info("windowmode: user32.dll not loaded?? skipping CDS hooks");
        return;
    }
    void* pA   = reinterpret_cast<void*>(GetProcAddress(u32, "ChangeDisplaySettingsA"));
    void* pW   = reinterpret_cast<void*>(GetProcAddress(u32, "ChangeDisplaySettingsW"));
    void* pExA = reinterpret_cast<void*>(GetProcAddress(u32, "ChangeDisplaySettingsExA"));
    void* pExW = reinterpret_cast<void*>(GetProcAddress(u32, "ChangeDisplaySettingsExW"));

    auto try_one = [](void* target, void* detour, void** orig_out, const char* name) {
        if (!target) {
            mtr::log::info("windowmode: GetProcAddress(%s) failed", name);
            return;
        }
        if (MH_CreateHook(target, detour, orig_out) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            mtr::log::info("windowmode: MH_CreateHook/Enable(%s) failed", name);
        } else {
            mtr::log::info("windowmode: hooked %s at %p", name, target);
        }
    };
    try_one(pA,   reinterpret_cast<void*>(&hk_CDSA),   reinterpret_cast<void**>(&g_orig_cds_a),    "ChangeDisplaySettingsA");
    try_one(pW,   reinterpret_cast<void*>(&hk_CDSW),   reinterpret_cast<void**>(&g_orig_cds_w),    "ChangeDisplaySettingsW");
    try_one(pExA, reinterpret_cast<void*>(&hk_CDSExA), reinterpret_cast<void**>(&g_orig_cds_ex_a), "ChangeDisplaySettingsExA");
    try_one(pExW, reinterpret_cast<void*>(&hk_CDSExW), reinterpret_cast<void**>(&g_orig_cds_ex_w), "ChangeDisplaySettingsExW");
}

} // namespace

// ---- Public API -----------------------------------------------------------

bool enabled() { return g_enabled.load(); }

void set_enabled(bool on) {
    bool was = g_enabled.exchange(on);
    if (was != on) {
        // Counters reset on enable/disable transitions so the diagnostic
        // numbers reflect "since last toggle" rather than lifetime — easier
        // to interpret while testing.
        g_create_rewrites.store(0);
        g_reset_rewrites.store(0);
        g_cds_blocks.store(0);
        save_ini();
        mtr::log::info("windowmode: %s", on ? "enabled" : "disabled");
    }
}

void apply_for_create_device(D3DPRESENT_PARAMETERS* pp, HWND focus_window) {
    if (!g_enabled.load()) return;
    if (!pp && !focus_window) return;
    do_rewrite(pp, focus_window);
    g_create_rewrites.fetch_add(1);
}

void apply_for_reset(D3DPRESENT_PARAMETERS* pp) {
    if (!g_enabled.load()) return;
    if (!pp) return;
    // For Reset there's no separate focus_window argument — the device
    // already owns its window, and pp->hDeviceWindow is what the engine
    // is asking us to reset against. Pass null as focus so do_rewrite
    // falls back to pp->hDeviceWindow.
    do_rewrite(pp, nullptr);
    g_reset_rewrites.fetch_add(1);
}

unsigned long long create_device_rewrites() { return g_create_rewrites.load(); }
unsigned long long reset_rewrites()          { return g_reset_rewrites.load(); }
unsigned long long change_display_settings_blocks() { return g_cds_blocks.load(); }

HWND last_styled_window() { std::scoped_lock lk(g_mu); return g_last_styled; }
int  last_monitor_w()     { std::scoped_lock lk(g_mu); return g_last_mon_w; }
int  last_monitor_h()     { std::scoped_lock lk(g_mu); return g_last_mon_h; }

// ---- ShowWindow hook: block engine's SW_MAXIMIZE on the managed window ----
//
// Engine global `g_window_show_state` @ 0x006FBD14 holds nCmdShow; defaults
// to SW_MAXIMIZE (3) per agent IDA findings. Engine's
// `game_window_show_and_update_client_size` @ 0x005635E0 calls
// `ShowWindow(g_focus_hwnd, g_window_show_state)` — maximizing the window
// to fill the monitor. Even though our styles say WS_OVERLAPPEDWINDOW
// (caption + frame visible), the window is MAXIMIZED → covers the whole
// screen, looks like fullscreen.
//
// Two-layer fix: (a) write SW_SHOWNORMAL into g_window_show_state at install
// so any engine read sees "show normal"; (b) hook user32!ShowWindow to
// rewrite SW_MAXIMIZE → SW_SHOWNORMAL on our managed window (defense for
// any ShowWindow call that uses a literal SW_MAXIMIZE rather than the
// global). Plus patch the engine's cached client dims so its viewport
// math matches our actual window size.

constexpr uintptr_t kGameWindowShowStateVA  = 0x006FBD14;
constexpr uintptr_t kGameClientWidthVA      = 0x006FBC38;
constexpr uintptr_t kGameClientHeightVA     = 0x006FBC3C;

using PFN_ShowWindow = BOOL (WINAPI*)(HWND, int);
PFN_ShowWindow g_orig_ShowWindow = nullptr;

BOOL WINAPI hk_ShowWindow(HWND hwnd, int nCmdShow) {
    // Block SW_MAXIMIZE (3) and SW_SHOWMAXIMIZED (3) on our managed window
    // when keep-dxres is active — substitute SW_SHOWNORMAL (1). Other
    // values (SW_HIDE, SW_SHOW, SW_SHOWNA, SW_MINIMIZE etc) pass through
    // unchanged so the engine can still minimize/restore normally.
    if (has_keep_dxres_cmdline() && (nCmdShow == SW_MAXIMIZE || nCmdShow == SW_SHOWMAXIMIZED)) {
        nCmdShow = SW_SHOWNORMAL;
    }
    return g_orig_ShowWindow(hwnd, nCmdShow);
}

bool install_showwindow_hook() {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(u32, "ShowWindow"));
    if (!target) return false;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hk_ShowWindow),
                      reinterpret_cast<void**>(&g_orig_ShowWindow)) != MH_OK) {
        mtr::log::info("windowmode: install_showwindow_hook MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        mtr::log::info("windowmode: install_showwindow_hook MH_EnableHook FAILED");
        return false;
    }
    mtr::log::info("windowmode: hooked user32!ShowWindow @ %p — SW_MAXIMIZE on managed window rewritten to SW_SHOWNORMAL", target);
    return true;
}

// Patch engine globals to "windowed normal" state. Called from install() if
// keep-dxres flag is on. Idempotent.
void patch_engine_window_globals() {
    if (!has_keep_dxres_cmdline()) return;
    __try {
        *reinterpret_cast<volatile int*>(kGameWindowShowStateVA) = SW_SHOWNORMAL;
        mtr::log::info("windowmode: patched g_window_show_state @ 0x%08X = SW_SHOWNORMAL (1)",
                       static_cast<unsigned>(kGameWindowShowStateVA));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("windowmode: patch_engine_window_globals FAULT writing show_state");
    }
}

// Called from apply_windowed to update engine's cached client dims so the
// engine's viewport / aspect / camera math uses our actual window size,
// not whatever (probably monitor-sized) dims the engine captured before we
// resized.
void patch_engine_client_dims(int w, int h) {
    if (!has_keep_dxres_cmdline()) return;
    __try {
        *reinterpret_cast<volatile int*>(kGameClientWidthVA)  = w;
        *reinterpret_cast<volatile int*>(kGameClientHeightVA) = h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- WndProc hook: game runs in any focus state ---------------------------
//
// RE'd 2026-05-12 via IDA: game_MainWndProc lives at VA 0x0056F990. On
// WM_ACTIVATEAPP it:
//   - wParam=0 (LOSE focus): calls ChangeDisplaySettingsA(NULL, CDS_FULLSCREEN)
//     at 0x0056FBAC → desktop is yanked back to native + main loop pauses;
//   - wParam=1 (GAIN focus): calls App vtable slot [0x38] (=index 14) which
//     is the "re-apply fullscreen + restore SW_MAXIMIZE state" path.
//
// Either branch breaks any non-fullscreen / multi-window scenario. The 2007
// engine pauses itself on focus loss and snaps back to monitor-sized exclusive
// fullscreen on focus gain — both decisions are baked into WM_ACTIVATEAPP
// handling.
//
// Fix (RULE №1, no crutches): swallow WM_ACTIVATEAPP outright. The engine
// never knows it lost focus, so neither the pause-on-defocus nor the
// re-fullscreen-on-refocus paths ever fire. Game keeps running normally
// regardless of whether its window is foreground.
//
// Companion fix in dinput_hook.cpp: hook SetCooperativeLevel to strip
// DISCL_FOREGROUND so DI returns real input even when the window isn't
// foreground (without that, swallowing WM_ACTIVATEAPP alone leaves the
// game running but blind in the background).

constexpr uintptr_t kGameMainWndProcVA = 0x0056F990;

using PFN_WndProc = LRESULT (CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
PFN_WndProc g_orig_game_wndproc = nullptr;

LRESULT CALLBACK hk_game_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ACTIVATEAPP) {
        // Don't swallow — the engine's WM_ACTIVATEAPP(1) handler does BOTH
        // (a) the initial "push first screen" / init-gate work that has to
        //     run once for the engine to leave startup, AND
        // (b) the "re-fullscreen via App vtable[14]" path we want to avoid.
        //
        // Swallowing kills (a) and the engine hangs in <startup> forever
        // (observed: client without focus never pushes its first screen).
        //
        // Instead: ALWAYS rewrite wParam to TRUE. The engine sees the
        // window as permanently active; it never receives a "lose focus"
        // message so:
        //   - The CDS restore at 0x0056FBAC (focus-loss branch) never fires.
        //   - App vt[14] (focus-gain branch) runs exactly once at startup,
        //     completing init, then never re-fires because the wp=0→wp=1
        //     transition never happens again.
        // Net effect: engine init completes regardless of real focus state,
        // and the re-fullscreen-on-defocus-return loop is dead.
        wp = TRUE;
    }
    return g_orig_game_wndproc(hwnd, msg, wp, lp);
}

bool install_wndproc_hook() {
    void* target = reinterpret_cast<void*>(kGameMainWndProcVA);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hk_game_wndproc),
                      reinterpret_cast<void**>(&g_orig_game_wndproc)) != MH_OK) {
        mtr::log::info(
            "windowmode: install_wndproc_hook MH_CreateHook FAILED "
            "@ 0x%08X (game_MainWndProc)",
            static_cast<unsigned>(kGameMainWndProcVA));
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        mtr::log::info(
            "windowmode: install_wndproc_hook MH_EnableHook FAILED "
            "@ 0x%08X (game_MainWndProc)",
            static_cast<unsigned>(kGameMainWndProcVA));
        return false;
    }
    mtr::log::info(
        "windowmode: hooked game_MainWndProc @ 0x%08X — WM_ACTIVATEAPP "
        "swallowed (engine no longer pauses on focus loss or re-applies "
        "fullscreen on focus gain). Companion: dinput_hook strips "
        "DISCL_FOREGROUND so input keeps flowing.",
        static_cast<unsigned>(kGameMainWndProcVA));
    return true;
}

void on_end_scene_tick() {
    int n = g_enforce_frames.load(std::memory_order_relaxed);
    if (n <= 0) return;

    int dxres_w = 0, dxres_h = 0;
    if (!parse_cmdline_dxres(dxres_w, dxres_h)) {
        g_enforce_frames.store(0, std::memory_order_relaxed);
        return;
    }

    HWND wnd;
    int mx, my;
    {
        std::scoped_lock lk(g_mu);
        wnd = g_last_styled;
        mx = g_last_mon_x;
        my = g_last_mon_y;
    }
    if (!wnd || !IsWindow(wnd)) {
        g_enforce_frames.store(0, std::memory_order_relaxed);
        return;
    }

    // Check current client area against requested dims. SetWindowPos is
    // a no-op when nothing changed, but we still want to skip re-issuing
    // the style change every frame — only when the engine has actually
    // resized us away from where we want.
    RECT rc{};
    if (!GetClientRect(wnd, &rc)) {
        g_enforce_frames.store(n - 1, std::memory_order_relaxed);
        return;
    }
    const int cur_w = rc.right  - rc.left;
    const int cur_h = rc.bottom - rc.top;
    if (cur_w != dxres_w || cur_h != dxres_h) {
        apply_windowed(wnd, mx, my, dxres_w, dxres_h);
    }
    g_enforce_frames.store(n - 1, std::memory_order_relaxed);
}

// ---- Persistence ----------------------------------------------------------

void load_ini() {
    char ini[MAX_PATH] = {0};
    if (!resolve_ini_path(ini, sizeof(ini))) return;
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        // First run — keep default-on; persist once so the user can edit.
        save_ini();
        return;
    }
    char buf[16] = {0};
    GetPrivateProfileStringA("windowmode", "enabled", "1", buf, sizeof(buf), ini);
    bool en = (buf[0] != '0');
    g_enabled.store(en);
    mtr::log::info("windowmode: ini -> enabled=%d", (int)en);
}

void save_ini() {
    char ini[MAX_PATH] = {0};
    if (!resolve_ini_path(ini, sizeof(ini))) return;
    WritePrivateProfileStringA("windowmode", "enabled",
        g_enabled.load() ? "1" : "0", ini);
}

// Trivially debounced — currently set_enabled saves immediately, so this
// is just a defensive entry point for any future caller that wants to
// flag "save soon" without committing to disk this frame.
void request_save() { save_ini(); }

// ---- install --------------------------------------------------------------

void install() {
    load_ini();
    install_cds_hooks();
    install_wndproc_hook();
    install_showwindow_hook();
    patch_engine_window_globals();
    mtr::log::info("windowmode: install done (enabled=%d)", (int)g_enabled.load());
}

} // namespace mtr::windowmode
