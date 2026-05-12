// Trigger box overlay — Phase 1 + 2 implementation.
//
// Per the v2 plan in research/findings/trigger-box-overlay-plan-2026-05-09.md:
//   - View matrix at 0x00724C10 (16 floats, written by sub_4C1BA0 per-camera
//     apply, RH coordinate system, Y-up, D3D9 row-vector convention).
//   - Projection matrix at 0x00745AA0 (named kHaloProjMatrixVA in
//     interp_halo.cpp; rewritten each frame by halo_stash_camera_proj_params).
//   - Both reads SEH-guarded — if either faults, the overlay aborts cleanly
//     for this frame.
//
// Rendering pipeline (homogeneous parametric clip, NOT Liang-Barsky):
//   1. Compute the 8 world-space corners of the AABB.
//   2. clip = world_pos_row_vec * View * Proj   (row-vector convention).
//   3. For each of the 12 edges (pairs of corners):
//      - Run homogeneous parametric clip against 6 D3D9 frustum planes:
//          w + x >= 0,  w - x >= 0,  w + y >= 0,  w - y >= 0,
//          z >= 0,  w - z >= 0   (D3D9 z range is [0, w], NOT [-w, w]).
//      - If both endpoints outside any one plane → discard edge.
//      - If endpoints straddle a plane → lerp the 4D homogeneous endpoints
//        at t = d_a / (d_a - d_b), then continue with the clipped pair.
//      - Never divide by w until all 6 planes survived.
//   4. Whole-box reject: if all 8 corners share an outside half-space for
//      any plane, skip the box (cheap early-out).
//   5. NDC → screen with explicit D3D9 Y-flip.
//   6. ImGui::GetForegroundDrawList()->AddLine() per surviving edge.

#include "mtr/trigger_overlay.h"
#include "mtr/overlay_math.h"   // shared row_mul / mat_mul / clip_segment / etc.

