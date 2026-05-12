// Engine-side camera hooks for d3d9_hook.
//
//   per_camera_apply (0x4C1BA0)  PRE — single funnel for every camera class
//                                       (PathCam/ScriptCam/Stationary/death).
//                                       Overwrites *(outer+0x34) with freecam
//                                       view, writes draw-distance overrides
//                                       to the cached frustum buffer at
//                                       outer+0xD4, and disables side-cull
//                                       planes when the toggle's on.
//   build_proj_matrix (0x562B20) — substitute aspect (main camera only) +
//                                   FOV when override active.
//   build_frustum    (0x4DF2C0) — substitute the far plane + far corners.
//   camera_compute   (0x564600) — invalidate cached aspect every frame so
//                                   live-aspect changes propagate.
//   game_camera_tick (0x58C910) — capture PathCam controller for
//                                   MMB-teleport plumbing.
//   vis_test         (0x4E0B90) — diagnostic force-pass when draw_dist
//                                   override active. Per-object visibility.

#include "d3d9_internal.h"

#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::aspect    { float current(); }
namespace mtr::fov       { bool  has_override(); float current(); }
namespace mtr::draw_dist { bool  has_override(); float current(); }
namespace mtr::scene     {
    bool fog_disabled();
    bool side_cull_disabled();
    void enforce_fog_disabled();
}
namespace mtr::freecam {
    bool active();
    bool build_view_matrix(D3DMATRIX* out);
    void on_engine_view(const D3DMATRIX& vm);
    void set_last_controller(void* c);
}

