// MSAA (multisample anti-aliasing) override.
//
// D3D9 multisample is configured via D3DPRESENT_PARAMETERS at CreateDevice
// time and re-applied on every Reset. Wilbur's engine doesn't expose a
// quality slider for it; we mutate the present params on the way through
// our hk_CreateDevice / hk_Reset hooks (same pattern as windowmode).
//
// Caps gating:
//   IDirect3D9::CheckDeviceMultiSampleType(adapter, devtype, fmt, windowed,
//                                          type, &max_quality)
// must return S_OK before the type is usable. We probe both the chosen
// back-buffer format AND the auto-depth-stencil format (D24S8 by default
// for Wilbur). If EITHER fails the cap check, we fall back to the next
// lower sample count, all the way down to NONE. So a user request for 8x
// silently degrades to whatever the driver actually supports.
//
// SwapEffect note: D3D9 strictly requires D3DSWAPEFFECT_DISCARD when
// multisample is enabled. If the engine asked for COPY/FLIP, we override
// to DISCARD when MSAA > NONE — otherwise CreateDevice fails with
// INVALIDCALL. Cleared back when the user disables MSAA.
//
// Auto-depth-stencil: pp->EnableAutoDepthStencil drives the device's
// internal depth buffer. When true, the device matches MultiSampleType
// for us. When false, the engine creates the depth buffer manually via
// CreateDepthStencilSurface; that path is unhooked here. If MSAA appears
// not to work (geometry but no depth), it means the engine uses manual
// depth-stencil and we'd need to extend the hook surface.

#include "mtr/msaa.h"

#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdint>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::msaa {

namespace {

// ---- State ----------------------------------------------------------------

// 2026-05-09 user request: MSAA defaults ON at 16x. Cap-check at
// apply-time falls back through 16→8→4→2→NONE if the driver doesn't
// support the requested level for the back-buffer + depth-stencil
// format combo, so the worst case on weaker hardware is "best
// supported", not failure.
std::atomic<bool>    g_enabled{true};
std::atomic<uint32_t> g_sample_count{16};

// Last applied state — written after the cap check inside hk_CreateDevice
// so the menu can show what the device actually got (vs what was asked).
std::atomic<uint32_t> g_actual_count{0};
std::atomic<uint32_t> g_actual_quality{0};
std::atomic<unsigned long long> g_apply_count{0};
std::atomic<unsigned long long> g_create_overrides{0};
std::atomic<unsigned long long> g_reset_overrides {0};

// Saved SwapEffect / EnableAutoDepthStencil so we can restore the engine's
// original choice when MSAA flips off without a reset.
D3DSWAPEFFECT g_saved_swap_effect = D3DSWAPEFFECT_DISCARD;
bool          g_saved_swap_effect_valid = false;

D3DMULTISAMPLE_TYPE count_to_type(uint32_t n) {
    switch (n) {
        case 0:  return D3DMULTISAMPLE_NONE;
        case 2:  return D3DMULTISAMPLE_2_SAMPLES;
        case 3:  return D3DMULTISAMPLE_3_SAMPLES;
        case 4:  return D3DMULTISAMPLE_4_SAMPLES;
        case 5:  return D3DMULTISAMPLE_5_SAMPLES;
        case 6:  return D3DMULTISAMPLE_6_SAMPLES;
        case 7:  return D3DMULTISAMPLE_7_SAMPLES;
        case 8:  return D3DMULTISAMPLE_8_SAMPLES;
        case 16: return D3DMULTISAMPLE_16_SAMPLES;
        default: return D3DMULTISAMPLE_NONE;
    }
}

// Cap check helper. Tries the desired sample count; if it (or the depth
// buffer at the same count) fails the cap, walks down through standard
// counts until something works (or NONE). Sets *out_quality and returns
// the largest count that passes. NONE result = MSAA fully unsupported.
uint32_t pick_supported(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE devtype,
                        D3DFORMAT bb_fmt, D3DFORMAT depth_fmt,
                        BOOL windowed, uint32_t desired,
                        DWORD* out_quality)
{
    if (!d3d || desired == 0) {
        if (out_quality) *out_quality = 0;
        return 0;
    }
    static const uint32_t kSteps[] = { 16, 8, 4, 2, 0 };
    for (uint32_t cand : kSteps) {
        if (cand > desired) continue;
        if (cand == 0) {
            if (out_quality) *out_quality = 0;
            return 0;
        }
        D3DMULTISAMPLE_TYPE type = count_to_type(cand);
        DWORD q_color = 0, q_depth = 0;
        HRESULT hrc = d3d->CheckDeviceMultiSampleType(
            adapter, devtype, bb_fmt, windowed, type, &q_color);
        if (FAILED(hrc) || q_color == 0) continue;
        HRESULT hrd = d3d->CheckDeviceMultiSampleType(
            adapter, devtype, depth_fmt, windowed, type, &q_depth);
        if (FAILED(hrd) || q_depth == 0) continue;
        // Quality must satisfy both formats. Use the smaller as the cap;
        // 0 = lowest valid quality bucket — driver picks the typical
        // pattern. We pick 0 for stability across drivers.
        DWORD q = 0;
        if (q_color > 0 && q < q_color) q = 0;
        if (out_quality) *out_quality = q;
        return cand;
    }
    if (out_quality) *out_quality = 0;
    return 0;
}

void apply_to_pp(D3DPRESENT_PARAMETERS* pp,
                 IDirect3D9* d3d, UINT adapter, D3DDEVTYPE devtype,
                 const char* phase)
{
    if (!pp) return;

    // Save the engine's original SwapEffect so we can restore on disable.
    if (!g_saved_swap_effect_valid) {
        g_saved_swap_effect = pp->SwapEffect;
        g_saved_swap_effect_valid = true;
    }

    if (!g_enabled.load(std::memory_order_acquire)) {
        // MSAA off — write NONE and restore the engine's preferred swap
        // effect (only if we previously overrode it).
        pp->MultiSampleType    = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        if (g_saved_swap_effect_valid
            && pp->SwapEffect == D3DSWAPEFFECT_DISCARD
            && g_saved_swap_effect != D3DSWAPEFFECT_DISCARD) {
            pp->SwapEffect = g_saved_swap_effect;
        }
        g_actual_count.store(0, std::memory_order_release);
        g_actual_quality.store(0, std::memory_order_release);
        return;
    }

    const uint32_t desired = g_sample_count.load(std::memory_order_acquire);
    const D3DFORMAT bb_fmt = pp->BackBufferFormat != D3DFMT_UNKNOWN
                                 ? pp->BackBufferFormat
                                 : D3DFMT_X8R8G8B8;
    const D3DFORMAT depth_fmt = pp->AutoDepthStencilFormat != D3DFMT_UNKNOWN
                                    ? pp->AutoDepthStencilFormat
                                    : D3DFMT_D24S8;
    DWORD q = 0;
    const uint32_t actual = pick_supported(
        d3d, adapter, devtype,
        bb_fmt, depth_fmt, pp->Windowed, desired, &q);

    pp->MultiSampleType    = count_to_type(actual);
    pp->MultiSampleQuality = q;

    if (actual > 0 && pp->SwapEffect != D3DSWAPEFFECT_DISCARD) {
        // D3D9 spec: MSAA + swap effect != DISCARD = INVALIDCALL on
        // CreateDevice. Force DISCARD; restore on disable above.
        pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    }

    g_actual_count.store(actual, std::memory_order_release);
    g_actual_quality.store(static_cast<uint32_t>(q),
                           std::memory_order_release);
    g_apply_count.fetch_add(1, std::memory_order_relaxed);

    mtr::log::info("msaa: %s -> requested=%ux applied=%ux q=%u "
                   "(bb_fmt=%u depth_fmt=%u windowed=%d)",
                   phase, desired, actual, (unsigned)q,
                   (unsigned)bb_fmt, (unsigned)depth_fmt, pp->Windowed);
}

} // namespace