#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "imgui.h"

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::trigger_overlay {

namespace {

using mtr::overlay_math::Vec4;
using mtr::overlay_math::row_mul;
using mtr::overlay_math::mat_mul;
using mtr::overlay_math::plane_dist;
using mtr::overlay_math::clip_segment;
using mtr::overlay_math::is_finite_vec4;

// === Engine globals ========================================================

constexpr uintptr_t kViewMatrixGlobalVA = 0x00724C10;
constexpr uintptr_t kProjMatrixGlobalVA = 0x00745AA0;

// === User toggles ==========================================================

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_show_test_box{true};   // Phase 1 default ON for first-light
std::atomic<int>  g_last_visible{0};
std::atomic<int>  g_export_frames{0};      // autonomous-validation export budget
std::atomic<uint64_t> g_frame_seq{0};      // monotonic frame counter for export keys

// === Math: shared via include/mtr/overlay_math.h ===========================
// (Vec4, row_mul, mat_mul, plane_dist, clip_segment, vec4_lerp, is_finite_vec4
//  all factored into the shared header so npc_overlay can reuse without
//  divergence. ndc_to_screen is also there but uses out-params instead of
//  ImVec2 — local trampoline for back-compat.)

inline ImVec2 ndc_to_screen(const Vec4& clip, float vp_w, float vp_h) {
    float sx = 0.0f, sy = 0.0f;
    mtr::overlay_math::ndc_to_screen(clip, vp_w, vp_h, &sx, &sy);
    return ImVec2(sx, sy);
}

// Whole-box reject: if all 8 corners share an outside half-space for any one
// plane, the box is fully outside that half and we skip everything.
// (Box-specific; stays in trigger_overlay since npc_overlay doesn't render
//  AABBs.)
bool whole_box_outside(const Vec4 corners[8]) {
    for (int p = 0; p < 6; ++p) {
        bool all_out = true;
        for (int i = 0; i < 8; ++i) {
            if (plane_dist(p, corners[i]) >= 0.0f) {
                all_out = false;
                break;
            }
        }
        if (all_out) return true;
    }
    return false;
}

// === AABB rendering ========================================================

// 12 edges of an AABB, indices into the 8-corner array.
constexpr int kEdgeCount = 12;
constexpr int kEdges[kEdgeCount][2] = {
    {0,1},{1,3},{3,2},{2,0},   // bottom face
    {4,5},{5,7},{7,6},{6,4},   // top face
    {0,4},{1,5},{2,6},{3,7},   // verticals
};

// Order corners so that edges between corners i and i+1 along an axis match
// the kEdges table above. Bit b of the corner index = side along axis b.
//   bit 0 → +x
//   bit 1 → +z
//   bit 2 → +y  (top vs bottom)
void build_aabb_corners(float cx, float cy, float cz,
                        float ex, float ey, float ez,
                        Vec4 out[8]) {
    for (int i = 0; i < 8; ++i) {
        const float dx = (i & 1) ? ex : -ex;
        const float dz = (i & 2) ? ez : -ez;
        const float dy = (i & 4) ? ey : -ey;
        out[i].x = cx + dx;
        out[i].y = cy + dy;
        out[i].z = cz + dz;
        out[i].w = 1.0f;
    }
}

// Project, clip, draw a single AABB. Returns the number of edges drawn
// (0 = box fully outside frustum). When `exporting` is true, also emits
// TRIGGER_OVERLAY_BOX + TRIGGER_OVERLAY_EDGE log lines so the offline
// validator can re-run the math against logged matrices.
int draw_aabb(float cx, float cy, float cz,
              float ex, float ey, float ez,
              const float view_proj[16],
              float vp_w, float vp_h,
              uint32_t color,
              bool exporting,
              uint64_t frame_seq,
              int box_idx) {
    Vec4 world[8];
    build_aabb_corners(cx, cy, cz, ex, ey, ez, world);

    Vec4 clip[8];
    for (int i = 0; i < 8; ++i) {
        clip[i] = row_mul(world[i], view_proj);
    }

    if (whole_box_outside(clip)) {
        if (exporting) {
            mtr::log::info("TRIGGER_OVERLAY_BOX f=%llu idx=%d "
                           "center=%.6f,%.6f,%.6f extents=%.6f,%.6f,%.6f "
                           "color=0x%08X clipped=whole",
                           (unsigned long long)frame_seq, box_idx,
                           cx, cy, cz, ex, ey, ez, color);
        }
        return 0;
    }

    if (exporting) {
        mtr::log::info("TRIGGER_OVERLAY_BOX f=%llu idx=%d "
                       "center=%.6f,%.6f,%.6f extents=%.6f,%.6f,%.6f "
                       "color=0x%08X clipped=partial",
                       (unsigned long long)frame_seq, box_idx,
                       cx, cy, cz, ex, ey, ez, color);
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    int edges_drawn = 0;
    for (int e = 0; e < kEdgeCount; ++e) {
        Vec4 a = clip[kEdges[e][0]];
        Vec4 b = clip[kEdges[e][1]];
        Vec4 ca, cb;
        if (!clip_segment(a, b, &ca, &cb)) continue;
        const ImVec2 sa = ndc_to_screen(ca, vp_w, vp_h);
        const ImVec2 sb = ndc_to_screen(cb, vp_w, vp_h);
        // Skip degenerate / off-screen pixels (shouldn't happen post-clip
        // but defensive).
        if (!std::isfinite(sa.x) || !std::isfinite(sa.y)
            || !std::isfinite(sb.x) || !std::isfinite(sb.y)) continue;
        dl->AddLine(sa, sb, color, 1.5f);
        if (exporting) {
            mtr::log::info("TRIGGER_OVERLAY_EDGE f=%llu box=%d i=%d "
                           "a=%.4f,%.4f b=%.4f,%.4f",
                           (unsigned long long)frame_seq, box_idx, e,
                           sa.x, sa.y, sb.x, sb.y);
        }
        ++edges_drawn;
    }
    return edges_drawn;
}

// === Matrix capture (SEH-guarded) ==========================================

bool read_view_matrix(float out[16]) {
    bool ok = false;
    __try {
        const float* src = reinterpret_cast<const float*>(kViewMatrixGlobalVA);
        for (int i = 0; i < 16; ++i) out[i] = src[i];
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

bool read_proj_matrix(float out[16]) {
    bool ok = false;
    __try {
        const float* src = reinterpret_cast<const float*>(kProjMatrixGlobalVA);
        for (int i = 0; i < 16; ++i) out[i] = src[i];
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// === One-shot validity sniff ===============================================
//
// First time tick() runs with overlay enabled, we read both matrices and
// log shape diagnostics so the user knows the projection capture is sane:
//   - view: orthonormal-ish rotation in upper 3x3, translation in row 3.
//   - proj: perspective shape (m11/m00 = aspect ratio, m22 ~ far/(far-near),
//           m32 = 1, m33 = 0 for D3D9 RH).
// Bad shapes here suggest 0x745AA0 isn't the right global.
std::atomic<bool> g_logged_shapes{false};

void log_matrix_shapes_once(const float view[16], const float proj[16]) {
    if (g_logged_shapes.exchange(true)) return;
    mtr::log::info("trigger_overlay: view m[0]=%.4f m[5]=%.4f m[10]=%.4f "
                   "trans=(%.2f, %.2f, %.2f)",
                   view[0], view[5], view[10],
                   view[12], view[13], view[14]);
    mtr::log::info("trigger_overlay: proj m[0]=%.4f m[5]=%.4f m[10]=%.4f "
                   "m[14]=%.4f m[11]=%.4f (D3D9 RH expects m[11]=1)",
                   proj[0], proj[5], proj[10], proj[14], proj[11]);
}

} // namespace

// === Public API ============================================================

bool enabled()                    { return g_enabled.load(std::memory_order_acquire); }
void set_enabled(bool v)          { g_enabled.store(v, std::memory_order_release);
                                    mtr::log::info("trigger_overlay: enabled=%d", v ? 1 : 0); }

int  visible_box_count()          { return g_last_visible.load(std::memory_order_acquire); }

bool show_test_box()              { return g_show_test_box.load(std::memory_order_acquire); }
void set_show_test_box(bool v)    { g_show_test_box.store(v, std::memory_order_release);
                                    mtr::log::info("trigger_overlay: test_box=%d", v ? 1 : 0); }

void set_export_frames(int n)     { if (n < 0) n = 0;
                                    g_export_frames.store(n, std::memory_order_release);
                                    mtr::log::info("trigger_overlay: export_frames=%d", n); }
int  export_frames_remaining()    { return g_export_frames.load(std::memory_order_acquire); }

void tick(IDirect3DDevice9* /*dev*/) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        g_last_visible.store(0, std::memory_order_release);
        return;
    }

    float view[16], proj[16];
    if (!read_view_matrix(view) || !read_proj_matrix(proj)) {
        g_last_visible.store(0, std::memory_order_release);
        return;
    }

    log_matrix_shapes_once(view, proj);

    float view_proj[16];
    mat_mul(view, proj, view_proj);

    // Viewport size from ImGui's IO display (the device's back-buffer size,
    // already kept in sync by ImGui's frame setup).
    const ImGuiIO& io = ImGui::GetIO();
    const float vp_w = io.DisplaySize.x;
    const float vp_h = io.DisplaySize.y;
    if (vp_w <= 0.0f || vp_h <= 0.0f) {
        g_last_visible.store(0, std::memory_order_release);
        return;
    }

    // Autonomous validation export: when armed, emit machine-parseable
    // log lines with the exact matrices + edge endpoints we just computed.
    // Validator reads these and re-does the math to assert correctness.
    const int export_remaining = g_export_frames.load(std::memory_order_acquire);
    const bool exporting = export_remaining > 0;
    const uint64_t frame_seq = g_frame_seq.fetch_add(1, std::memory_order_relaxed);

    if (exporting) {
        mtr::log::info("TRIGGER_OVERLAY_FRAME_BEGIN f=%llu vp=%.4f,%.4f",
                       (unsigned long long)frame_seq, vp_w, vp_h);
        mtr::log::info("TRIGGER_OVERLAY_VIEW f=%llu "
                       "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
                       "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
                       (unsigned long long)frame_seq,
                       view[0],  view[1],  view[2],  view[3],
                       view[4],  view[5],  view[6],  view[7],
                       view[8],  view[9],  view[10], view[11],
                       view[12], view[13], view[14], view[15]);
        mtr::log::info("TRIGGER_OVERLAY_PROJ f=%llu "
                       "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
                       "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
                       (unsigned long long)frame_seq,
                       proj[0],  proj[1],  proj[2],  proj[3],
                       proj[4],  proj[5],  proj[6],  proj[7],
                       proj[8],  proj[9],  proj[10], proj[11],
                       proj[12], proj[13], proj[14], proj[15]);
    }

    int visible_boxes = 0;
    int total_edges   = 0;

    // Phase 1 test box at world origin, extents (500, 500, 500), green.
    // Large extents so SOME face is in-frustum on most camera positions —
    // a (10,10,10) box at world origin is invisible from main-menu cameras
    // that frame the player elsewhere. The math validator runs on whatever
    // edges DO survive clipping, so the box size doesn't affect correctness;
    // larger just means the visual confirmation in screenshots is reliable.
    if (g_show_test_box.load(std::memory_order_acquire)) {
        const int edges = draw_aabb(0.0f, 0.0f, 0.0f, 500.0f, 500.0f, 500.0f,
                                    view_proj, vp_w, vp_h,
                                    IM_COL32(80, 220, 100, 230),
                                    exporting, frame_seq, /*box_idx=*/0);
        if (edges > 0) {
            ++visible_boxes;
            total_edges += edges;
        }
    }

    if (exporting) {
        mtr::log::info("TRIGGER_OVERLAY_FRAME_END f=%llu boxes=%d edges=%d",
                       (unsigned long long)frame_seq, visible_boxes, total_edges);
        g_export_frames.fetch_sub(1, std::memory_order_acq_rel);
    }

    g_last_visible.store(visible_boxes, std::memory_order_release);
}

} // namespace mtr::trigger_overlay