namespace mtr::d3d9hook {

namespace {

using namespace detail;

PFN_BuildProjMatrix g_orig_BuildProjMatrix = nullptr;
PFN_CameraCompute   g_orig_CameraCompute   = nullptr;
PFN_GameCamTick     g_orig_GameCameraTick  = nullptr;
PFN_PerCameraApply  g_orig_PerCameraApply  = nullptr;
PFN_BuildFrustum    g_orig_BuildFrustum    = nullptr;
PFN_VisTest         g_orig_VisTest         = nullptr;

int __cdecl hk_BuildProjMatrix(float fov_deg, float aspect, float near_, float far_) {
    // Skip RT-probe / overlay-quad projections (square aspect ~ 1.0): those
    // should not get the user's wide aspect override or FOV change. Everything
    // else is treated as a world-camera projection.
    //
    // A narrow-FOV "UI / HUD perspective" hypothesis was investigated and
    // ruled out (verified by runtime logs 2026-05-06): the game's HUD does
    // NOT go through this builder; it goes through the sprite batcher
    // (sub_4E8D30). Override of the HUD/menu aspect lives in
    // `mtr::sprite_matrix` + `mtr::ui_aspect_rules`, not here.
    const bool is_main = (aspect < 0.99f || aspect > 1.01f);

    if (is_main) {
        const float target = mtr::aspect::current();
        if (target > 0.1f && target < 10.0f) aspect = target;

        // FOV override — engine reads defproj.fov once at camera-init and
        // caches it in per-camera projection state. Console writes don't
        // propagate. Apply here at the projection-build site so the override
        // is live every frame.
        if (mtr::fov::has_override()) {
            fov_deg = mtr::fov::current();
        }
    }
    return g_orig_BuildProjMatrix(fov_deg, aspect, near_, far_);
}

int __fastcall hk_GameCameraTick(int this_, int /*edx*/) {
    // Run the engine's PathCam tick. We no longer override here — override
    // is downstream in camera_apply_all_active so it works across every
    // camera class. Just snapshot the controller for MMB-teleport plumbing.
    int rc = g_orig_GameCameraTick(this_, 0);
    if (this_) {
        mtr::freecam::set_last_controller(reinterpret_cast<void*>(this_));
    }
    return rc;
}

// PRE-hook for sub_4C1BA0 (per-camera apply). Overwrite the view matrix at
// *(outer+0x34) before the orig copies it to global 0x724C10 + sends to D3D.
// Also: when draw_dist override is on, write directly to the engine's
// CACHED frustum buffer (outer+0xD4) — the engine builds the frustum at
// scene init and never rebuilds it per frame, so hooking build_view_frustum
// alone never fires after startup. This is a data-level override on the
// engine's own buffer (not a code patch).
//
// Filter outer+0x308 (state) so we only override the main camera, not RT/
// shadow/probe passes (which use the same apply func with state==1).
int __fastcall hk_PerCameraApply(void* this_, int /*edx*/, int skip_inverse) {
    if (this_) {
        const uint32_t state = *reinterpret_cast<const uint32_t*>(
            static_cast<char*>(this_) + 0x308);
        const bool main_cam = (state == 0);

        if (main_cam && mtr::freecam::active()) {
            D3DMATRIX** view_ptr_slot = reinterpret_cast<D3DMATRIX**>(
                static_cast<char*>(this_) + 0x34);
            D3DMATRIX* view_dst = *view_ptr_slot;
            if (view_dst) {
                mtr::freecam::on_engine_view(*view_dst);
                D3DMATRIX our_view;
                if (mtr::freecam::build_view_matrix(&our_view)) {
                    std::memcpy(view_dst, &our_view, sizeof(D3DMATRIX));
                }
            }
        }

        if (main_cam && mtr::draw_dist::has_override()) {
            const float far_user = mtr::draw_dist::current();
            char* outer = static_cast<char*>(this_);

            // Per-camera FAR field at projection-cache+4 (= outer+0x44).
            *reinterpret_cast<float*>(outer + 0x44) = far_user;

            // Cached frustum buffer at outer+0xD4 (= projection-cache+0x94).
            // Layout (from build_view_frustum decompile):
            //   +0..15  : near plane
            //   +16..31 : far plane (n=0,0,-1, w=-far)
            //   +32..47 : top plane
            //   +48..63 : bottom plane
            //   +64..79 : left plane
            //   +80..95 : right plane
            //   +352..363 : far corner C0 (s_h*-far, s_v*-far, -far)
            //   +364..375 : far corner C1
            //   +376..387 : far corner C2
            //   +388..399 : far corner C3
            float* fb = reinterpret_cast<float*>(outer + 0xD4);

            // Far plane equation: (0, 0, -1, -far) — view-space, RH
            fb[4] = 0.0f;
            fb[5] = 0.0f;
            fb[6] = -1.0f;
            fb[7] = -far_user;

            // Recompute far corners from current fov / aspect.
            const float fov_deg = *reinterpret_cast<float*>(outer + 0x48);
            const float aspect  = *reinterpret_cast<float*>(outer + 0x4C);
            const float fov_v_half = 0.017453292f * fov_deg * 0.5f;
            const float s_v = std::tan(fov_v_half);
            const float s_h = s_v * aspect;
            const float fz = -far_user;
            const float fx = -s_h * far_user;
            const float fy = -s_v * far_user;

            float* fc = reinterpret_cast<float*>(outer + 0xD4 + 352);
            fc[0]  = fx;   fc[1]  = fy;   fc[2]  = fz;     // C0
            fc[3]  = -fx;  fc[4]  = fy;   fc[5]  = fz;     // C1
            fc[6]  = -fx;  fc[7]  = -fy;  fc[8]  = fz;     // C2
            fc[9]  = fx;   fc[10] = -fy;  fc[11] = fz;     // C3
        }

        // Side-plane cull disable. Setting top/bottom/left/right planes
        // (offsets +32 / +48 / +64 / +80) to (0,0,0,1) means
        // dot(plane.xyz, p) + plane.w = 1 > 0 for every point → always pass.
        if (main_cam && mtr::scene::side_cull_disabled()) {
            float* fb = reinterpret_cast<float*>(static_cast<char*>(this_) + 0xD4);
            // top    (+32 = float index 8)
            fb[8]  = 0.0f; fb[9]  = 0.0f; fb[10] = 0.0f; fb[11] = 1.0f;
            // bottom (+48 = float index 12)
            fb[12] = 0.0f; fb[13] = 0.0f; fb[14] = 0.0f; fb[15] = 1.0f;
            // left   (+64 = float index 16)
            fb[16] = 0.0f; fb[17] = 0.0f; fb[18] = 0.0f; fb[19] = 1.0f;
            // right  (+80 = float index 20)
            fb[20] = 0.0f; fb[21] = 0.0f; fb[22] = 0.0f; fb[23] = 1.0f;
        }

        // Pump the fog cvar each frame so it stays at 0 if the user wants
        // fog disabled and the engine writes back to it.
        mtr::scene::enforce_fog_disabled();
    }
    return g_orig_PerCameraApply(this_, 0, skip_inverse);
}

// __thiscall: ECX = this. MSVC maps __thiscall to __fastcall (this in ECX,
// edx unused for this signature) for x86 hooking purposes.
int __fastcall hk_CameraCompute(int this_, int /*edx_unused*/) {
    if (this_) {
        const float target = mtr::aspect::current();
        if (target > 0.1f && target < 10.0f) {
            float* aspect_field = reinterpret_cast<float*>(this_ + 12);
            BYTE*  dirty_flag   = reinterpret_cast<BYTE*>(this_ + 112);
            if (*aspect_field != target) {
                *aspect_field = target;
                *dirty_flag   = 1;  // force original to rebuild matrix this call
            }
        }
    }
    return g_orig_CameraCompute(this_, 0);
}

int __fastcall hk_VisTest(void* this_, int /*edx*/, int a2, int a3,
                           int a4, int a5, int a6, int a7) {
    static std::atomic<int> g_vis_seen{0};

    int orig = g_orig_VisTest(this_, 0, a2, a3, a4, a5, a6, a7);

    int rc = orig;
    if (mtr::draw_dist::has_override()) {
        // Force-pass when override is active. Renders entire object set.
        rc = 1;
    }

    int n = g_vis_seen.fetch_add(1);
    if (n < 8) {
        mtr::log::info("vistest #%d: this=%p a2=%p a3=%p flags=%08X  orig=%d  out=%d",
                       n, this_,
                       reinterpret_cast<void*>(a2), reinterpret_cast<void*>(a3),
                       static_cast<uint32_t>(a6), orig, rc);
    }
    return rc;
}

int __cdecl hk_BuildFrustum(int out_buf, float near_, float far_,
                             char has_clip, int extra_axis,
                             float extra_d, float fov_deg, float aspect) {
    static std::atomic<int> g_frustum_seen{0};
    int n = g_frustum_seen.fetch_add(1);
    if (n < 5) {
        mtr::log::info("build_frustum #%d: near=%.3f far=%.1f fov=%.2f aspect=%.4f "
                       "has_clip=%d  out_buf=%p",
                       n, near_, far_, fov_deg, aspect, (int)has_clip,
                       reinterpret_cast<void*>(out_buf));
    }

    const bool dd_on  = mtr::draw_dist::has_override();
    const float far_user = dd_on ? mtr::draw_dist::current() : far_;
    if (dd_on) far_ = far_user;

    int rc = g_orig_BuildFrustum(out_buf, near_, far_, has_clip, extra_axis,
                                  extra_d, fov_deg, aspect);

    // POST-orig: rewrite the far-corner rectangle. The orig hardcodes the
    // far corners at view-space z=-10000 (offsets +352..+396) using
    // tan(fov_h/2) and tan(fov_v/2). If culling tests against these
    // corners (e.g. cone bounds) instead of the plane, our `far_` arg
    // override has zero effect. Recompute corners with our value too.
    if (dd_on && out_buf) {
        const float fov_v_half = 0.017453292f * fov_deg * 0.5f;
        const float s_v = std::tan(fov_v_half);
        const float s_h = s_v * aspect;
        const float fz = -far_user;
        const float fx = s_h * (-far_user);
        const float fy = s_v * (-far_user);

        float* fc = reinterpret_cast<float*>(out_buf + 352);
        fc[0] = fx;        fc[1] = fy;        fc[2] = fz;     // C0
        fc[3] = -fx;       fc[4] = fy;        fc[5] = fz;     // C1
        fc[6] = -fx;       fc[7] = -fy;       fc[8] = fz;     // C2
        fc[9] = fx;        fc[10] = -fy;      fc[11] = fz;    // C3
    }
    return rc;
}

bool install_one(const char* tag, void* va, void* hk, void** orig_pp) {
    if (MH_CreateHook(va, hk, orig_pp) != MH_OK) {
        mtr::log::info("d3d9: MH_CreateHook(%s @%p) failed", tag, va);
        return false;
    }
    if (MH_EnableHook(va) != MH_OK) {
        mtr::log::info("d3d9: MH_EnableHook(%s) failed", tag);
        return false;
    }
    mtr::log::info("d3d9: hooked %s at %p", tag, va);
    return true;
}

} // namespace

namespace detail {

void install_camera_hooks() {
    install_one("build_proj_matrix",
                reinterpret_cast<void*>(kBuildProjMatrixVA),
                reinterpret_cast<void*>(&hk_BuildProjMatrix),
                reinterpret_cast<void**>(&g_orig_BuildProjMatrix));
    install_one("camera_compute",
                reinterpret_cast<void*>(kCameraComputeVA),
                reinterpret_cast<void*>(&hk_CameraCompute),
                reinterpret_cast<void**>(&g_orig_CameraCompute));
    install_one("gameplay_camera_tick",
                reinterpret_cast<void*>(kGameCameraTickVA),
                reinterpret_cast<void*>(&hk_GameCameraTick),
                reinterpret_cast<void**>(&g_orig_GameCameraTick));
    install_one("per_camera_apply",
                reinterpret_cast<void*>(kPerCameraApplyVA),
                reinterpret_cast<void*>(&hk_PerCameraApply),
                reinterpret_cast<void**>(&g_orig_PerCameraApply));
    install_one("build_view_frustum",
                reinterpret_cast<void*>(kBuildFrustumVA),
                reinterpret_cast<void*>(&hk_BuildFrustum),
                reinterpret_cast<void**>(&g_orig_BuildFrustum));

    // [historical note] camera_apply_state PRE-hook + vis_test thunk hook
    // both caused save-load instability and were removed. apply_state was
    // rebuilding nothing (build_frustum still only fired 5 times at init);
    // vis_test (sub_4E0B90) is a 1-instruction stolen-byte IAT thunk that
    // returns a structure pointer (low byte used as bool) — hooking it via
    // MinHook left a fragile trampoline that crashed during save loads
    // even when the override was OFF.
    (void)g_orig_VisTest;  // suppress unused warning; declaration kept for the future
}

} // namespace detail

} // namespace mtr::d3d9hook
