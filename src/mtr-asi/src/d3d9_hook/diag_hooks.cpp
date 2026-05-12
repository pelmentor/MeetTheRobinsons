// Diagnostic D3D9 hooks: SetClipPlane + SetRenderState.
//
//   IDirect3DDevice9::SetClipPlane (vtable[55]) — diagnostic. Logs first
//                                                  30 user clip plane
//                                                  writes to verify
//                                                  whether the engine
//                                                  uses D3D9 user clip
//                                                  planes for distance
//                                                  culling (a candidate
//                                                  for the "global plane
//                                                  clip" the user
//                                                  describes).
//   IDirect3DDevice9::SetRenderState (vtable[57]) — filters
//                                                   D3DRS_CLIPPLANEENABLE
//                                                   transitions for the
//                                                   same diagnostic, and
//                                                   forces D3DRS_FOGENABLE
//                                                   to 0 when the
//                                                   `mtr::scene::fog_disabled()`
//                                                   toggle is on.

#include "d3d9_internal.h"

#include <MinHook.h>
#include <atomic>
#include <intrin.h>

namespace mtr::log { void info(const char* fmt, ...); }
namespace mtr::scene {
    bool fog_disabled();
    bool no_backface_cull();
}

namespace mtr::d3d9hook {

namespace {

using namespace detail;

PFN_SetClipPlane   g_orig_SetClipPlane   = nullptr;
PFN_SetRenderState g_orig_SetRenderState = nullptr;

HRESULT STDMETHODCALLTYPE hk_SetClipPlane(IDirect3DDevice9* dev, DWORD index, const float* plane) {
    static std::atomic<int> g_seen{0};
    int n = g_seen.fetch_add(1);
    if (n < 30 && plane) {
        mtr::log::info("SetClipPlane #%d: idx=%lu  plane=(%.4f, %.4f, %.4f, %.4f)  caller=%p",
                       n, index, plane[0], plane[1], plane[2], plane[3], _ReturnAddress());
    }
    return g_orig_SetClipPlane(dev, index, plane);
}

HRESULT STDMETHODCALLTYPE hk_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD value) {
    if (state == D3DRS_CLIPPLANEENABLE) {
        static std::atomic<int> g_seen{0};
        static std::atomic<DWORD> g_last{0};
        if (g_last.exchange(value) != value && g_seen.fetch_add(1) < 30) {
            mtr::log::info("SetRenderState[CLIPPLANEENABLE] = 0x%lX (mask of active clip planes)  caller=%p",
                           value, _ReturnAddress());
        }
    }
    // D3D-level fog disable. Belt-and-braces: writing 0 to the engine's
    // fogEnabled cvar (mtr::scene::set_fog_disabled) handles the "engine
    // reads cvar -> calls SetRenderState(FOGENABLE)" path; this catches
    // any direct SetRenderState calls that aren't gated by the cvar.
    if (state == D3DRS_FOGENABLE && mtr::scene::fog_disabled()) {
        value = 0;
    }
    // Force-render-both-faces toggle. D3DCULL_NONE = 1 disables D3D's
    // backface culling entirely, so geometry that's wound away from the
    // camera (back of a box, inside of a hollow shell, single-sided
    // banners viewed from behind) renders too. Note: this is a pure
    // D3D-level state filter; it doesn't affect engine-side per-object
    // frustum culling at g_cull_frustum.
    if (state == D3DRS_CULLMODE && mtr::scene::no_backface_cull()) {
        value = D3DCULL_NONE;
    }
    return g_orig_SetRenderState(dev, state, value);
}

} // namespace

namespace detail {

void install_diag_hooks(void* p_SetClipPlane, void* p_SetRenderState) {
    if (MH_CreateHook(p_SetClipPlane, &hk_SetClipPlane,
                      reinterpret_cast<void**>(&g_orig_SetClipPlane)) == MH_OK &&
        MH_EnableHook(p_SetClipPlane) == MH_OK) {
        mtr::log::info("d3d9: hooked SetClipPlane");
    } else {
        mtr::log::info("d3d9: SetClipPlane hook failed");
    }
    if (MH_CreateHook(p_SetRenderState, &hk_SetRenderState,
                      reinterpret_cast<void**>(&g_orig_SetRenderState)) == MH_OK &&
        MH_EnableHook(p_SetRenderState) == MH_OK) {
        mtr::log::info("d3d9: hooked SetRenderState (CLIPPLANEENABLE filter + fog disable)");
    } else {
        mtr::log::info("d3d9: SetRenderState hook failed");
    }
}

} // namespace detail

} // namespace mtr::d3d9hook
