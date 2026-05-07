#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <intrin.h>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }
namespace mtr::menu {
    void on_end_scene(IDirect3DDevice9* dev);
    void on_reset_pre(IDirect3DDevice9* dev);
    void on_reset_post(IDirect3DDevice9* dev);
}
namespace mtr::aspect    { float current(); }
namespace mtr::screen_push     { bool current_top_name(char* out, size_t out_size); }
namespace mtr::ui_aspect_rules { float resolve_aspect(const char* top_name); }
namespace mtr::vis_test_probe  { void frame_tick(); }
namespace mtr::scene_vis_log   { void frame_tick(); }
namespace mtr::fps_limit       { void tick(); }
namespace mtr::sim_decouple    { void on_render_frame(); }
namespace mtr::sprite_matrix {
    bool  enabled();
    bool  auto_from_rules();
    float mul_a_a1();
    float mul_a_a2();
    float mul_a_a3();
    float mul_b_a1();
    float mul_b_a2();
    float mul_b_a3();
    float pass_override_factor();
    float pos_offset_x();
    float pos_offset_y();
}
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

using PFN_CreateDevice = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                      D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PFN_EndScene     = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using PFN_Reset        = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PFN_SetTransform = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using PFN_WrapSetTransform      = void (__cdecl*)(const D3DMATRIX*);
using PFN_WrapSetTransformState = void (__cdecl*)();
using PFN_BuildProjMatrix  = int  (__cdecl*)(float fov_deg, float aspect, float near_, float far_);
using PFN_CameraCompute    = int  (__fastcall*)(int this_, int /*edx*/);
using PFN_GameCamTick      = int  (__fastcall*)(int this_, int /*edx*/);
using PFN_PerCameraApply   = int  (__fastcall*)(void* this_, int /*edx*/, int skip_inverse);
using PFN_BuildFrustum     = int  (__cdecl*)(int out_buf, float near_, float far_,
                                              char has_clip, int extra_axis,
                                              float extra_d, float fov_deg, float aspect);
using PFN_CameraApplyState = char (__fastcall*)(int this_, int /*edx*/);
using PFN_VisTest          = int  (__fastcall*)(void* this_, int /*edx*/, int a2, int a3,
                                                 int a4, int a5, int a6, int a7);
using PFN_SetClipPlane     = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const float*);
using PFN_SetRenderState   = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using PFN_BuildOrtho       = int  (__cdecl*)(float l, float r, float t, float b, float n, float f);
using PFN_MatrixSet3       = int  (__cdecl*)(int a1, int a2, int a3);

PFN_CreateDevice     g_orig_CreateDevice     = nullptr;
PFN_EndScene         g_orig_EndScene         = nullptr;
PFN_Reset            g_orig_Reset            = nullptr;
PFN_SetTransform     g_orig_SetTransform     = nullptr;
PFN_WrapSetTransform      g_orig_WrapSetTransform      = nullptr;
PFN_WrapSetTransformState g_orig_WrapSetTransformState = nullptr;
PFN_BuildProjMatrix  g_orig_BuildProjMatrix  = nullptr;
PFN_CameraCompute    g_orig_CameraCompute    = nullptr;
PFN_GameCamTick      g_orig_GameCameraTick   = nullptr;
PFN_PerCameraApply   g_orig_PerCameraApply   = nullptr;
PFN_BuildFrustum     g_orig_BuildFrustum     = nullptr;
PFN_CameraApplyState g_orig_CameraApplyState = nullptr;
PFN_VisTest          g_orig_VisTest          = nullptr;
PFN_SetClipPlane     g_orig_SetClipPlane     = nullptr;
PFN_SetRenderState   g_orig_SetRenderState   = nullptr;
PFN_BuildOrtho       g_orig_BuildOrtho       = nullptr;
PFN_MatrixSet3       g_orig_MatrixSetXformA  = nullptr;
PFN_MatrixSet3       g_orig_MatrixSetXformB  = nullptr;

std::atomic<int>  g_last_pp_width{0};
std::atomic<int>  g_last_pp_height{0};

// Track every distinct (m00, m11, caller) projection seen so we don't spam
// the log but DO catch all unique aspects with the function that produced
// each one.
constexpr int kMaxSeenProj = 32;
struct SeenProj { float m00, m11; void* caller; };
SeenProj g_seen[kMaxSeenProj]{};
std::atomic<int> g_seen_count{0};
std::mutex g_seen_mu;

// Fixed VAs of the engine's two SetTransform wrappers. Both eventually call
// IDirect3DDevice::SetTransform via vtable[148], differing in where the
// source matrix comes from:
//   wrap_SetTransform       (0x5625E0): caller passes a matrix pointer arg
//   wrap_SetTransform_state (0x5625C0): no arg — reads from a fixed global
//                                       matrix buffer (kStateMatrixBufferVA).
// render_sprite_batcher uses wrap_SetTransform_state, so we MUST hook that
// one to see UI matrices. The first wrapper is used by the 3D camera path.
constexpr uintptr_t kWrapSetTransformVA      = 0x005625E0;
constexpr uintptr_t kWrapSetTransformStateVA = 0x005625C0;
constexpr uintptr_t kStateGlobalVA           = 0x006FBD58;
constexpr uintptr_t kStateMatrixBufferVA     = 0x00729E30;  // 64-byte matrix used by wrap_SetTransform_state
// Game's projection-matrix builder: sub_562B20(fov_deg, aspect, near, far).
// Internally converts FOV to radians and builds a perspective matrix. We
// substitute the aspect parameter — the function's contract takes aspect as
// input, so this is param substitution, not output mangling.
constexpr uintptr_t kBuildProjMatrixVA = 0x00562B20;

