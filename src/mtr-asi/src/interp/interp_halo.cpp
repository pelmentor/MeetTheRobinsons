// Halo follow-fix client (M3.3) — view-interp halo position correction.
//
// THE BUG
// =======
// With M3 view-interp ON, halos (the 2D sprite tags above followable
// entities) appear offset from their tagged entity, with the offset
// modulating per-frame as the camera rotates. Reported as the "M3-view-
// interp ghost-offset bug".
//
// THE CAUSE
// =========
// The HaloComponent's per-frame Update (sub_6678D0, vtable[+4] of vtable
// 0x6DD400) fires from engine_pump_alt at 0x68149A — BEFORE the sim tick
// and BEFORE render_frame_top_level. It walks both the static spec array
// (180-byte stride at HaloComponent+0x1C) and the dynamic linked list
// (head at HaloComponent+0x24) and projects each halo's world position
// to screen via sub_58B6F0 / sub_58B7D0. Those helpers transform the
// world-pos through the matrix at camera_struct+0x110 (the cached
// view-projection matrix).
//
// Our M3 view-interp lerps the GLOBAL view matrix at 0x724C10 in
// camera_apply_all_active POST — which fires LATER, inside render_
// frame_top_level. Result: halo update sees un-interp'd view-proj, but
// the rendered geometry uses the interp'd view → halos sit at where the
// world position would project under the un-interp'd view, while the
// entity model sits at where the same world position projects under the
// interp'd view → ghost offset that scales with the per-frame interp
// delta.
//
// THE FIX
// =======
// Hook sub_6678D0 PRE-orig. In the hook:
//   1. Bail out fast if view_interp not enabled / not throttling /
//      no two snapshots / cut detected.
//   2. Identify the camera_struct via sub_58DA10(world_ctx, 0).
//   3. Save camera_struct[+0x110] (16 floats = 64 bytes).
//   4. Compute V_interp from the M3 snapshots (slerp+lerp, same as
//      view_apply_interp_for_render_frame uses).
//   5. Compute VP_interp = V_interp * P, where P is the freshly-built
//      projection matrix at 0x745AA0 (engine writes it via
//      halo_stash_camera_proj_params at engine_pump_alt's halo block,
//      right before halo_component_update is dispatched). This matches
//      what the engine itself does each frame to fill camera_struct
//      [+0x110] under non-interp conditions, just with V_interp in
//      place of V_engine.
//   6. Write VP_interp into camera_struct[+0x110].
//   7. Call orig (which projects halos with the interp'd VP).
//   8. Restore camera_struct[+0x110].
//
// IMPORTANT: an earlier draft used VP_interp = V_interp * V_curr_inv *
// VP_curr_saved, but V_curr from the M3 snapshot is stale relative to
// the V the engine used to build VP_curr (snapshots refresh on sim
// ticks; VP_curr refreshes on every camera_apply). Stale V_curr meant
// V_curr_inv * VP_curr ≠ P, which corrupted the override and made
// halos fail the frustum cull entirely (vanishing instead of merely
// offset). Switching to V_interp * P_fresh sidesteps the staleness
// because both inputs come from this frame's known-good state.
//
// Camera_struct fields at +0x150 (audio listener vec3) and +0x168 (view
// forward vec3) are NOT overridden — the audio attenuation is sub-frame
// and the backface cull edge cases are below user-perceptible noise.
//
// Default OFF — user opts in via UI alongside view_interp. Auto-vetoed
// when sim_decouple is in mini-game mode (no view interp there either).
//
// Hook target VAs:
//   sub_6678D0 = 0x6678D0      HaloComponent::Update (vtable[+4])
//   sub_58DA10 = 0x58DA10      world_ctx::get_camera_struct(world_ctx, scene)
//   dword_741648 = 0x741648    world ctx ptr (read at runtime)
//
// All addresses verified against Wilbur.exe build at imagebase 0x400000.

#include "interp_internal.h"
#include "mtr/interp.h"
#include "mtr/sim_decouple.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::interp {

namespace {

// === Engine VAs ============================================================

constexpr uintptr_t kHaloUpdateVA       = 0x006678D0;  // HaloComponent::Update
constexpr uintptr_t kGetCameraStructVA  = 0x0058DA10;  // world_ctx::get_camera_struct
constexpr uintptr_t kWorldCtxPtrVA      = 0x00741648;  // dword_741648 — world ctx
constexpr uintptr_t kCameraStructVPOffset = 0x110;     // view-proj matrix offset
constexpr uintptr_t kHaloProjMatrixVA   = 0x00745AA0;  // P matrix (engine rewrites it each frame
                                                       //  in halo_stash_camera_proj_params,
                                                       //  immediately before halo Update fires)

// __thiscall on x86 MSVC: this in ECX, args on stack.
using PFN_HaloUpdate      = int (__thiscall*)(void* this_, int scene);
using PFN_GetCameraStruct = void* (__thiscall*)(void* world_ctx, int scene);

PFN_HaloUpdate g_orig_halo_update = nullptr;

// === State =================================================================

std::atomic<bool>     g_halo_interp_enabled{false};
std::atomic<uint64_t> g_halo_interp_writes{0};
std::atomic<uint64_t> g_halo_interp_skips{0};

// === Math helpers ==========================================================
// Row-major 4x4 (D3DMATRIX). v' = v * M.

void mat4_mul(const float* A, const float* B, float* out) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) {
                s += A[i*4+k] * B[k*4+j];
            }
            out[i*4+j] = s;
        }
    }
}

