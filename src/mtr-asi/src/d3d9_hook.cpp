// D3D9 hook module — core (vtable capture + IDirect3D9::CreateDevice +
// IDirect3DDevice9::EndScene/Reset + install dispatcher).
//
// The engine-side hooks (camera, sprite, diag) live under d3d9_hook/.
// This TU owns the device-creation lifecycle (windowmode rewrite, frame
// tick infra, menu draw, FPS limiter) plus the vtable capture sequence
// that produces the function pointers handed off to the sub-TUs.

#include "d3d9_hook/d3d9_internal.h"

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }
namespace mtr::menu {
    void on_end_scene(IDirect3DDevice9* dev);
    void on_reset_pre(IDirect3DDevice9* dev);
    void on_reset_post(IDirect3DDevice9* dev);
}
namespace mtr::vis_test_probe  { void frame_tick(); }
namespace mtr::scene_vis_log   { void frame_tick(); }
namespace mtr::peripheral_cull_probe { void frame_tick(); }
namespace mtr::fps_limit       { void tick(); }
namespace mtr::sim_decouple    { void on_render_frame(); }
namespace mtr::dt_correctness  { void tick_snapshot_log(); }
namespace mtr::windowmode {
    void apply_for_create_device(D3DPRESENT_PARAMETERS* pp, HWND focus_window);
    void apply_for_reset(D3DPRESENT_PARAMETERS* pp);
    void on_end_scene_tick();
}
namespace mtr::msaa {
    void apply_for_create_device(D3DPRESENT_PARAMETERS* pp,
                                 IDirect3D9* d3d, unsigned int adapter,
                                 D3DDEVTYPE devtype);
    void apply_for_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp);
}