// Ortho-projection builder: sub_562B70(l, r, t, b, n, f). Internally calls
// sub_63CB8B (the actual matrix construction: m00=2/(r-l), m05=2/(b-t), etc.)
// then routes the matrix through the engine's transform-state vtable. Only
// two callers in the binary:
//   - sub_4A9CE0 -> (0, 1, 0, 1, -0.01, 1.0)  : the 2D HUD/menu render pass
//     (called every frame from render_frame_top_level, drives the layered
//      overlay queue -- HUD, menus, screens, fonts).
//   - sub_563300 -> (0, 1, 1, 0, -1, 1)       : Y-inverted screen-fill quad
//      (post-process / screen-tear-off, gated by dword_6FBD2C).
// The HUD ortho is the right injection point for "UI aspect" override --
// modifying L/R bounds pillarboxes the UI inside the wider viewport without
// touching the world camera or relying on FOV-pattern matching.
constexpr uintptr_t kBuildOrthoVA       = 0x00562B70;

// Camera-context "recompute projection if dirty" function: sub_564600.
//   if (*(this+112)) build_proj_matrix(*(this+8) /*fov*/, *(this+12) /*aspect*/,
//                                      *this /*near*/, *(this+4) /*far*/);
// We override *(this+12) with our target aspect and re-arm the dirty flag so
// every call rebuilds with the right aspect — this is the cache-invalidation
// half of live aspect updates.
constexpr uintptr_t kCameraComputeVA   = 0x00564600;

// Gameplay-camera-tick. Slot[1] of the PathCam vtable @ 0x6C9AE0 (and the
// deathcam class via wrapper sub_533C10). We hook this purely to capture the
// PathCam controller pointer for MMB-teleport later -- the actual freecam
// view-matrix override has been moved downstream to camera_apply_all_active
// (see kCameraApplyAllVA below) so it works across PathCam/ScriptCam/
// StationaryCam/deathcam transitions.
constexpr uintptr_t kGameCameraTickVA  = 0x0058C910;

// View-frustum builder: __cdecl(out_buf, near, far, has_clip, axis, d, fov, aspect).
// Called by game_camera_apply_state (sub_564650) when the per-camera frustum
// dirty flag (*(camera+144)) is set. The far parameter (a3) is what bakes
// the frustum's far plane -- the TRUE draw-distance gate (objects past it
// get culled before rasterisation, regardless of the projection-matrix far).
// We override `far_` here when the user has a draw_dist override.
// Note: needs the dirty flag set every frame for the override to apply
// continuously; hk_CameraCompute below dirties +144 alongside +112 when
// our override is active.
constexpr uintptr_t kBuildFrustumVA    = 0x004DF2C0;

// game_camera_apply_state(camera_proj_cache) -- called per camera per frame.
// Reads its own frustum-dirty flag at +144; if set, calls build_view_frustum
// then clears the flag. We dirty +144 PRE-orig when our draw_dist override
// is active so each frame rebuilds the frustum. (hk_CameraCompute below
// runs AFTER apply_state's dirty check, so dirtying there only takes effect
// the NEXT frame at best -- this PRE-hook is the right place.)
constexpr uintptr_t kCameraApplyStateVA = 0x00564650;

// Per-object visibility test. SecuROM thunk: jmp [byte_F5F876+340BE].
// Called from the main render loop (sub_4C3790 @ 0x4C385D) per object;
// returns nonzero if object passes visibility (frustum/distance/etc).
// Hooking this is the only way to actually extend "draw distance" in this
// engine -- frustum-far overrides confirmed inert via diagnostic logs.
// We force-return TRUE when draw_dist override is active; performance hit
// is acceptable for the diagnostic value.
constexpr uintptr_t kVisTestVA          = 0x004E0B90;

// Sprite-batcher matrix builders. `render_sprite_batcher` (sub_4E8D30) is
// the per-frame 2D / sprite render pass — called once from
// render_frame_top_level at 0x4D23BF, AFTER all 3D passes. Each frame it
// pushes its own projection + view via these two helpers, which build a
// 4x4 matrix via runtime function pointers (dword_715B64 / dword_715B48)
// and route the matrix through the engine wrapper's vtable slot 39
// (dword_72E67C[156]) — the SAME slot build_ortho_matrix uses, so the
// matrix lands in the same downstream pipeline as our existing
// hk_BuildOrtho. But these helpers BYPASS sub_562B70 (the ortho builder
// we hook), which is why hk_BuildOrtho never fires on the actual HUD.
//
// Static RE confirms only ONE caller per helper (the sprite batcher),
// and the constants passed are FIXED:
//   matrix_set_via_xform_a(2.0f, -2.0f, 1.0f)  // projection-style
//   matrix_set_via_xform_b(-2.0f, -2.0f, 0.0f) // view-style
//
// These hooks are DIAGNOSTIC ONLY for now: log first N calls per frame
// (capped to avoid log spam) so we can confirm the path is live and see
// the call cadence. When user runtime data confirms HUD goes through
// here, we'll add aspect-substitution at this site.
constexpr uintptr_t kMatrixSetXformAVA = 0x00562AA0;
constexpr uintptr_t kMatrixSetXformBVA = 0x00562AE0;

// Per-camera apply: __thiscall(outer_cam, skip_inverse). Reads the view
// matrix pointed to by *(outer+0x34), copies it into D3D global 0x724C10,
// computes world via inverse and copies into 0x724C50. THIS is the single
// funnel every camera class (PathCam/ScriptCam/StationaryCam/deathcam)
// goes through during camera_apply_all_active. Hooking PRE and overwriting
// *(outer+0x34) before the orig copies it = our freecam pose ends up in
// the globals AND is what the engine subsequently sends to D3D, regardless
// of which camera class is "active" this frame. Filter on outer+0x308==0
// to avoid stomping render-target / probe cameras.
//
// Prologue is `sub esp,0Ch / push esi / mov esi,ecx / jmp [F8788C]` (a
// SecuROM thunk to the real implementation). 6 bytes before the jmp, more
// than the 5 MinHook needs.
constexpr uintptr_t kPerCameraApplyVA  = 0x004C1BA0;

