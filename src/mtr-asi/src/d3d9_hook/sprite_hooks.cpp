// Sprite-batcher / UI matrix hooks for d3d9_hook.
//
//   wrap_SetTransform        (0x5625E0) — diagnostic: per-state matrix log,
//                                          filtered by sprite-batcher caller
//                                          range so 3D camera VIEW updates
//                                          don't dominate the dedup table.
//   wrap_SetTransform_state  (0x5625C0) — variant used by render_sprite_batcher;
//                                          source matrix lives at fixed
//                                          buffer kStateMatrixBufferVA.
//   build_ortho              (0x562B70) — historical UI pillarbox path;
//                                          confirmed inert at runtime
//                                          (only caller is the debug 4:3
//                                          wireframe overlay).
//   matrix_set_via_xform_a   (0x562AA0) — sprite scale matrix builder.
//                                          UI pillarbox factor lives here.
//   matrix_set_via_xform_b   (0x562AE0) — sprite translate matrix builder;
//                                          position-offset nudges live here.
//   IDirect3DDevice9::SetTransform — vtable[44]; pure passthrough today
//                                     (left in place so any future filter
//                                      has a stable mount point).

#include "d3d9_internal.h"

#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <intrin.h>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::freecam { bool trace_armed(); }
namespace mtr::aspect          { float current(); }
namespace mtr::screen_push     { bool current_top_name(char* out, size_t out_size); }
namespace mtr::ui_aspect_rules { float resolve_aspect(const char* top_name); }
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

