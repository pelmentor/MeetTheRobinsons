// Render interpolation orchestrator.
//
// This TU owns:
//   - Snapshot infrastructure (g_prev, g_curr double-buffer + dirty flag).
//   - The camera_apply_all_active POST hook entry point.
//   - Cut detection.
//   - Aim-snap state + effective_alpha() resolver.
//   - Math helpers (Quat / slerp / mat3x3 / compose_*) used by the split
//     view / player / npc TUs under interp/.
//   - QPC plumbing.
//
// Per-render-frame, hk_camera_apply_all_active POST:
//   1. dt-correctness write_for_render_frame (real-time dt for render-path).
//   2. refresh aim-snap input.
//   3. orig camera_apply.
//   4. shift prev <- curr if dirty (fresh sim landed); capture new curr.
//   5. cut detect + counter increment.
//   6. dispatch view / player / npc apply functions.
//
// The split TUs each own their own enabled flag + state; the dispatch
// calls are no-ops when off.
//
// Capture flow timing:
//   - hk_sim_aggregator (sim_decouple.cpp) calls on_sim_tick_post() AFTER
//     the orig sim runs.
//   - on_sim_tick_post() marks g_curr_dirty = true.
//   - Next camera_apply POST shifts prev <- curr, captures new curr from
//     globals 0x724C10/0x724C50, clears dirty.
//   - Subsequent render frames between sim ticks see prev/curr unchanged
//     and alpha grows from 0.0 to 1.0 across the sim_step window.
//
// When sim is throttle-skipped, on_sim_tick_post doesn't fire, so
// step 4 is a no-op for those frames — curr stays at the last actual
// sim's matrix, alpha grows past 1.0 but is clamped.

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mtr/interp.h"
#include "mtr/sim_decouple.h"
#include "mtr/dt_correctness.h"
#include "interp/interp_internal.h"

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::interp {

namespace {

// === Engine VAs (orchestrator-only) ======================================

constexpr uintptr_t kCameraApplyAllActiveVA = 0x004C1E40;

// === Hook ================================================================

using PFN_CameraApplyAll = char (__cdecl*)();   // no args, returns char (decompile shows char return)
PFN_CameraApplyAll g_orig_camera_apply = nullptr;

// === Snapshot state ======================================================

Snapshot g_prev{};
Snapshot g_curr{};

// Set by on_sim_tick_post(); cleared after the next camera_apply POST
// captures the new curr. Atomic so the sim-thread → render-thread
// hand-off is well-defined (in this engine both run on the main thread,
// but cheap atomic insurance).
std::atomic<bool> g_curr_dirty{false};

std::atomic<uint64_t> g_snapshots_taken{0};
std::atomic<uint64_t> g_cuts_detected{0};

// Cut detection state. Persists across render frames so sub-frame
// clients can read "cut on this frame" reliably until the next sim tick
// resets it.
std::atomic<bool> g_cut_for_curr{false};

// Live-tunable thresholds.
std::atomic<float> g_cut_translation{5.0f};
std::atomic<float> g_cut_rotation_deg{30.0f};

// === Aim-snap state (M3.2) ===============================================

std::atomic<bool> g_aim_snap_enabled{true};
std::atomic<int>  g_aim_snap_vk{0x02 /* VK_RBUTTON */};
std::atomic<bool> g_aim_snap_active{false};

// === QPC plumbing ========================================================

uint64_t g_qpc_freq = 0;

// === Helpers (detail::) ==================================================

// Cut detection: translation = view matrix's row 3 cols 0..2 (D3DMATRIX
// row-major: m[3][0..2] is the translation). Rotation proxy = dot product
// of forward axes (view m[2][0..2]); converted to angle via acos.
bool detect_cut(const Snapshot& prev, const Snapshot& curr) {
    const float dx = curr.view[12] - prev.view[12];
    const float dy = curr.view[13] - prev.view[13];
    const float dz = curr.view[14] - prev.view[14];
    const float trans2 = dx*dx + dy*dy + dz*dz;
    const float t_thresh = g_cut_translation.load(std::memory_order_relaxed);
    if (trans2 > t_thresh * t_thresh) return true;

    // Forward-axis dot. Indices into 16-float row-major: [8], [9], [10].
    const float fx0 = prev.view[8],  fy0 = prev.view[9],  fz0 = prev.view[10];
    const float fx1 = curr.view[8],  fy1 = curr.view[9],  fz1 = curr.view[10];
    const float dot = fx0*fx1 + fy0*fy1 + fz0*fz1;
    const float clamped = (dot > 1.0f) ? 1.0f : ((dot < -1.0f) ? -1.0f : dot);
    const float angle_rad = std::acos(clamped);
    const float r_thresh_rad = g_cut_rotation_deg.load(std::memory_order_relaxed) * 0.01745329252f;
    if (angle_rad > r_thresh_rad) return true;

    return false;
}

void copy_globals_to(Snapshot& dst) {
    std::memcpy(dst.view,
                reinterpret_cast<const void*>(detail::kViewMatrixGlobalVA),
                sizeof(dst.view));
    std::memcpy(dst.world,
                reinterpret_cast<const void*>(detail::kWorldMatrixGlobalVA),
                sizeof(dst.world));
    dst.qpc   = detail::qpc_now();
    dst.valid = true;
}

// Refresh M3.2 aim-snap state once per camera_apply call. Polling
// GetAsyncKeyState is a single syscall on x86; doing it here keeps the
// state consistent across the interp writes within one render frame.
void refresh_aim_snap_input() {
    if (!g_aim_snap_enabled.load(std::memory_order_relaxed)) {
        g_aim_snap_active.store(false, std::memory_order_relaxed);
        return;
    }
    const int vk = g_aim_snap_vk.load(std::memory_order_relaxed);
    if (vk == 0) {
        g_aim_snap_active.store(false, std::memory_order_relaxed);
        return;
    }
    const SHORT s = GetAsyncKeyState(vk);
    g_aim_snap_active.store((s & 0x8000) != 0, std::memory_order_relaxed);
}

// === Hook entry ==========================================================

char __cdecl hk_camera_apply_all_active() {
    // dt-correctness: write flt_6FFCBC = real_render_dt before orig
    // runs, so render-path consumers (PathCam spring, UV scroll, HUD
    // tweens, screen shake, etc.) integrate at this render frame's
    // actual elapsed time. No-op when dt-correctness is toggled off.
    mtr::dt_correctness::write_for_render_frame();

    refresh_aim_snap_input();
    char rc = g_orig_camera_apply();

    // Always-on capture: even when not dirty, the FIRST few frames need
    // a seed snapshot so prev exists by the time the second sim tick
    // fires. After both slots are valid, refresh only on dirty.
    const bool dirty = g_curr_dirty.exchange(false, std::memory_order_acq_rel);

    if (dirty || !g_curr.valid) {
        if (g_curr.valid) {
            g_prev = g_curr;
        }
        copy_globals_to(g_curr);
        g_snapshots_taken.fetch_add(1, std::memory_order_relaxed);

        if (g_prev.valid && g_curr.valid) {
            const bool cut = detect_cut(g_prev, g_curr);
            g_cut_for_curr.store(cut, std::memory_order_relaxed);
            if (cut) {
                g_cuts_detected.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Per-render-frame interp dispatch. Each apply function gates on its
    // own enabled flag + sim-decouple throttle + cut/teleport state.
    const float alpha = detail::effective_alpha();
    detail::view_apply_interp_for_render_frame(alpha);
    detail::player_apply_interp_for_render_frame(alpha);
    detail::npc_apply_interp_for_render_frame(alpha);

    return rc;
}

} // namespace

// === Math helpers + qpc + effective_alpha (detail::) =====================
// Defined here because the orchestrator uses them, and the split TUs need
// them at link time. interp_internal.h declares them.

namespace detail {

uint64_t qpc_now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

void ensure_qpc_freq() {
    if (g_qpc_freq) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = static_cast<uint64_t>(f.QuadPart);
}

float effective_alpha() {
    if (g_aim_snap_active.load(std::memory_order_relaxed)) return 1.0f;
    return current_alpha();
}

// Quaternion utilities for view-interp slerp (M3.1) ----------------------
//
// View + world matrices live in D3DMATRIX row-major float[16]. Rotation
// occupies the top-left 3x3 — indices 0,1,2 (row 0), 4,5,6 (row 1),
// 8,9,10 (row 2), skipping translation column at 3,7,11 and translation
// row at 12..15.
//
// Algorithm: Shepperd's method for matrix→quat (handles all sign cases
// without divide-by-near-zero). Then slerp(q0, q1, alpha) via standard
// shortest-path + sin-division formula. Then quat→matrix to recompose.

Quat mat3x3_to_quat(const float* m) {
    const float m00 = m[0],  m01 = m[1],  m02 = m[2];
    const float m10 = m[4],  m11 = m[5],  m12 = m[6];
    const float m20 = m[8],  m21 = m[9],  m22 = m[10];

    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;  // s = 4w
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;  // s = 4x
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;  // s = 4y
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;  // s = 4z
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

void quat_to_mat3x3(const Quat& q, float* m_out) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    m_out[0]  = 1.0f - 2.0f * (yy + zz);
    m_out[1]  =        2.0f * (xy + wz);
    m_out[2]  =        2.0f * (xz - wy);

    m_out[4]  =        2.0f * (xy - wz);
    m_out[5]  = 1.0f - 2.0f * (xx + zz);
    m_out[6]  =        2.0f * (yz + wx);

    m_out[8]  =        2.0f * (xz + wy);
    m_out[9]  =        2.0f * (yz - wx);
    m_out[10] = 1.0f - 2.0f * (xx + yy);
}

Quat quat_slerp(Quat q0, Quat q1, float t) {
    // Shortest-path correction: ensure dot >= 0 by negating q1 if needed.
    float dot = q0.x*q1.x + q0.y*q1.y + q0.z*q1.z + q0.w*q1.w;
    if (dot < 0.0f) {
        q1.x = -q1.x; q1.y = -q1.y; q1.z = -q1.z; q1.w = -q1.w;
        dot = -dot;
    }
    // Near-identical rotations: fall back to nlerp (avoids sin(small)/
    // sin(small) numerical instability). The 0.9995 threshold matches
    // common implementations (Bullet, Eigen, Unity).
    if (dot > 0.9995f) {
        Quat r = {
            q0.x + t * (q1.x - q0.x),
            q0.y + t * (q1.y - q0.y),
            q0.z + t * (q1.z - q0.z),
            q0.w + t * (q1.w - q0.w),
        };
        const float n = std::sqrt(r.x*r.x + r.y*r.y + r.z*r.z + r.w*r.w);
        if (n > 0.0f) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
        return r;
    }
    const float theta_0 = std::acos(dot);
    const float theta   = theta_0 * t;
    const float sin_theta_0 = std::sin(theta_0);
    const float sin_theta   = std::sin(theta);
    const float s0 = std::cos(theta) - dot * sin_theta / sin_theta_0;
    const float s1 = sin_theta / sin_theta_0;
    return Quat {
        s0 * q0.x + s1 * q1.x,
        s0 * q0.y + s1 * q1.y,
        s0 * q0.z + s1 * q1.z,
        s0 * q0.w + s1 * q1.w,
    };
}

// Extract camera world position from a row-major D3D view matrix.
//
// View matrix V = T(-C) * R for row-vector convention. The translation
// row is V[12..14] = -R^T * C (where C is camera world position).
//   C.x = -(V[0]*V[12] + V[1]*V[13] + V[2]*V[14])
//   C.y = -(V[4]*V[12] + V[5]*V[13] + V[6]*V[14])
//   C.z = -(V[8]*V[12] + V[9]*V[13] + V[10]*V[14])
void extract_camera_world_pos(const float* V, float* C) {
    C[0] = -(V[0]*V[12] + V[1]*V[13] + V[2]*V[14]);
    C[1] = -(V[4]*V[12] + V[5]*V[13] + V[6]*V[14]);
    C[2] = -(V[8]*V[12] + V[9]*V[13] + V[10]*V[14]);
}

// Camera-world-space view interp (correct for view matrices).
//
// Direct lerp of view matrices is mathematically wrong because lerp(V0,
// V1) ≠ inv(lerp(camera0, camera1)). The translation column of a view
// matrix is -R^T * cam_pos, and lerping it independently of the rotation
// produces a translation that doesn't correspond to any physical camera
// position when R is interpolated.
//
//   1. Slerp the rotation top-3x3.
//   2. Extract cam_pos from each input view matrix.
//   3. Lerp cam_pos as a 3-vector.
//   4. Reconstruct translation row from interp_R + interp_cam_pos.
void compose_view_interp_camspace(const float* prev, const float* curr,
                                  float alpha, float* out) {
    const Quat q0 = mat3x3_to_quat(prev);
    const Quat q1 = mat3x3_to_quat(curr);
    const Quat qi = quat_slerp(q0, q1, alpha);
    quat_to_mat3x3(qi, out);

    float c_prev[3], c_curr[3];
    extract_camera_world_pos(prev, c_prev);
    extract_camera_world_pos(curr, c_curr);

    const float cx = c_prev[0] + alpha * (c_curr[0] - c_prev[0]);
    const float cy = c_prev[1] + alpha * (c_curr[1] - c_prev[1]);
    const float cz = c_prev[2] + alpha * (c_curr[2] - c_prev[2]);

    out[12] = -(cx*out[0] + cy*out[4] + cz*out[8]);
    out[13] = -(cx*out[1] + cy*out[5] + cz*out[9]);
    out[14] = -(cx*out[2] + cy*out[6] + cz*out[10]);

    // Pass-through right-hand column + bottom-right.
    out[3]  = curr[3];
    out[7]  = curr[7];
    out[11] = curr[11];
    out[15] = curr[15];
}

// General matrix interp (used for the world-matrix global, which isn't
// necessarily a view matrix). For the view matrix specifically use
// compose_view_interp_camspace which is mathematically correct.
void compose_interp_matrix(const float* prev, const float* curr, float alpha, float* out) {
    const Quat q0 = mat3x3_to_quat(prev);
    const Quat q1 = mat3x3_to_quat(curr);
    const Quat qi = quat_slerp(q0, q1, alpha);
    quat_to_mat3x3(qi, out);

    out[12] = prev[12] + alpha * (curr[12] - prev[12]);
    out[13] = prev[13] + alpha * (curr[13] - prev[13]);
    out[14] = prev[14] + alpha * (curr[14] - prev[14]);

    out[3]  = curr[3];
    out[7]  = curr[7];
    out[11] = curr[11];
    out[15] = curr[15];
}

// Compose lerp(pos) + slerp(rot 3x3) for a player-shaped snap. Used by
// interp_player.cpp + interp_npc.cpp.
void compose_player_interp(const PlayerSnap& a, const PlayerSnap& b,
                           float alpha, PlayerSnap& out) {
    out.pos[0] = a.pos[0] + alpha * (b.pos[0] - a.pos[0]);
    out.pos[1] = a.pos[1] + alpha * (b.pos[1] - a.pos[1]);
    out.pos[2] = a.pos[2] + alpha * (b.pos[2] - a.pos[2]);

    // Build temporary 4x4 layout for slerp (we already have mat3x3_to_quat
    // for that shape). Copy 3x3 into rows 0..2 of a 4x4 with 0/1 padding.
    float ma[16] = {
        a.rot[0], a.rot[1], a.rot[2], 0.0f,
        a.rot[3], a.rot[4], a.rot[5], 0.0f,
        a.rot[6], a.rot[7], a.rot[8], 0.0f,
        0.0f,     0.0f,     0.0f,     1.0f,
    };
    float mb[16] = {
        b.rot[0], b.rot[1], b.rot[2], 0.0f,
        b.rot[3], b.rot[4], b.rot[5], 0.0f,
        b.rot[6], b.rot[7], b.rot[8], 0.0f,
        0.0f,     0.0f,     0.0f,     1.0f,
    };
    const Quat q0 = mat3x3_to_quat(ma);
    const Quat q1 = mat3x3_to_quat(mb);
    const Quat qi = quat_slerp(q0, q1, alpha);
    float mout[16] = {0};
    quat_to_mat3x3(qi, mout);
    out.rot[0] = mout[0];  out.rot[1] = mout[1];  out.rot[2] = mout[2];
    out.rot[3] = mout[4];  out.rot[4] = mout[5];  out.rot[5] = mout[6];
    out.rot[6] = mout[8];  out.rot[7] = mout[9];  out.rot[8] = mout[10];
}

} // namespace detail

// === Public API ============================================================

void install() {
    detail::ensure_qpc_freq();
    void* va = reinterpret_cast<void*>(kCameraApplyAllActiveVA);
    if (MH_CreateHook(va, reinterpret_cast<void*>(&hk_camera_apply_all_active),
                      reinterpret_cast<void**>(&g_orig_camera_apply)) != MH_OK) {
        mtr::log::info("interp: MH_CreateHook(camera_apply_all_active @%p) failed", va);
        return;
    }
    if (MH_EnableHook(va) != MH_OK) {
        mtr::log::info("interp: MH_EnableHook(camera_apply_all_active) failed");
        return;
    }
    mtr::log::info("interp: hooked camera_apply_all_active at %p", va);

    // M3.3 — halo follow-fix hook. Self-gated; safe to install whether
    // or not view_interp ends up enabled.
    detail::halo_install();
}

void on_sim_tick_post() {
    g_curr_dirty.store(true, std::memory_order_release);
}

// === Cross-subsystem entity pose primitives =================================
//
// Public surface for coop/ — see mtr/interp.h. Layout of EntityPose matches
// detail::PlayerSnap, so the read/write helpers in interp_internal.h forward
// directly via reinterpret_cast. If either struct's layout changes, this
// static assert fires.

static_assert(sizeof(EntityPose) == sizeof(detail::PlayerSnap),
              "EntityPose / detail::PlayerSnap layouts diverged — update both");
static_assert(offsetof(EntityPose, pos)   == offsetof(detail::PlayerSnap, pos),   "");
static_assert(offsetof(EntityPose, rot)   == offsetof(detail::PlayerSnap, rot),   "");
static_assert(offsetof(EntityPose, qpc)   == offsetof(detail::PlayerSnap, qpc),   "");
static_assert(offsetof(EntityPose, valid) == offsetof(detail::PlayerSnap, valid), "");

uint64_t qpc_now() { return detail::qpc_now(); }

bool capture_entity_pose(const void* engine_entity, EntityPose& out) {
    if (!engine_entity) return false;
    __try {
        auto& as_snap = reinterpret_cast<detail::PlayerSnap&>(out);
        detail::capture_pos_rot(as_snap, engine_entity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void apply_entity_pose_interp(const EntityPose& prev, const EntityPose& curr,
                              float alpha, void* engine_entity) {
    if (!engine_entity || !curr.valid) return;
    const auto& a_snap = reinterpret_cast<const detail::PlayerSnap&>(prev);
    const auto& b_snap = reinterpret_cast<const detail::PlayerSnap&>(curr);
    detail::PlayerSnap out;
    if (!prev.valid) {
        out = b_snap;            // no prev yet: snap to curr.
    } else {
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        detail::compose_player_interp(a_snap, b_snap, alpha, out);
    }
    detail::write_pos_rot(out, engine_entity);
}

const Snapshot& prev() { return g_prev; }
const Snapshot& curr() { return g_curr; }

float current_alpha() {
    if (!g_curr.valid) return 1.0f;
    detail::ensure_qpc_freq();
    if (!g_qpc_freq) return 1.0f;
    const int hz = mtr::sim_decouple::target_hz();
    if (hz <= 0) return 1.0f;
    const uint64_t step = g_qpc_freq / static_cast<uint64_t>(hz);
    if (!step) return 1.0f;
    const uint64_t now = detail::qpc_now();
    if (now <= g_curr.qpc) return 0.0f;
    const double a = static_cast<double>(now - g_curr.qpc) / static_cast<double>(step);
    if (a >= 1.0) return 1.0f;
    if (a <= 0.0) return 0.0f;
    return static_cast<float>(a);
}

bool has_two_snapshots() {
    return g_prev.valid && g_curr.valid;
}

bool is_cut_detected() {
    return g_cut_for_curr.load(std::memory_order_relaxed);
}

float cut_translation_threshold() { return g_cut_translation.load(std::memory_order_relaxed); }
void  set_cut_translation_threshold(float v) {
    if (v < 0.0f) v = 0.0f;
    g_cut_translation.store(v, std::memory_order_relaxed);
}

float cut_rotation_threshold_deg() { return g_cut_rotation_deg.load(std::memory_order_relaxed); }
void  set_cut_rotation_threshold_deg(float deg) {
    if (deg < 0.0f) deg = 0.0f;
    if (deg > 180.0f) deg = 180.0f;
    g_cut_rotation_deg.store(deg, std::memory_order_relaxed);
}

uint64_t snapshots_taken() { return g_snapshots_taken.load(std::memory_order_relaxed); }
uint64_t cuts_detected()   { return g_cuts_detected.load(std::memory_order_relaxed); }

// === Aim-snap (M3.2) =======================================================

bool aim_snap_enabled() { return g_aim_snap_enabled.load(std::memory_order_relaxed); }
void set_aim_snap_enabled(bool on) {
    g_aim_snap_enabled.store(on, std::memory_order_relaxed);
}
int  aim_snap_vk() { return g_aim_snap_vk.load(std::memory_order_relaxed); }
void set_aim_snap_vk(int vk) {
    g_aim_snap_vk.store(vk, std::memory_order_relaxed);
}
bool aim_snap_active() { return g_aim_snap_active.load(std::memory_order_relaxed); }

} // namespace mtr::interp