// ---- Public API -----------------------------------------------------------

bool enabled()                  { return g_enabled.load(); }
void set_enabled(bool v)        { g_enabled.store(v);
                                  mtr::log::info("msaa: enabled=%d", v ? 1 : 0); }

uint32_t sample_count()         { return g_sample_count.load(); }
void     set_sample_count(uint32_t n) {
    // Snap to a supported count. The cap check at apply time handles the
    // device-specific failure path; here we just keep the value sane.
    if (n != 0 && n != 2 && n != 4 && n != 8 && n != 16) {
        if      (n < 2)  n = 0;
        else if (n < 4)  n = 2;
        else if (n < 8)  n = 4;
        else if (n < 16) n = 8;
        else             n = 16;
    }
    g_sample_count.store(n);
    mtr::log::info("msaa: sample_count=%ux", n);
}

uint32_t actual_count()         { return g_actual_count.load(); }
uint32_t actual_quality()       { return g_actual_quality.load(); }

void apply_for_create_device(D3DPRESENT_PARAMETERS* pp,
                             IDirect3D9* d3d,
                             UINT adapter,
                             D3DDEVTYPE devtype) {
    g_create_overrides.fetch_add(1, std::memory_order_relaxed);
    apply_to_pp(pp, d3d, adapter, devtype, "CreateDevice");
}

void apply_for_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (!dev || !pp) return;
    g_reset_overrides.fetch_add(1, std::memory_order_relaxed);
    // Recover IDirect3D9 from the device so we can run cap checks on
    // Reset. Also need adapter + devtype; pull from creation parameters.
    IDirect3D9* d3d = nullptr;
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) {
        mtr::log::info("msaa: Reset GetDirect3D failed; skip cap check");
        return;
    }
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    UINT adapter = D3DADAPTER_DEFAULT;
    D3DDEVTYPE devtype = D3DDEVTYPE_HAL;
    if (SUCCEEDED(dev->GetCreationParameters(&cp))) {
        adapter = cp.AdapterOrdinal;
        devtype = cp.DeviceType;
    }
    apply_to_pp(pp, d3d, adapter, devtype, "Reset");
    d3d->Release();
}

} // namespace mtr::msaa