// Resolve the camera_struct used by the halo update path. Replicates
// engine_pump_alt's call: sub_58DA10(world_ctx, 0) where world_ctx =
// *dword_741648. Falls back to nullptr on null world ctx (pre-init).
void* resolve_camera_struct() {
    void* world_ctx = *reinterpret_cast<void**>(kWorldCtxPtrVA);
    if (!world_ctx) return nullptr;
    auto fn = reinterpret_cast<PFN_GetCameraStruct>(kGetCameraStructVA);
    return fn(world_ctx, 0);
}

// === Hook ==================================================================

int __fastcall hk_halo_update(void* this_, void* /*edx*/, int scene) {
    // Fast path: feature off.
    if (!g_halo_interp_enabled.load(std::memory_order_relaxed)) {
        return g_orig_halo_update(this_, scene);
    }
    // Same gating as view_apply_interp_for_render_frame so the halo
    // override only fires when the view itself is being interp'd.
    if (!view_interp_enabled())                        goto fast;
    if (!has_two_snapshots())                          goto fast;
    if (is_cut_detected())                             goto fast;
    if (!mtr::sim_decouple::effective_is_throttling()) goto fast;

    {
        void* cam = resolve_camera_struct();
        if (!cam) goto fast;

        auto* vp_slot = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(cam) + kCameraStructVPOffset);

        const Snapshot& p = prev();
        const Snapshot& c = curr();

        // Build V_interp (matches view_apply_interp_for_render_frame's
        // choice — camera-world-space when toggled, direct lerp+slerp
        // otherwise).
        float v_interp[16];
        const float alpha = detail::effective_alpha();
        if (view_interp_camspace()) {
            detail::compose_view_interp_camspace(p.view, c.view, alpha, v_interp);
        } else {
            detail::compose_interp_matrix(p.view, c.view, alpha, v_interp);
        }

        // Read P from 0x745AA0. The engine just rewrote it via halo_stash
        // _camera_proj_params at the start of engine_pump_alt's halo block
        // (right before scene_component_list_dispatch_update walks
        // halo_component_update), so it's guaranteed current.
        float p_curr[16];
        std::memcpy(p_curr, reinterpret_cast<const void*>(kHaloProjMatrixVA),
                    sizeof(p_curr));

        // VP_interp = V_interp * P (row-major D3D, v_clip = v_world * V * P).
        float vp_interp[16];
        mat4_mul(v_interp, p_curr, vp_interp);

        // Save VP_curr, swap in VP_interp, run orig, restore.
        float vp_saved[16];

        // SEH-wrap the actual write/read of camera memory: the camera
        // can be torn down during level transitions and we'd rather
        // skip a halo update than crash.
        __try {
            std::memcpy(vp_saved, vp_slot, sizeof(vp_saved));
            std::memcpy(vp_slot, vp_interp, sizeof(vp_interp));
            int rc = g_orig_halo_update(this_, scene);
            std::memcpy(vp_slot, vp_saved, sizeof(vp_saved));
            g_halo_interp_writes.fetch_add(1, std::memory_order_relaxed);
            return rc;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Best-effort restore so subsequent frames have a clean VP.
            __try {
                std::memcpy(vp_slot, vp_saved, sizeof(vp_saved));
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            g_halo_interp_skips.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }

fast:
    g_halo_interp_skips.fetch_add(1, std::memory_order_relaxed);
    return g_orig_halo_update(this_, scene);
}

} // namespace

namespace detail {

void halo_install() {
    void* va = reinterpret_cast<void*>(kHaloUpdateVA);
    if (MH_CreateHook(va, reinterpret_cast<void*>(&hk_halo_update),
                      reinterpret_cast<void**>(&g_orig_halo_update)) != MH_OK) {
        mtr::log::info("interp_halo: MH_CreateHook(halo_update @%p) failed", va);
        return;
    }
    if (MH_EnableHook(va) != MH_OK) {
        mtr::log::info("interp_halo: MH_EnableHook(halo_update) failed");
        return;
    }
    mtr::log::info("interp_halo: hooked HaloComponent::Update at %p", va);
}

} // namespace detail

// === Public API ============================================================

bool halo_interp_enabled() { return g_halo_interp_enabled.load(std::memory_order_relaxed); }

void set_halo_interp_enabled(bool on) {
    const bool prev_v = g_halo_interp_enabled.exchange(on, std::memory_order_relaxed);
    if (prev_v != on) {
        mtr::log::info("interp: halo_interp %s", on ? "enabled" : "disabled");
    }
}

uint64_t halo_interp_writes() { return g_halo_interp_writes.load(std::memory_order_relaxed); }
uint64_t halo_interp_skips()  { return g_halo_interp_skips.load(std::memory_order_relaxed); }

} // namespace mtr::interp