void log_pp(const D3DPRESENT_PARAMETERS* pp, const char* tag) {
    if (!pp) return;
    mtr::log::info("[%s] pp: Windowed=%d %ux%u fmt=%u SwapEffect=%u hWnd=%p",
                   tag, pp->Windowed, pp->BackBufferWidth, pp->BackBufferHeight,
                   pp->BackBufferFormat, pp->SwapEffect, pp->hDeviceWindow);
    g_last_pp_width  = static_cast<int>(pp->BackBufferWidth);
    g_last_pp_height = static_cast<int>(pp->BackBufferHeight);
}

HRESULT STDMETHODCALLTYPE hk_CreateDevice(
    IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface)
{
    mtr::log::info("CreateDevice: adapter=%u type=%u flags=0x%08lX hFocus=%p",
                   Adapter, DeviceType, BehaviorFlags, hFocusWindow);
    log_pp(pPresentationParameters, "CreateDevice");

    HRESULT hr = g_orig_CreateDevice(This, Adapter, DeviceType, hFocusWindow,
                                     BehaviorFlags, pPresentationParameters,
                                     ppReturnedDeviceInterface);
    mtr::log::info("CreateDevice -> hr=0x%08lX device=%p", hr,
                   ppReturnedDeviceInterface ? *ppReturnedDeviceInterface : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_EndScene(IDirect3DDevice9* dev) {
    // Snapshot per-frame counters before the menu draws, so the UI sees the
    // just-completed frame's numbers (not in-flight).
    mtr::vis_test_probe::frame_tick();
    mtr::scene_vis_log::frame_tick();
    // Decouple telemetry: count the render frame for the EMA. Cheap (one
    // QPC + one atomic add). Always-on; mode==OFF doesn't affect it.
    mtr::sim_decouple::on_render_frame();
    mtr::menu::on_end_scene(dev);
    HRESULT hr = g_orig_EndScene(dev);
    // FPS cap: spin AFTER EndScene returns. Frame is committed; spinning here
    // doesn't block GPU work (which keeps draining the command buffer) and
    // happens before Present, where vsync (if enabled) will wait further.
    mtr::fps_limit::tick();
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    log_pp(pp, "Reset");
    mtr::menu::on_reset_pre(dev);
    HRESULT hr = g_orig_Reset(dev, pp);
    if (SUCCEEDED(hr)) mtr::menu::on_reset_post(dev);
    {
        std::scoped_lock lock(g_seen_mu); // re-log projections after Reset
        g_seen_count.store(0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* pMatrix) {
    return g_orig_SetTransform(dev, state, pMatrix);
}

void __cdecl hk_WrapSetTransform(const D3DMATRIX* pMatrix) {
    void* caller = _ReturnAddress();

    if (pMatrix) {
        const DWORD state = *reinterpret_cast<const DWORD*>(kStateGlobalVA);
        const float m00 = pMatrix->m[0][0];
        const float m11 = pMatrix->m[1][1];
        const float m32 = pMatrix->m[3][2];
        const float m33 = pMatrix->m[3][3];
        const bool is_perspective = (m33 == 0.0f) && (m32 != 0.0f);

        // Existing perspective-projection log (used by world-camera RE).
        if (state == D3DTS_PROJECTION && is_perspective) {
            std::scoped_lock lock(g_seen_mu);
            const int n = g_seen_count.load();
            bool seen = false;
            for (int i = 0; i < n; ++i) {
                if (g_seen[i].caller == caller &&
                    g_seen[i].m00 == m00 && g_seen[i].m11 == m11) { seen = true; break; }
            }
            if (!seen && n < kMaxSeenProj) {
                g_seen[n] = { m00, m11, caller };
                g_seen_count.store(n + 1);
                const float aspect = (m00 != 0.0f) ? (m11 / m00) : 0.0f;
                const int gw = *reinterpret_cast<const int*>(0x6FBC38);
                const int gh = *reinterpret_cast<const int*>(0x6FBC3C);
                mtr::log::info("WrapSetTransform[PROJECTION] #%d  caller=%p  "
                               "m00=%.6f m11=%.6f  aspect=%.4f  game.client=%dx%d",
                               n, caller, m00, m11, aspect, gw, gh);
            }
        }

        // UI-pillarbox RE logging (Phase D6 first-principles): dump
        // matrices set from INSIDE render_sprite_batcher's address range
        // (0x4E8D30..0x4E9321). Filtering by caller range avoids the
        // dedup table filling up with world-camera VIEW updates (which
        // change every frame and have nothing to do with the UI pass).
        const uintptr_t ip = reinterpret_cast<uintptr_t>(caller);
        const bool from_sprite_batcher = (ip >= 0x4E8D30 && ip <= 0x4E9400);
        if (from_sprite_batcher) {
            static std::atomic<int> ui_seen_n{0};
            static struct {
                void* caller; DWORD state;
                float m00, m11, m22, m30, m31;
            } ui_seen[128]{};
            static std::mutex ui_seen_mu;
            std::scoped_lock lock(ui_seen_mu);
            const float m22 = pMatrix->m[2][2];
            const float m30 = pMatrix->m[3][0];
            const float m31 = pMatrix->m[3][1];
            int n = ui_seen_n.load();
            bool seen = false;
            for (int i = 0; i < n; ++i) {
                if (ui_seen[i].caller == caller &&
                    ui_seen[i].state == state &&
                    ui_seen[i].m00 == m00 && ui_seen[i].m11 == m11 &&
                    ui_seen[i].m22 == m22 &&
                    ui_seen[i].m30 == m30 && ui_seen[i].m31 == m31) { seen = true; break; }
            }
            if (!seen && n < 128) {
                ui_seen[n] = { caller, state, m00, m11, m22, m30, m31 };
                ui_seen_n.store(n + 1);
                const char* state_name =
                    (state == D3DTS_WORLD)      ? "WORLD" :
                    (state == D3DTS_VIEW)       ? "VIEW"  :
                    (state == D3DTS_PROJECTION) ? "PROJ"  : "OTHER";
                mtr::log::info(
                    "UI_RE[%s] caller=%p (state=%lu)\n"
                    "  | %.6f %.6f %.6f %.6f |\n"
                    "  | %.6f %.6f %.6f %.6f |\n"
                    "  | %.6f %.6f %.6f %.6f |\n"
                    "  | %.6f %.6f %.6f %.6f |",
                    state_name, caller, static_cast<unsigned long>(state),
                    pMatrix->m[0][0], pMatrix->m[0][1], pMatrix->m[0][2], pMatrix->m[0][3],
                    pMatrix->m[1][0], pMatrix->m[1][1], pMatrix->m[1][2], pMatrix->m[1][3],
                    pMatrix->m[2][0], pMatrix->m[2][1], pMatrix->m[2][2], pMatrix->m[2][3],
                    pMatrix->m[3][0], pMatrix->m[3][1], pMatrix->m[3][2], pMatrix->m[3][3]);
            }
        }
    }

    g_orig_WrapSetTransform(pMatrix);
}

// Hook for wrap_SetTransform_state (sub_5625C0). render_sprite_batcher
// uses this variant — it takes no arguments and reads its source matrix
// from the global buffer at kStateMatrixBufferVA. Same logging logic as
// hk_WrapSetTransform but reading the matrix from the fixed buffer.
void __cdecl hk_WrapSetTransformState() {
    void* caller = _ReturnAddress();
    const uintptr_t ip = reinterpret_cast<uintptr_t>(caller);
    const bool from_sprite_batcher = (ip >= 0x4E8D30 && ip <= 0x4E9400);
    if (from_sprite_batcher) {
        const D3DMATRIX* pMatrix = reinterpret_cast<const D3DMATRIX*>(kStateMatrixBufferVA);
        const DWORD state = *reinterpret_cast<const DWORD*>(kStateGlobalVA);
        const float m00 = pMatrix->m[0][0];
        const float m11 = pMatrix->m[1][1];
        const float m22 = pMatrix->m[2][2];
        const float m30 = pMatrix->m[3][0];
        const float m31 = pMatrix->m[3][1];

        static std::atomic<int> ui_state_seen_n{0};
        static struct { void* caller; DWORD state; float m00, m11, m22, m30, m31; } ui_state_seen[128]{};
        static std::mutex ui_state_seen_mu;
        std::scoped_lock lock(ui_state_seen_mu);
        int n = ui_state_seen_n.load();
        bool seen = false;
        for (int i = 0; i < n; ++i) {
            if (ui_state_seen[i].caller == caller &&
                ui_state_seen[i].state == state &&
                ui_state_seen[i].m00 == m00 && ui_state_seen[i].m11 == m11 &&
                ui_state_seen[i].m22 == m22 &&
                ui_state_seen[i].m30 == m30 && ui_state_seen[i].m31 == m31) { seen = true; break; }
        }
        if (!seen && n < 128) {
            ui_state_seen[n] = { caller, state, m00, m11, m22, m30, m31 };
            ui_state_seen_n.store(n + 1);
            const char* state_name =
                (state == D3DTS_WORLD)      ? "WORLD" :
                (state == D3DTS_VIEW)       ? "VIEW"  :
                (state == D3DTS_PROJECTION) ? "PROJ"  : "OTHER";
            mtr::log::info(
                "UI_RE_SB[%s] caller=%p (state=%lu)\n"
                "  | %.6f %.6f %.6f %.6f |\n"
                "  | %.6f %.6f %.6f %.6f |\n"
                "  | %.6f %.6f %.6f %.6f |\n"
                "  | %.6f %.6f %.6f %.6f |",
                state_name, caller, static_cast<unsigned long>(state),
                pMatrix->m[0][0], pMatrix->m[0][1], pMatrix->m[0][2], pMatrix->m[0][3],
                pMatrix->m[1][0], pMatrix->m[1][1], pMatrix->m[1][2], pMatrix->m[1][3],
                pMatrix->m[2][0], pMatrix->m[2][1], pMatrix->m[2][2], pMatrix->m[2][3],
                pMatrix->m[3][0], pMatrix->m[3][1], pMatrix->m[3][2], pMatrix->m[3][3]);
        }
    }

    g_orig_WrapSetTransformState();
}

int __cdecl hk_BuildProjMatrix(float fov_deg, float aspect, float near_, float far_) {
    // Skip RT-probe / overlay-quad projections (square aspect ~ 1.0): those
    // should not get the user's wide aspect override or FOV change. Everything
    // else is treated as a world-camera projection.
    //
    // Note: a narrow-FOV "UI / HUD perspective" hypothesis was investigated
    // and ruled out (verified by runtime logs 2026-05-06): the game's HUD
    // does NOT go through this builder; it goes through the sprite batcher
    // (sub_4E8D30). Override of the HUD/menu aspect lives in
    // `mtr::sprite_matrix` + `mtr::ui_aspect_rules`, not here.
    const bool is_main = (aspect < 0.99f || aspect > 1.01f);

    if (is_main) {
        const float target = mtr::aspect::current();
        if (target > 0.1f && target < 10.0f) aspect = target;

        // FOV override -- engine reads defproj.fov once at camera-init and
        // caches it in per-camera projection state. Console writes don't
        // propagate. Apply here at the projection-build site so the override
        // is live every frame.
        if (mtr::fov::has_override()) {
            fov_deg = mtr::fov::current();
        }
    }
    return g_orig_BuildProjMatrix(fov_deg, aspect, near_, far_);
}

// Hook for the ortho-projection builder (sub_562B70). DIAGNOSTIC ONLY.
//
// History: a HUD-aspect-pillarbox path was implemented here based on the
// hypothesis that HUD/menus go through this builder. Runtime testing
// (2026-05-06) ruled this out: the only caller during runtime is the debug
// 4:3 wireframe overlay (`debug_overlay_draw_4x3_wireframe`, called once
// at startup). The actual HUD path is the sprite batcher (sub_4E8D30) and
// override lives in `mtr::sprite_matrix` + `mtr::ui_aspect_rules`.
//
// We keep this hook + its log so future RE work can confirm whether any
// new caller appears (e.g. a pause-screen overlay we haven't observed),
// but we no longer modify L/R bounds.
int __cdecl hk_BuildOrtho(float l, float r, float t, float b, float n, float f) {
    static std::atomic<int> g_seen{0};
    void* caller = _ReturnAddress();
    int seq = g_seen.fetch_add(1);
    if (seq < 8) {
        mtr::log::info("BuildOrtho #%d: l=%.3f r=%.3f t=%.3f b=%.3f n=%.3f f=%.3f  caller=%p",
                       seq, l, r, t, b, n, f, caller);
    }
    return g_orig_BuildOrtho(l, r, t, b, n, f);
}

// Diagnostic hooks for the sprite-batcher matrix path. Args are passed as
// int but represent IEEE 754 floats (caller pushes 0x40000000 = 2.0f, etc).
// Cap log count so we don't spam — the batcher fires once per frame so 8
// distinct (a1,a2,a3,caller) tuples is plenty to confirm reach + invariants.
namespace {
struct MatrixSetSeen {
    uint32_t a1, a2, a3;
    void* caller;
};
constexpr int kMaxMatrixSetSeen = 8;
MatrixSetSeen g_msa_seen[kMaxMatrixSetSeen]{};
std::atomic<int> g_msa_seen_count{0};
MatrixSetSeen g_msb_seen[kMaxMatrixSetSeen]{};
std::atomic<int> g_msb_seen_count{0};
std::mutex g_ms_mu;

void log_matrix_set(const char* tag, MatrixSetSeen* seen, std::atomic<int>& seen_count,
                    int a1, int a2, int a3, void* caller) {
    const float f1 = *reinterpret_cast<const float*>(&a1);
    const float f2 = *reinterpret_cast<const float*>(&a2);
    const float f3 = *reinterpret_cast<const float*>(&a3);

    std::scoped_lock lock(g_ms_mu);
    const int n = seen_count.load();
    for (int i = 0; i < n; ++i) {
        if (seen[i].a1 == static_cast<uint32_t>(a1) &&
            seen[i].a2 == static_cast<uint32_t>(a2) &&
            seen[i].a3 == static_cast<uint32_t>(a3) &&
            seen[i].caller == caller) {
            return; // already logged this exact tuple
        }
    }
    if (n < kMaxMatrixSetSeen) {
        seen[n] = { static_cast<uint32_t>(a1), static_cast<uint32_t>(a2),
                    static_cast<uint32_t>(a3), caller };
        seen_count.store(n + 1);
        mtr::log::info("%s #%d: a1=%.4f a2=%.4f a3=%.4f  (raw 0x%08X 0x%08X 0x%08X)  caller=%p",
                       tag, n, f1, f2, f3,
                       static_cast<uint32_t>(a1), static_cast<uint32_t>(a2),
                       static_cast<uint32_t>(a3), caller);
    }
}
} // anonymous namespace

// Apply per-arg multiplier when sprite_matrix override is enabled. Args are
// IEEE 754 floats passed via int registers; we round-trip through float for
// the multiply, then re-encode to int. Multiplier of 1.0 = passthrough.
int hk_apply_float_mul(int arg, float mul) {
    if (mul == 1.0f) return arg;
    float f = *reinterpret_cast<const float*>(&arg);
    f *= mul;
    return *reinterpret_cast<const int*>(&f);
}

// Add a constant to a float-as-int arg (same encoding trick). Used by the
// position-offset channel to nudge the UI translation post-pillarbox.
int hk_apply_float_add(int arg, float add) {
    if (add == 0.0f) return arg;
    float f = *reinterpret_cast<const float*>(&arg);
    f += add;
    return *reinterpret_cast<const int*>(&f);
}

// Compute the X-pillarbox factor from current top-screen + ui_aspect_rules.
// Returns 1.0 if no rule matches OR auto-mode is off — caller should treat
// 1.0 as passthrough.
float resolve_auto_factor() {
    if (!mtr::sprite_matrix::auto_from_rules()) return 1.0f;
    char top[64] = {0};
    mtr::screen_push::current_top_name(top, sizeof(top));
    float target = mtr::ui_aspect_rules::resolve_aspect(top);
    if (target <= 0.0f) return 1.0f;
    float screen = mtr::aspect::current();
    if (screen <= 0.0f) return 1.0f;
    return target / screen;
}

// Each matrix-override control activates independently:
//   - pass_override_factor (Phase 3 split-pass) wins absolutely.
//   - auto_from_rules: applies if the toggle is on AND a rule matches —
//     does NOT require sprite_matrix::enabled().
//   - manual sliders (mul_a_*, mul_b_*): apply only when the master
//     "Enabled" toggle is on AND no auto-from-rules match. They're the
//     manual override path.
//   - pos_offset_x/y: applied if non-zero, regardless of any other state.
//     Lets the user nudge UI placement without configuring anything else.
int __cdecl hk_MatrixSetXformA(int a1, int a2, int a3) {
    log_matrix_set("MatrixSetXformA", g_msa_seen, g_msa_seen_count, a1, a2, a3, _ReturnAddress());

    const float ovr = mtr::sprite_matrix::pass_override_factor();
    if (ovr != 0.0f) {
        a1 = hk_apply_float_mul(a1, ovr);
        return g_orig_MatrixSetXformA(a1, a2, a3);
    }

    // auto_from_rules path is independent of `enabled` master — if the
    // toggle is on and a rule matches, apply the factor.
    const float auto_f = resolve_auto_factor();
    if (auto_f != 1.0f) {
        a1 = hk_apply_float_mul(a1, auto_f);
    } else if (mtr::sprite_matrix::enabled()) {
        a1 = hk_apply_float_mul(a1, mtr::sprite_matrix::mul_a_a1());
        a2 = hk_apply_float_mul(a2, mtr::sprite_matrix::mul_a_a2());
        a3 = hk_apply_float_mul(a3, mtr::sprite_matrix::mul_a_a3());
    }
    return g_orig_MatrixSetXformA(a1, a2, a3);
}

int __cdecl hk_MatrixSetXformB(int a1, int a2, int a3) {
    log_matrix_set("MatrixSetXformB", g_msb_seen, g_msb_seen_count, a1, a2, a3, _ReturnAddress());

    // Auto/pass-override factors are applied ONLY to XformA (the SCALE
    // matrix). The render_sprite_batcher pipeline is pre-multiply
    // (top = translate × scale), so the engine's translate(-0.5,-0.5,0) is
    // already inside the scale's column. Scaling scale.sx by F yields
    // m41 = -0.5 * 2F = -F and m11 = 2F → vert (0,0)→clip(-F,1),
    // vert (1,1)→clip(F,-1). Range (-F,F): correctly-centered pillarbox.
    //
    // Multiplying translate.tx by F here would make m41 = -0.5F * 2 = -F
    // but leave m11 = 2 → range (-F, 2-F). Width unchanged, center shifted
    // right by (1-F). That was the visible "menu offset to the right" bug.
    //
    // Manual sprite_matrix sliders (mul_b_*) remain available for users
    // who explicitly want to nudge translation. pos_offset_x/y is the
    // additive screen-space nudge and applies regardless of factor state.
    if (mtr::sprite_matrix::enabled()) {
        a1 = hk_apply_float_mul(a1, mtr::sprite_matrix::mul_b_a1());
        a2 = hk_apply_float_mul(a2, mtr::sprite_matrix::mul_b_a2());
        a3 = hk_apply_float_mul(a3, mtr::sprite_matrix::mul_b_a3());
    }
    const float dx = mtr::sprite_matrix::pos_offset_x();
    const float dy = mtr::sprite_matrix::pos_offset_y();
    if (dx != 0.0f) a1 = hk_apply_float_add(a1, dx);
    if (dy != 0.0f) a2 = hk_apply_float_add(a2, dy);
    return g_orig_MatrixSetXformB(a1, a2, a3);
}

int __fastcall hk_GameCameraTick(int this_, int /*edx*/) {
    // Run the engine's PathCam tick. We no longer override here -- override
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
// CACHED frustum buffer (outer+0xD4) -- the engine builds the frustum at
// scene init and never rebuilds it per frame, so hooking build_view_frustum
// alone never fires after startup. This is a data-level override on the
// engine's own buffer (not a code patch) and is the cleanest non-crutch
// way to extend the camera-relative cull plane.
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
            // Anything that rebuilds the frustum reads from here.
            *reinterpret_cast<float*>(outer + 0x44) = far_user;

            // Cached frustum buffer at outer+0xD4 (= projection-cache+0x94).
            // Layout (from build_view_frustum decompile):
            //   +0..15  : near plane  (n.x, n.y, n.z, w)
            //   +16..31 : far plane   (n.x=0, n.y=0, n.z=-1, w=-far)
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
            fb[4] = 0.0f;       // +16
            fb[5] = 0.0f;       // +20
            fb[6] = -1.0f;      // +24
            fb[7] = -far_user;  // +28

            // Recompute far corners from current fov / aspect (in projection cache)
            const float fov_deg = *reinterpret_cast<float*>(outer + 0x48);
            const float aspect  = *reinterpret_cast<float*>(outer + 0x4C);
            const float fov_v_half = 0.017453292f * fov_deg * 0.5f;
            const float s_v = std::tan(fov_v_half);
            const float s_h = s_v * aspect;
            const float fz = -far_user;
            const float fx = -s_h * far_user;
            const float fy = -s_v * far_user;

            float* fc = reinterpret_cast<float*>(outer + 0xD4 + 352);
            // C0: (-s_h*far, -s_v*far, -far)
            fc[0]  = fx;   fc[1]  = fy;   fc[2]  = fz;
            // C1: (+s_h*far, -s_v*far, -far)
            fc[3]  = -fx;  fc[4]  = fy;   fc[5]  = fz;
            // C2: (+s_h*far, +s_v*far, -far)
            fc[6]  = -fx;  fc[7]  = -fy;  fc[8]  = fz;
            // C3: (-s_h*far, +s_v*far, -far)
            fc[9]  = fx;   fc[10] = -fy;  fc[11] = fz;
        }

        // Side-plane cull disable. The frustum buffer's top/bottom/left/
        // right planes (offsets +32 / +48 / +64 / +80, each 16 bytes) cull
        // geometry at the screen edges. When freecam pans outside the
        // engine's original frustum, those side planes still apply and clip
        // legitimate geometry. Setting all four to (0,0,0,1) means
        // dot(plane.xyz, p) + plane.w = 1 > 0 for every point -> always
        // pass. Same data-level technique as the far-plane override.
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
        // Force-pass when override is active. (Renders the entire object set;
        // perf hit is fine for now -- we want to see if THIS is the gate.)
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

// Diagnostic: capture D3D9 user clip plane usage. The user describes the
// missing draw-distance gate as a "global plane clip" -- D3DRS_CLIPPLANEENABLE
// + SetClipPlane(0..5, plane_eqn) is exactly that on the hardware level.
// We log first 30 SetClipPlane calls and any non-zero CLIPPLANEENABLE writes
// to confirm/deny that this is the mechanism the engine uses for distance
// culling. If non-zero CLIPPLANEENABLE is logged we know clip planes ARE in
// use; if not, we rule it out and look elsewhere.
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
    return g_orig_SetRenderState(dev, state, value);
}

char __fastcall hk_CameraApplyState(int this_, int /*edx*/) {
    // Dirty the frustum cache PRE so the orig's `if (dirty)` branch runs
    // and calls build_view_frustum — which our hk_BuildFrustum will
    // intercept to substitute far / corners.
    if (this_ && mtr::draw_dist::has_override()) {
        *reinterpret_cast<BYTE*>(this_ + 144) = 1;
    }
    return g_orig_CameraApplyState(this_, 0);
}

int __cdecl hk_BuildFrustum(int out_buf, float near_, float far_,
                             char has_clip, int extra_axis,
                             float extra_d, float fov_deg, float aspect) {
    // Diagnostic: log the first few calls so we can confirm reach.
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
        const float fx = s_h * (-far_user);  // = -s_h*far  (orig: v15 * -10000)
        const float fy = s_v * (-far_user);  // = -s_v*far  (orig: v48 * -10000)

        float* fc = reinterpret_cast<float*>(out_buf + 352);
        // C0: (-s_h*far, -s_v*far, -far)  -- bottom-left far corner
        fc[0] = fx;        fc[1] = fy;        fc[2] = fz;
        // C1: (+s_h*far, -s_v*far, -far)
        fc[3] = -fx;       fc[4] = fy;        fc[5] = fz;
        // C2: (+s_h*far, +s_v*far, -far)
        fc[6] = -fx;       fc[7] = -fy;       fc[8] = fz;
        // C3: (-s_h*far, +s_v*far, -far)
        fc[9] = fx;        fc[10] = -fy;      fc[11] = fz;
    }
    return rc;
}

bool capture_vtables_and_hook() {
    HMODULE d3d9_mod = GetModuleHandleA("d3d9.dll");
    if (!d3d9_mod) d3d9_mod = LoadLibraryA("d3d9.dll");
    if (!d3d9_mod) {
        mtr::log::info("d3d9: LoadLibrary(d3d9.dll) failed");
        return false;
    }

    using PFN_Direct3DCreate9 = IDirect3D9*(WINAPI*)(UINT);
    auto pDirect3DCreate9 = reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(d3d9_mod, "Direct3DCreate9"));
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

    bool ok = true;
    if (MH_CreateHook(p_CreateDevice, &hk_CreateDevice,
                      reinterpret_cast<void**>(&g_orig_CreateDevice)) != MH_OK) {
        mtr::log::info("d3d9: MH_CreateHook(CreateDevice) failed"); ok = false;
    }
    if (MH_CreateHook(p_EndScene, &hk_EndScene,
                      reinterpret_cast<void**>(&g_orig_EndScene)) != MH_OK) {
        mtr::log::info("d3d9: MH_CreateHook(EndScene) failed"); ok = false;
    }
    if (MH_CreateHook(p_Reset, &hk_Reset,
                      reinterpret_cast<void**>(&g_orig_Reset)) != MH_OK) {
        mtr::log::info("d3d9: MH_CreateHook(Reset) failed"); ok = false;
    }
    if (MH_CreateHook(p_SetTransform, &hk_SetTransform,
                      reinterpret_cast<void**>(&g_orig_SetTransform)) != MH_OK) {
        mtr::log::info("d3d9: MH_CreateHook(SetTransform) failed"); ok = false;
    }
    if (!ok) return false;

    if (MH_EnableHook(p_CreateDevice) != MH_OK ||
        MH_EnableHook(p_EndScene)     != MH_OK ||
        MH_EnableHook(p_Reset)        != MH_OK ||
        MH_EnableHook(p_SetTransform) != MH_OK)
    {
        mtr::log::info("d3d9: MH_EnableHook failed for one of CreateDevice/EndScene/Reset/SetTransform");
        return false;
    }

    // Diagnostic: SetClipPlane + SetRenderState (CLIPPLANEENABLE filter) so we
    // can verify whether the engine uses D3D9 user clip planes for distance
    // culling -- a strong candidate for the "global plane clip" the user is
    // describing.
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
        mtr::log::info("d3d9: hooked SetRenderState (CLIPPLANEENABLE filter)");
    } else {
        mtr::log::info("d3d9: SetRenderState hook failed");
    }

    // Game-side wrapper around SetTransform — diagnostic only (kept for now).
    void* p_wrap = reinterpret_cast<void*>(kWrapSetTransformVA);
    if (MH_CreateHook(p_wrap, &hk_WrapSetTransform,
                      reinterpret_cast<void**>(&g_orig_WrapSetTransform)) != MH_OK ||
        MH_EnableHook(p_wrap) != MH_OK) {
        mtr::log::info("d3d9: failed to hook game-side wrap_SetTransform at %p", p_wrap);
    } else {
        mtr::log::info("d3d9: hooked wrap_SetTransform at %p", p_wrap);
    }

    // Game-side wrapper that reads matrix from a global buffer — used by
    // render_sprite_batcher (and other 2D paths). Diagnostic logging for
    // the UI-RE work; will become the matrix-replacement injection point
    // for first-principles pillarbox once the math is derived.
    void* p_wrap_state = reinterpret_cast<void*>(kWrapSetTransformStateVA);
    if (MH_CreateHook(p_wrap_state, &hk_WrapSetTransformState,
                      reinterpret_cast<void**>(&g_orig_WrapSetTransformState)) != MH_OK ||
        MH_EnableHook(p_wrap_state) != MH_OK) {
        mtr::log::info("d3d9: failed to hook game-side wrap_SetTransform_state at %p", p_wrap_state);
    } else {
        mtr::log::info("d3d9: hooked wrap_SetTransform_state at %p", p_wrap_state);
    }

    // Game-side projection matrix builder — substitute aspect at the input.
    void* p_build = reinterpret_cast<void*>(kBuildProjMatrixVA);
    if (MH_CreateHook(p_build, &hk_BuildProjMatrix,
                      reinterpret_cast<void**>(&g_orig_BuildProjMatrix)) != MH_OK ||
        MH_EnableHook(p_build) != MH_OK) {
        mtr::log::info("d3d9: failed to hook game-side build_proj_matrix at %p", p_build);
    } else {
        mtr::log::info("d3d9: hooked build_proj_matrix at %p", p_build);
    }

    // Game-side ortho-projection builder — proper injection point for HUD/UI
    // aspect override. Replaces the previous FOV<10 heuristic in
    // hk_BuildProjMatrix as the primary UI override path.
    void* p_ortho = reinterpret_cast<void*>(kBuildOrthoVA);
    if (MH_CreateHook(p_ortho, &hk_BuildOrtho,
                      reinterpret_cast<void**>(&g_orig_BuildOrtho)) != MH_OK ||
        MH_EnableHook(p_ortho) != MH_OK) {
        mtr::log::info("d3d9: failed to hook build_ortho at %p", p_ortho);
    } else {
        mtr::log::info("d3d9: hooked build_ortho at %p", p_ortho);
    }

    // Sprite-batcher matrix builders — diagnostic to confirm the HUD path.
    // Static RE shows render_sprite_batcher (sub_4E8D30) bypasses
    // build_ortho_matrix (sub_562B70) and uses these two helpers instead.
    // Logging only — substitution comes after runtime confirmation.
    void* p_msa = reinterpret_cast<void*>(kMatrixSetXformAVA);
    if (MH_CreateHook(p_msa, &hk_MatrixSetXformA,
                      reinterpret_cast<void**>(&g_orig_MatrixSetXformA)) != MH_OK ||
        MH_EnableHook(p_msa) != MH_OK) {
        mtr::log::info("d3d9: failed to hook matrix_set_via_xform_a at %p", p_msa);
    } else {
        mtr::log::info("d3d9: hooked matrix_set_via_xform_a (sprite proj) at %p", p_msa);
    }

    void* p_msb = reinterpret_cast<void*>(kMatrixSetXformBVA);
    if (MH_CreateHook(p_msb, &hk_MatrixSetXformB,
                      reinterpret_cast<void**>(&g_orig_MatrixSetXformB)) != MH_OK ||
        MH_EnableHook(p_msb) != MH_OK) {
        mtr::log::info("d3d9: failed to hook matrix_set_via_xform_b at %p", p_msb);
    } else {
        mtr::log::info("d3d9: hooked matrix_set_via_xform_b (sprite view) at %p", p_msb);
    }

    // Camera "if dirty rebuild" gateway — invalidates cache so live changes
    // take effect without needing a scene transition.
    void* p_compute = reinterpret_cast<void*>(kCameraComputeVA);
    if (MH_CreateHook(p_compute, &hk_CameraCompute,
                      reinterpret_cast<void**>(&g_orig_CameraCompute)) != MH_OK ||
        MH_EnableHook(p_compute) != MH_OK) {
        mtr::log::info("d3d9: failed to hook game-side camera_compute at %p", p_compute);
    } else {
        mtr::log::info("d3d9: hooked camera_compute at %p", p_compute);
    }

    // Gameplay camera-tick — capture controller for MMB-teleport plumbing.
    // No view-matrix override here (moved to camera_apply_all_active POST).
    void* p_camtick = reinterpret_cast<void*>(kGameCameraTickVA);
    if (MH_CreateHook(p_camtick, &hk_GameCameraTick,
                      reinterpret_cast<void**>(&g_orig_GameCameraTick)) != MH_OK ||
        MH_EnableHook(p_camtick) != MH_OK) {
        mtr::log::info("d3d9: failed to hook gameplay_camera_tick at %p", p_camtick);
    } else {
        mtr::log::info("d3d9: hooked gameplay_camera_tick at %p", p_camtick);
    }

    // Per-camera apply (PRE-hook). Overwrites *(outer+0x34) before the orig
    // propagates it to globals + D3D. Single funnel for every camera class.
    void* p_apply = reinterpret_cast<void*>(kPerCameraApplyVA);
    if (MH_CreateHook(p_apply, &hk_PerCameraApply,
                      reinterpret_cast<void**>(&g_orig_PerCameraApply)) != MH_OK ||
        MH_EnableHook(p_apply) != MH_OK) {
        mtr::log::info("d3d9: failed to hook per_camera_apply at %p", p_apply);
    } else {
        mtr::log::info("d3d9: hooked per_camera_apply at %p", p_apply);
    }

    // View-frustum builder -- substitute the far parameter so culling sees
    // our user-chosen draw distance (NOT the projection-matrix far which
    // only affects depth precision).
    void* p_frustum = reinterpret_cast<void*>(kBuildFrustumVA);
    if (MH_CreateHook(p_frustum, &hk_BuildFrustum,
                      reinterpret_cast<void**>(&g_orig_BuildFrustum)) != MH_OK ||
        MH_EnableHook(p_frustum) != MH_OK) {
        mtr::log::info("d3d9: failed to hook build_view_frustum at %p", p_frustum);
    } else {
        mtr::log::info("d3d9: hooked build_view_frustum at %p", p_frustum);
    }

    // [removed] camera_apply_state PRE-hook + vis_test thunk hook -- both
    // caused save-load instability. apply_state was rebuilding nothing
    // (build_frustum still only fired 5 times at init in the diagnostic).
    // vis_test (sub_4E0B90) is a 1-instruction SecuROM thunk that returns
    // a structure pointer (low byte used as bool); hooking it via MinHook
    // left a fragile trampoline that crashed during save loads even when
    // the override was OFF. Need a different approach for draw distance.

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

void install() {
    HANDLE t = CreateThread(nullptr, 0, deferred_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

} // namespace mtr::d3d9hook