namespace mtr::d3d9hook {

namespace {

detail::PFN_CreateDevice g_orig_CreateDevice = nullptr;
detail::PFN_EndScene     g_orig_EndScene     = nullptr;
detail::PFN_Reset        g_orig_Reset        = nullptr;

// Early hook on Direct3DCreate9 itself — installed from DllMain before
// the engine's WinMain runs. When the engine calls Direct3DCreate9, our
// hook fires, reads the returned IDirect3D9's vtable, and hooks
// CreateDevice on it BEFORE the engine has a chance to call CreateDevice.
// This catches MSAA / windowmode overrides on cold launch (the previous
// deferred-thread install ran AFTER the engine's CreateDevice, missing
// it). 2026-05-09 fix.
using PFN_Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
PFN_Direct3DCreate9_t g_orig_Direct3DCreate9 = nullptr;
std::atomic<bool>     g_create_device_hooked{false};

std::atomic<int> g_last_pp_width{0};
std::atomic<int> g_last_pp_height{0};

void log_pp(const D3DPRESENT_PARAMETERS* pp, const char* tag) {
    if (!pp) return;
    mtr::log::info("[%s] pp: Windowed=%d %ux%u fmt=%u SwapEffect=%u hWnd=%p",
                   tag, pp->Windowed, pp->BackBufferWidth, pp->BackBufferHeight,
                   pp->BackBufferFormat, pp->SwapEffect, pp->hDeviceWindow);
    g_last_pp_width  = static_cast<int>(pp->BackBufferWidth);
    g_last_pp_height = static_cast<int>(pp->BackBufferHeight);
}

// Forward decl — hk_CreateDevice is defined below; the early-install path
// needs to reference it before that point.
HRESULT STDMETHODCALLTYPE hk_CreateDevice(
    IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface);

// Hook IDirect3D9::CreateDevice on the live IDirect3D9 vtable. Idempotent
// — guarded by g_create_device_hooked atomic so the deferred-thread path
// (capture_vtables_and_hook) and our Direct3DCreate9 fast-path don't
// double-hook. Returns true if hook is now active (either we just hooked
// or it was already hooked).
bool hook_create_device_on_vtable(IDirect3D9* d3d) {
    if (!d3d) return false;
    bool expected = false;
    if (!g_create_device_hooked.compare_exchange_strong(expected, true)) {
        return true; // already done by another path
    }
    void** vt = *reinterpret_cast<void***>(d3d);
    void* p_CreateDevice = vt[16]; // IDirect3D9::CreateDevice slot
    if (MH_CreateHook(p_CreateDevice, &hk_CreateDevice,
                      reinterpret_cast<void**>(&g_orig_CreateDevice)) != MH_OK) {
        mtr::log::info("d3d9: early MH_CreateHook(CreateDevice) failed");
        g_create_device_hooked.store(false);
        return false;
    }
    if (MH_EnableHook(p_CreateDevice) != MH_OK) {
        mtr::log::info("d3d9: early MH_EnableHook(CreateDevice) failed");
        return false;
    }
    mtr::log::info("d3d9: CreateDevice hooked early via Direct3DCreate9 (vtable=%p slot=%p)",
                   vt, p_CreateDevice);
    return true;
}

IDirect3D9* WINAPI hk_Direct3DCreate9(UINT SDKVersion) {
    IDirect3D9* d3d = g_orig_Direct3DCreate9 ? g_orig_Direct3DCreate9(SDKVersion)
                                             : nullptr;
    mtr::log::info("Direct3DCreate9: SDKVersion=%u -> %p", SDKVersion, d3d);
    if (d3d) {
        // Ensure CreateDevice is hooked before the caller (the engine)
        // calls into it. After this returns, the engine's d3d->CreateDevice
        // will route through hk_CreateDevice.
        hook_create_device_on_vtable(d3d);
    }
    return d3d;
}

HRESULT STDMETHODCALLTYPE hk_CreateDevice(
    IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface)
{
    mtr::log::info("CreateDevice: adapter=%u type=%u flags=0x%08lX hFocus=%p",
                   Adapter, DeviceType, BehaviorFlags, hFocusWindow);
    log_pp(pPresentationParameters, "CreateDevice (pre)");

    // Phase B (DXVK migration): apply native borderless-fullscreen rewrite
    // before the engine actually creates the device. Mutates pp in place
    // (Windowed=TRUE, fills BB dims if zero) and restyles the focus window
    // to borderless filling the monitor. No-op when toggle is OFF.
    mtr::windowmode::apply_for_create_device(pPresentationParameters, hFocusWindow);
    // MSAA override: cap-checks against the device + back-buffer format
    // and writes pp->MultiSampleType / Quality / SwapEffect. Runs AFTER
    // windowmode (which may flip pp->Windowed) so the cap query uses the
    // final windowed state.
    mtr::msaa::apply_for_create_device(pPresentationParameters, This,
                                       Adapter, DeviceType);
    log_pp(pPresentationParameters, "CreateDevice (post-rewrite)");

    HRESULT hr = g_orig_CreateDevice(This, Adapter, DeviceType, hFocusWindow,
                                     BehaviorFlags, pPresentationParameters,
                                     ppReturnedDeviceInterface);
    mtr::log::info("CreateDevice -> hr=0x%08lX device=%p", hr,
                   ppReturnedDeviceInterface ? *ppReturnedDeviceInterface : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_EndScene(IDirect3DDevice9* dev) {
    // Enforce keep-dxresolution windowed size against engine re-maximize
    // attempts during init. No-op once engine settles or keep-dxres is off.
    mtr::windowmode::on_end_scene_tick();
    // Snapshot per-frame counters before the menu draws, so the UI sees the
    // just-completed frame's numbers (not in-flight).
    mtr::vis_test_probe::frame_tick();
    mtr::scene_vis_log::frame_tick();
    mtr::peripheral_cull_probe::frame_tick();
    // Decouple telemetry: count the render frame for the EMA. Cheap (one
    // QPC + one atomic add). Always-on; mode==OFF doesn't affect it.
    mtr::sim_decouple::on_render_frame();
    // dt-correctness state snapshot. Internally rate-limited to one
    // [dtc-snap] log line per ~2 wall-clock seconds.
    mtr::dt_correctness::tick_snapshot_log();
    mtr::menu::on_end_scene(dev);
    HRESULT hr = g_orig_EndScene(dev);
    // FPS cap: spin AFTER EndScene returns. Frame is committed; spinning
    // here doesn't block GPU work and happens before Present (where vsync,
    // if enabled, will wait further).
    mtr::fps_limit::tick();
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    log_pp(pp, "Reset (pre)");
    // Phase B: re-apply borderless-fullscreen rewrite. Reset fires on
    // alt-tab return / window resize / device-lost recovery — without
    // this the engine could yank the device back into exclusive
    // fullscreen on alt-tab. No-op when toggle is OFF.
    mtr::windowmode::apply_for_reset(pp);
    // MSAA override on Reset (alt-tab return / device-lost recovery /
    // resolution change). Same cap-check pipeline as CreateDevice; the
    // reset path queries IDirect3D9 from the device.
    mtr::msaa::apply_for_reset(dev, pp);
    log_pp(pp, "Reset (post-rewrite)");
    mtr::menu::on_reset_pre(dev);
    HRESULT hr = g_orig_Reset(dev, pp);
    if (SUCCEEDED(hr)) mtr::menu::on_reset_post(dev);
    return hr;
}

bool capture_vtables_and_hook() {
    HMODULE d3d9_mod = GetModuleHandleA("d3d9.dll");
    if (!d3d9_mod) d3d9_mod = LoadLibraryA("d3d9.dll");
    if (!d3d9_mod) {
        mtr::log::info("d3d9: LoadLibrary(d3d9.dll) failed");
        return false;
    }

    using PFN_Direct3DCreate9 = IDirect3D9*(WINAPI*)(UINT);
    auto pDirect3DCreate9 = reinterpret_cast<PFN_Direct3DCreate9>(
        GetProcAddress(d3d9_mod, "Direct3DCreate9"));
    if (!pDirect3DCreate9) {
        mtr::log::info("d3d9: GetProcAddress(Direct3DCreate9) failed");
        return false;
    }

    IDirect3D9* d3d = pDirect3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        mtr::log::info("d3d9: Direct3DCreate9 returned null");
        return false;
    }

    void** d3d_vt = *reinterpret_cast<void***>(d3d);
    void* p_CreateDevice = d3d_vt[16]; // IDirect3D9::CreateDevice

    HWND hidden = CreateWindowExA(0, "STATIC", "mtr-dummy", WS_OVERLAPPEDWINDOW,
                                  0, 0, 100, 100, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!hidden) {
        d3d->Release();
        mtr::log::info("d3d9: dummy CreateWindowExA failed");
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = hidden;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* dummy = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hidden,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                   &pp, &dummy);
    if (FAILED(hr) || !dummy) {
        DestroyWindow(hidden);
        d3d->Release();
        mtr::log::info("d3d9: dummy CreateDevice failed hr=0x%08lX", hr);
        return false;
    }

    void** dev_vt = *reinterpret_cast<void***>(dummy);
    void* p_EndScene       = dev_vt[42];
    void* p_Reset          = dev_vt[16];
    void* p_SetTransform   = dev_vt[44];
    void* p_SetRenderState = dev_vt[57];
    void* p_SetClipPlane   = dev_vt[55];

    mtr::log::info("d3d9: vtables captured -- "
                   "IDirect3D9::CreateDevice=%p; IDirect3DDevice9::EndScene=%p Reset=%p "
                   "SetTransform=%p SetRenderState=%p SetClipPlane=%p",
                   p_CreateDevice, p_EndScene, p_Reset, p_SetTransform,
                   p_SetRenderState, p_SetClipPlane);

    dummy->Release();
    DestroyWindow(hidden);
    d3d->Release();

    // === Core hooks (this TU) =============================================

    bool ok = true;
    // CreateDevice may already have been hooked early via the
    // Direct3DCreate9 fast-path. Only hook it here if not yet done.
    if (!g_create_device_hooked.load()) {
        if (MH_CreateHook(p_CreateDevice, &hk_CreateDevice,
                          reinterpret_cast<void**>(&g_orig_CreateDevice)) != MH_OK) {
            mtr::log::info("d3d9: MH_CreateHook(CreateDevice) failed"); ok = false;
        } else if (MH_EnableHook(p_CreateDevice) != MH_OK) {
            mtr::log::info("d3d9: MH_EnableHook(CreateDevice) failed"); ok = false;
        } else {
            g_create_device_hooked.store(true);
        }
    }
    if (MH_CreateHook(p_EndScene, &hk_EndScene,
                      reinterpret_cast<void**>(&g_orig_EndScene)) != MH_OK) {
        mtr::log::info("d3d9: MH_CreateHook(EndScene) failed"); ok = false;
    }
    if (MH_CreateHook(p_Reset, &hk_Reset,
                      reinterpret_cast<void**>(&g_orig_Reset)) != MH_OK) {
        mtr::log::info("d3d9: MH_CreateHook(Reset) failed"); ok = false;
    }
    if (!ok) return false;
    if (MH_EnableHook(p_EndScene)     != MH_OK ||
        MH_EnableHook(p_Reset)        != MH_OK)
    {
        mtr::log::info("d3d9: MH_EnableHook failed for one of EndScene/Reset");
        return false;
    }

    // === Sub-TU hooks =====================================================

    detail::install_diag_hooks(p_SetClipPlane, p_SetRenderState);
    detail::install_sprite_hooks(p_SetTransform);
    detail::install_camera_hooks();

    mtr::log::info("d3d9: hooks armed");
    return true;
}

DWORD WINAPI deferred_thread(LPVOID) {
    for (int i = 0; i < 300; ++i) {
        if (GetModuleHandleA("d3d9.dll")) break;
        Sleep(100);
    }
    if (!GetModuleHandleA("d3d9.dll")) {
        mtr::log::info("d3d9: d3d9.dll never loaded -- giving up");
        return 0;
    }
    capture_vtables_and_hook();
    return 0;
}

} // namespace

int last_pp_width()  { return g_last_pp_width.load(); }
int last_pp_height() { return g_last_pp_height.load(); }

// Hook Direct3DCreate9 from DllMain context. Safe to call before MinHook
// is initialized? No — must run AFTER MH_Initialize. Called from DllMain
// post-MH_Initialize so the engine's later Direct3DCreate9 (in WinMain)
// is intercepted in time to install CreateDevice on its IDirect3D9.
void install_early() {
    HMODULE d3d9_mod = GetModuleHandleA("d3d9.dll");
    if (!d3d9_mod) {
        // d3d9.dll not yet loaded. Wilbur.exe imports d3d9.dll so the
        // PE loader normally has it mapped before any DllMain runs;
        // if it's not here, the engine will fail anyway. Fall back to
        // the deferred-thread path which polls.
        mtr::log::info("d3d9: install_early — d3d9.dll not loaded yet, deferring");
        return;
    }
    auto pCreate9 = reinterpret_cast<PFN_Direct3DCreate9_t>(
        GetProcAddress(d3d9_mod, "Direct3DCreate9"));
    if (!pCreate9) {
        mtr::log::info("d3d9: install_early — GetProcAddress(Direct3DCreate9) failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(pCreate9),
                      &hk_Direct3DCreate9,
                      reinterpret_cast<void**>(&g_orig_Direct3DCreate9)) != MH_OK) {
        mtr::log::info("d3d9: install_early — MH_CreateHook(Direct3DCreate9) failed");
        return;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(pCreate9)) != MH_OK) {
        mtr::log::info("d3d9: install_early — MH_EnableHook(Direct3DCreate9) failed");
        return;
    }
    mtr::log::info("d3d9: Direct3DCreate9 hooked early at %p", pCreate9);
}

void install() {
    HANDLE t = CreateThread(nullptr, 0, deferred_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

} // namespace mtr::d3d9hook