namespace mtr::d3d9hook {

namespace {

using namespace detail;

PFN_SetTransform          g_orig_SetTransform          = nullptr;
PFN_WrapSetTransform      g_orig_WrapSetTransform      = nullptr;
PFN_WrapSetTransformState g_orig_WrapSetTransformState = nullptr;
PFN_BuildOrtho            g_orig_BuildOrtho            = nullptr;
PFN_MatrixSet3            g_orig_MatrixSetXformA       = nullptr;
PFN_MatrixSet3            g_orig_MatrixSetXformB       = nullptr;

// Track distinct (m00, m11, caller) projection tuples seen via wrap_SetTransform
// so the log doesn't spam — we want each unique aspect with the function
// that produced it.
constexpr int kMaxSeenProj = 32;
struct SeenProj { float m00, m11; void* caller; };
SeenProj g_seen[kMaxSeenProj]{};
std::atomic<int> g_seen_count{0};
std::mutex g_seen_mu;

HRESULT STDMETHODCALLTYPE hk_SetTransform(IDirect3DDevice9* dev,
                                          D3DTRANSFORMSTATETYPE state,
                                          const D3DMATRIX* pMatrix) {
    // Diagnostic: while the freecam snap-back trace probe is armed, log
    // every D3DTS_WORLD matrix translation AND its caller's return
    // address — that tells us which engine function submitted the
    // matrix, so we can find the source field for the player draw.
    // Diagnostic gate removed: we identified the renderer's source as
    // the per-draw matrix at 0x724B10 populated by game_render_main_scene
    // (sub_563C70). The fix is upstream — see freecam.cpp's
    // entity_transform_tick skip-flag patch which prevents the snap-back
    // anim from driving the matrix in the first place.
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

        // UI-pillarbox RE logging: dump matrices set from inside
        // render_sprite_batcher's address range (0x4E8D30..0x4E9321).
        // Filtering by caller range avoids the dedup table filling up
        // with world-camera VIEW updates (which change every frame and
        // have nothing to do with the UI pass).
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

// render_sprite_batcher uses the no-arg variant — source matrix from
// global buffer at kStateMatrixBufferVA. Same logging logic as
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

// Hook for the ortho-projection builder (sub_562B70). DIAGNOSTIC ONLY.
//
// History: a HUD-aspect-pillarbox path was implemented here based on the
// hypothesis that HUD/menus go through this builder. Runtime testing
// (2026-05-06) ruled this out: the only caller during runtime is the
// debug 4:3 wireframe overlay (`debug_overlay_draw_4x3_wireframe`,
// called once at startup). The actual HUD path is the sprite batcher
// (sub_4E8D30) and override lives in `mtr::sprite_matrix` +
// `mtr::ui_aspect_rules`.
//
// We keep this hook + log so future RE work can confirm whether any
// new caller appears (e.g. a pause-screen overlay we haven't observed),
// but we no longer modify L/R bounds.
int __cdecl hk_BuildOrtho(float l, float r, float t, float b, float n_, float f) {
    static std::atomic<int> g_seen{0};
    void* caller = _ReturnAddress();
    int seq = g_seen.fetch_add(1);
    if (seq < 8) {
        mtr::log::info("BuildOrtho #%d: l=%.3f r=%.3f t=%.3f b=%.3f n=%.3f f=%.3f  caller=%p",
                       seq, l, r, t, b, n_, f, caller);
    }
    return g_orig_BuildOrtho(l, r, t, b, n_, f);
}

// Diagnostic state for the matrix-set helpers. Args are passed as int
// but represent IEEE 754 floats (caller pushes 0x40000000 = 2.0f, etc).
// Cap log count so we don't spam — the batcher fires once per frame so
// 8 distinct (a1,a2,a3,caller) tuples is plenty to confirm reach + invariants.
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
            return;
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

// Apply per-arg multiplier when sprite_matrix override is enabled. Args
// are IEEE 754 floats passed via int registers; we round-trip through
// float for the multiply, then re-encode to int. Multiplier of 1.0 = passthrough.
int hk_apply_float_mul(int arg, float mul) {
    if (mul == 1.0f) return arg;
    float f = *reinterpret_cast<const float*>(&arg);
    f *= mul;
    return *reinterpret_cast<const int*>(&f);
}

// Add a constant to a float-as-int arg (same encoding trick). Used by the
// position-offset channel to nudge UI translation post-pillarbox.
int hk_apply_float_add(int arg, float add) {
    if (add == 0.0f) return arg;
    float f = *reinterpret_cast<const float*>(&arg);
    f += add;
    return *reinterpret_cast<const int*>(&f);
}

// X-pillarbox factor from current top-screen + ui_aspect_rules. Returns
// 1.0 if no rule matches OR auto-mode is off — caller treats 1.0 as
// passthrough.
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
//     "Enabled" toggle is on AND no auto-from-rules match.
//   - pos_offset_x/y: applied if non-zero, regardless of any other state.
int __cdecl hk_MatrixSetXformA(int a1, int a2, int a3) {
    log_matrix_set("MatrixSetXformA", g_msa_seen, g_msa_seen_count, a1, a2, a3, _ReturnAddress());

    const float ovr = mtr::sprite_matrix::pass_override_factor();
    if (ovr != 0.0f) {
        a1 = hk_apply_float_mul(a1, ovr);
        return g_orig_MatrixSetXformA(a1, a2, a3);
    }

    // auto_from_rules path is independent of `enabled` master.
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
    // (top = translate × scale), so the engine's translate(-0.5,-0.5,0)
    // is already inside the scale's column. Multiplying translate.tx by
    // F here would shift the center right by (1-F) — the visible "menu
    // offset to the right" bug.
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

void install_sprite_hooks(void* p_SetTransform) {
    // SetTransform — passthrough today; left as a stable mount point.
    if (MH_CreateHook(p_SetTransform, &hk_SetTransform,
                      reinterpret_cast<void**>(&g_orig_SetTransform)) != MH_OK ||
        MH_EnableHook(p_SetTransform) != MH_OK) {
        mtr::log::info("d3d9: SetTransform hook failed");
    }

    install_one("wrap_SetTransform",
                reinterpret_cast<void*>(kWrapSetTransformVA),
                reinterpret_cast<void*>(&hk_WrapSetTransform),
                reinterpret_cast<void**>(&g_orig_WrapSetTransform));
    install_one("wrap_SetTransform_state",
                reinterpret_cast<void*>(kWrapSetTransformStateVA),
                reinterpret_cast<void*>(&hk_WrapSetTransformState),
                reinterpret_cast<void**>(&g_orig_WrapSetTransformState));
    install_one("build_ortho",
                reinterpret_cast<void*>(kBuildOrthoVA),
                reinterpret_cast<void*>(&hk_BuildOrtho),
                reinterpret_cast<void**>(&g_orig_BuildOrtho));
    install_one("matrix_set_via_xform_a (sprite proj)",
                reinterpret_cast<void*>(kMatrixSetXformAVA),
                reinterpret_cast<void*>(&hk_MatrixSetXformA),
                reinterpret_cast<void**>(&g_orig_MatrixSetXformA));
    install_one("matrix_set_via_xform_b (sprite view)",
                reinterpret_cast<void*>(kMatrixSetXformBVA),
                reinterpret_cast<void*>(&hk_MatrixSetXformB),
                reinterpret_cast<void**>(&g_orig_MatrixSetXformB));
}

} // namespace detail

} // namespace mtr::d3d9hook
