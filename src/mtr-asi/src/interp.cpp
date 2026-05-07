// Render interpolation snapshot infrastructure (M2 of high-fps decouple).
//
// Two snapshot slots, double-buffered:
//   - prev: the sim tick BEFORE current
//   - curr: the most recently completed sim tick
//
// Capture flow (when throttle is engaged):
//   1. hk_sim_aggregator (in sim_decouple.cpp) calls on_sim_tick_post()
//      after the orig sim runs.
//   2. on_sim_tick_post() marks g_curr_dirty = true.
//   3. hk_camera_apply_all_active POST: if dirty, shift prev <- curr,
//      capture new curr from globals 0x724C10/0x724C50, clear dirty.
//   4. Subsequent render frames between sim ticks see prev/curr unchanged
//      and alpha grows from 0.0 to 1.0 across the sim_step window.
//
// When sim is skipped (throttle-decimated), step 1 doesn't fire, so
// step 3 is a no-op for those frames — curr stays at the last actual
// sim's matrix, alpha grows past 1.0 but is clamped.
//
// Cut detection: each new curr is compared to prev (translation column
// + view-axis rotation). Large delta → mark this render frame's
// is_cut_detected() = true so M3 clients can skip interp.

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "mtr/interp.h"
#include "mtr/sim_decouple.h"

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::interp {

namespace {

// Engine VAs ---------------------------------------------------------------

constexpr uintptr_t kCameraApplyAllActiveVA = 0x004C1E40;
constexpr uintptr_t kViewMatrixGlobalVA     = 0x00724C10;  // 16 floats
constexpr uintptr_t kWorldMatrixGlobalVA    = 0x00724C50;  // 16 floats
constexpr uintptr_t kEntityLookupByNameRetryVA = 0x005AC8F0;  // entity_lookup_by_name_retry(name, retry_count)

// Engine's per-frame transform list head. Walked by entity_transform_tick.
// Non-encrypted linked list; node layout (verified via 0x4B9F60):
//   node + 0x04  next ptr
//   node + 0x0C  timer (float; if non-zero, ticks down each sim — node will dispose when expired)
//   node + 0x40  entity ptr (used for entity-class metadata reads)
//   node + 0x44  flags (bit 0x10 = "skip transform this tick")
//   node + 0x5C  inner-transform ptr where pos (+0x58) + rot 3x3 (+0x70) live
constexpr uintptr_t kTransformListHeadVA   = 0x00724DE4;

// Player + NPC entity layout (verified via ai_resolve_nav_vector + entity_transform_tick):
//   +0x58 .. +0x60  (12 bytes)  pos.x, pos.y, pos.z
//   +0x70 .. +0x93  (36 bytes)  rotation 3x3 (row-major float[9])
constexpr uintptr_t kPlayerPosOffset = 0x58;
constexpr uintptr_t kPlayerRotOffset = 0x70;
constexpr size_t    kPlayerPosBytes  = 12;
constexpr size_t    kPlayerRotBytes  = 36;

// Hook ----------------------------------------------------------------------

using PFN_CameraApplyAll = char (__cdecl*)();   // no args, returns char (decompile shows char return)
PFN_CameraApplyAll g_orig_camera_apply = nullptr;

// Snapshot state -----------------------------------------------------------

Snapshot g_prev{};
Snapshot g_curr{};

// Set by on_sim_tick_post(); cleared after the next camera_apply POST
// captures the new curr. Atomic so the sim-thread → render-thread
// hand-off is well-defined (in this engine both run on the main thread,
// but cheap atomic insurance.)
std::atomic<bool> g_curr_dirty{false};

// Cumulative counters for the UI.
std::atomic<uint64_t> g_snapshots_taken{0};
std::atomic<uint64_t> g_cuts_detected{0};

// Cut detection state. Persists across render frames so sub-frame
// clients can read "cut on this frame" reliably until the next sim tick
// resets it.
std::atomic<bool> g_cut_for_curr{false};

// Live-tunable thresholds.
std::atomic<float> g_cut_translation{5.0f};
std::atomic<float> g_cut_rotation_deg{30.0f};

// View interp toggle (M3.1) and write counter.
std::atomic<bool>     g_view_interp_enabled{false};
std::atomic<uint64_t> g_view_interp_writes{0};

// M3.2 — aim-snap (input-bound).
std::atomic<bool> g_aim_snap_enabled{true};
std::atomic<int>  g_aim_snap_vk{0x02 /* VK_RBUTTON */};
std::atomic<bool> g_aim_snap_active{false};

// Forward declarations: helpers that the M4 code below uses but that
// are defined further down the file.
uint64_t qpc_now();
void     ensure_qpc_freq();

// Quaternion type used by both M3.1 view interp and M4 player interp.
// Definition must precede all consumers since Quat is returned by value.
struct Quat { float x, y, z, w; };
Quat mat3x3_to_quat(const float* m);
void quat_to_mat3x3(const Quat& q, float* m_out);
Quat quat_slerp(Quat q0, Quat q1, float t);

// Player interp (M4) state -------------------------------------------------

using PFN_EntityLookup = void* (__stdcall*)(const char* name, int retry);
const auto g_entity_lookup =
    reinterpret_cast<PFN_EntityLookup>(kEntityLookupByNameRetryVA);

struct PlayerSnap {
    float    pos[3];
    float    rot[9];
    uint64_t qpc;
    bool     valid;
};

PlayerSnap g_prev_player{};
PlayerSnap g_curr_player{};

// Pre-interp save: what was in entity BEFORE we wrote interp this render
// frame. PRE-sim hook restores from here so sim sees clean state.
PlayerSnap g_saved_player{};

// Cached player pointer + invalidation. Refreshed every kPlayerHandleRefreshFrames
// frames or whenever the cached pointer reads back as null. The cache lives in
// the render thread (camera_apply hook); the read from sim_aggregator hook
// uses the same global. Single-thread engine, atomic for safety.
std::atomic<void*> g_player_handle{nullptr};
std::atomic<int>   g_player_handle_age{0};
constexpr int      kPlayerHandleRefreshFrames = 60;

std::atomic<bool>     g_player_interp_enabled{false};
std::atomic<uint64_t> g_player_interp_writes{0};
std::atomic<bool>     g_player_save_valid{false};
std::atomic<bool>     g_player_teleport_for_curr{false};
std::atomic<float>    g_player_teleport_threshold{10.0f};
std::atomic<uint64_t> g_player_teleports{0};

// Read the cached player pointer; refresh on staleness or null. Returns
// nullptr if the engine doesn't have a player entity yet (early startup,
// between level loads).
void* get_player() {
    void* p = g_player_handle.load(std::memory_order_acquire);
    const int age = g_player_handle_age.fetch_add(1, std::memory_order_relaxed);
    if (!p || age >= kPlayerHandleRefreshFrames) {
        // The engine's entity registry isn't initialized until well after
        // our hooks are live (the very first sim tick fires during early
        // boot, before the actor world or registry exists). Wrap the call
        // so a null-deref inside the lookup function bottoms out at
        // fresh=nullptr rather than crashing the game.
        void* fresh = nullptr;
        __try {
            fresh = g_entity_lookup("player", 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fresh = nullptr;
        }
        g_player_handle.store(fresh, std::memory_order_release);
        g_player_handle_age.store(0, std::memory_order_relaxed);
        return fresh;
    }
    return p;
}

void capture_player_into(PlayerSnap& dst, const void* entity) {
    const auto* base = static_cast<const uint8_t*>(entity);
    std::memcpy(dst.pos, base + kPlayerPosOffset, kPlayerPosBytes);
    std::memcpy(dst.rot, base + kPlayerRotOffset, kPlayerRotBytes);
    dst.qpc   = qpc_now();
    dst.valid = true;
}

void write_player_from(const PlayerSnap& src, void* entity) {
    auto* base = static_cast<uint8_t*>(entity);
    std::memcpy(base + kPlayerPosOffset, src.pos, kPlayerPosBytes);
    std::memcpy(base + kPlayerRotOffset, src.rot, kPlayerRotBytes);
}

// NPC transform interp (M5) state ------------------------------------------

constexpr size_t kMaxNpcSlots             = 64;
constexpr int    kStaleSlotsAgeoutFrames  = 6;   // age out slots not seen for N sim ticks

struct NpcSlot {
    void*    inner;        // key: *(node+0x5C); zero means slot empty
    PlayerSnap prev;
    PlayerSnap curr;
    PlayerSnap saved;      // pre-interp state (for fence)
    bool     save_valid;
    bool     teleport;
    int      last_seen_tick;
};

NpcSlot g_npc_slots[kMaxNpcSlots]{};
std::atomic<int>      g_npc_tick_counter{0};
std::atomic<bool>     g_npc_interp_enabled{false};
std::atomic<uint64_t> g_npc_interp_writes{0};
std::atomic<uint64_t> g_npc_teleports{0};

// Find or allocate a slot for an inner-transform pointer. Linear-probe a
// hash bucket. Returns nullptr if all slots are taken (cap hit).
NpcSlot* find_or_alloc_npc(void* inner) {
    if (!inner) return nullptr;
    const uintptr_t k = reinterpret_cast<uintptr_t>(inner);
    const size_t base = (k >> 4) & (kMaxNpcSlots - 1);
    // First pass: existing match.
    for (size_t i = 0; i < kMaxNpcSlots; ++i) {
        const size_t idx = (base + i) & (kMaxNpcSlots - 1);
        if (g_npc_slots[idx].inner == inner) return &g_npc_slots[idx];
        if (!g_npc_slots[idx].inner) {
            g_npc_slots[idx] = NpcSlot{};
            g_npc_slots[idx].inner = inner;
            return &g_npc_slots[idx];
        }
    }
    return nullptr;  // table full
}

NpcSlot* find_npc(void* inner) {
    if (!inner) return nullptr;
    const uintptr_t k = reinterpret_cast<uintptr_t>(inner);
    const size_t base = (k >> 4) & (kMaxNpcSlots - 1);
    for (size_t i = 0; i < kMaxNpcSlots; ++i) {
        const size_t idx = (base + i) & (kMaxNpcSlots - 1);
        if (g_npc_slots[idx].inner == inner) return &g_npc_slots[idx];
        if (!g_npc_slots[idx].inner) return nullptr;
    }
    return nullptr;
}

void age_out_npc_slots(int current_tick) {
    for (size_t i = 0; i < kMaxNpcSlots; ++i) {
        if (g_npc_slots[i].inner
            && current_tick - g_npc_slots[i].last_seen_tick > kStaleSlotsAgeoutFrames) {
            g_npc_slots[i] = NpcSlot{};
        }
    }
}

// Walk the engine's transform list, calling cb(node) for every node we
// should consider. Skips:
//   - nodes with the +0x44 0x10 "skip" flag set
//   - the player's node (so we don't double-snap with M4)
//   - nodes with NULL inner ptr
template <typename Fn>
void walk_transform_list(Fn cb) {
    auto* node = *reinterpret_cast<uint8_t**>(kTransformListHeadVA);
    void* player = g_player_handle.load(std::memory_order_acquire);
    int safety = 8192;  // hard ceiling against pathological list cycles
    while (node && safety-- > 0) {
        const uint8_t flags  = *(node + 0x44);
        uint8_t* next        = *reinterpret_cast<uint8_t**>(node + 0x04);
        if ((flags & 0x10) == 0) {
            void* inner  = *reinterpret_cast<void**>(node + 0x5C);
            void* entity = *reinterpret_cast<void**>(node + 0x40);
            if (inner && entity != player) {
                cb(node, inner);
            }
        }
        node = next;
    }
}

// Compose lerp(pos) + slerp(rot 3x3) into out. out's rot[9] is row-major
// float[9] matching the engine's layout.
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

// QPC plumbing -------------------------------------------------------------

uint64_t g_qpc_freq = 0;

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

// Snapshot capture ---------------------------------------------------------

void copy_globals_to(Snapshot& dst) {
    std::memcpy(dst.view,  reinterpret_cast<const void*>(kViewMatrixGlobalVA),  sizeof(dst.view));
    std::memcpy(dst.world, reinterpret_cast<const void*>(kWorldMatrixGlobalVA), sizeof(dst.world));
    dst.qpc   = qpc_now();
    dst.valid = true;
}

// Cut detection: translation = view matrix's row 3 cols 0..2 (D3DMATRIX
// row-major: m[3][0], m[3][1], m[3][2] is the translation). Rotation
// proxy = dot product of forward axes (view m[2][0..2]); converted to
// angle via acos.
bool detect_cut(const Snapshot& prev, const Snapshot& curr) {
    const float dx = curr.view[12] - prev.view[12];
    const float dy = curr.view[13] - prev.view[13];
    const float dz = curr.view[14] - prev.view[14];
    const float trans2 = dx*dx + dy*dy + dz*dz;
    const float t_thresh = g_cut_translation.load(std::memory_order_relaxed);
    if (trans2 > t_thresh * t_thresh) return true;

    // Forward-axis dot. View matrix forward is row 2 in row-major D3D
    // (m[2][0..2]). Indices into 16-float row-major: [8], [9], [10].
    const float fx0 = prev.view[8],  fy0 = prev.view[9],  fz0 = prev.view[10];
    const float fx1 = curr.view[8],  fy1 = curr.view[9],  fz1 = curr.view[10];
    const float dot = fx0*fx1 + fy0*fy1 + fz0*fz1;
    const float clamped = (dot > 1.0f) ? 1.0f : ((dot < -1.0f) ? -1.0f : dot);
    const float angle_rad = std::acos(clamped);
    const float r_thresh_rad = g_cut_rotation_deg.load(std::memory_order_relaxed) * 0.01745329252f;
    if (angle_rad > r_thresh_rad) return true;

    return false;
}

// Quaternion utilities for view-interp slerp (M3.1) ----------------------
//
// Wilbur stores view + world matrices in D3DMATRIX row-major float[16].
// Rotation occupies the top-left 3x3 — indices 0,1,2,4,5,6,8,9,10
// (skipping translation column at 3,7,11 and translation row at 12..15).
//
// Algorithm: Shepperd's method for matrix→quat (handles all sign cases
// without divide-by-near-zero). Then slerp(q0, q1, alpha) via standard
// shortest-path + sin-division formula. Then quat→matrix to recompose.
//
// For the translation row [12..14] we just lerp linearly. Translation
// is ALWAYS lerp-correct under interpolation; only rotations need slerp.
// (Quat type forward-declared at top of namespace; definitions follow.)

Quat mat3x3_to_quat(const float* m) {
    // m is row-major float[16]; rotation is at indices 0,1,2 (row 0),
    // 4,5,6 (row 1), 8,9,10 (row 2). Read those into local 3x3 to
    // simplify indexing.
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
    // Writes rotation back into row-major 4x4 indices 0..2,4..6,8..10.
    // Caller must zero/set translation row + last column separately.
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

// Compose interpolated 4x4 from prev/curr at given alpha. Slerps top-3x3
// rotation; lerps translation row [12..14]; preserves bottom-right [3,7,11,15]
// from curr (these are typically [0,0,0,1] but we don't assume).
void compose_interp_matrix(const float* prev, const float* curr, float alpha, float* out) {
    const Quat q0 = mat3x3_to_quat(prev);
    const Quat q1 = mat3x3_to_quat(curr);
    const Quat qi = quat_slerp(q0, q1, alpha);
    quat_to_mat3x3(qi, out);

    // Lerp translation (row 3 in row-major = indices 12,13,14).
    out[12] = prev[12] + alpha * (curr[12] - prev[12]);
    out[13] = prev[13] + alpha * (curr[13] - prev[13]);
    out[14] = prev[14] + alpha * (curr[14] - prev[14]);

    // Pass-through for the right-hand column + bottom-right.
    out[3]  = curr[3];
    out[7]  = curr[7];
    out[11] = curr[11];
    out[15] = curr[15];
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

// Effective alpha that all interp consumers read. Returns 1.0 (snap to
// freshest sim) when aim-snap is active; otherwise the normal time-based
// alpha. Centralises the M3.2 logic so view + player + NPC writes pick
// it up uniformly.
float effective_alpha() {
    if (g_aim_snap_active.load(std::memory_order_relaxed)) return 1.0f;
    return current_alpha();
}

// Hook entry. camera_apply_all_active is __cdecl, no args. We can't take
// a pre-hook snapshot (that would be the previous frame's state, already
// in curr), so we hook POST: call orig, then capture if dirty.
char __cdecl hk_camera_apply_all_active() {
    refresh_aim_snap_input();
    char rc = g_orig_camera_apply();

    // Always-on capture: even when not dirty, the FIRST few frames need
    // a seed snapshot so prev exists by the time the second sim tick
    // fires. After both slots are valid, we only refresh on dirty.
    const bool dirty = g_curr_dirty.exchange(false, std::memory_order_acq_rel);

    if (dirty || !g_curr.valid) {
        // Shift curr -> prev (only if curr was valid).
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

    // M3.1 view-interp write. Runs unconditionally per render frame
    // (not just dirty frames) so skipped-sim frames between sim ticks
    // smoothly transition prev -> curr. Skipped when:
    //   - feature toggle is off (default)
    //   - we don't have two snapshots yet (first frame after install)
    //   - cut detected on this sim window
    //   - throttle isn't engaged (no point interpolating real-time sim)
    if (g_view_interp_enabled.load(std::memory_order_relaxed)
        && g_prev.valid && g_curr.valid
        && !g_cut_for_curr.load(std::memory_order_relaxed)
        && mtr::sim_decouple::effective_is_throttling()) {
        const float alpha = effective_alpha();
        float out_view [16];
        float out_world[16];
        compose_interp_matrix(g_prev.view,  g_curr.view,  alpha, out_view);
        compose_interp_matrix(g_prev.world, g_curr.world, alpha, out_world);
        std::memcpy(reinterpret_cast<void*>(kViewMatrixGlobalVA),  out_view,  sizeof(out_view));
        std::memcpy(reinterpret_cast<void*>(kWorldMatrixGlobalVA), out_world, sizeof(out_world));
        g_view_interp_writes.fetch_add(1, std::memory_order_relaxed);
    }

    // M4 player-interp write with save-write-restore fence. The save is
    // here (POST camera_apply, the spot where sim's freshest entity state
    // is also our curr_player). The restore is in pre_sim_restore_player()
    // called from sim_decouple's hk_sim_aggregator PRE.
    if (g_player_interp_enabled.load(std::memory_order_relaxed)
        && g_prev_player.valid && g_curr_player.valid
        && !g_player_teleport_for_curr.load(std::memory_order_relaxed)
        && mtr::sim_decouple::effective_is_throttling()) {
        void* p = g_player_handle.load(std::memory_order_acquire);
        if (p) {
            // Save BEFORE writing so the next pre-sim restore returns the
            // entity to the "real" sim_curr state, not our last interp.
            capture_player_into(g_saved_player, p);
            g_player_save_valid.store(true, std::memory_order_release);

            const float alpha = effective_alpha();
            PlayerSnap out;
            compose_player_interp(g_prev_player, g_curr_player, alpha, out);
            write_player_from(out, p);
            g_player_interp_writes.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // M5 NPC write. Walk the same transform list we captured earlier, find
    // each entity's slot, and apply lerp+slerp. Each slot saves its
    // pre-interp state so pre_sim_restore_npcs() can restore it.
    if (g_npc_interp_enabled.load(std::memory_order_relaxed)
        && mtr::sim_decouple::effective_is_throttling()) {
        const float alpha = effective_alpha();
        for (size_t i = 0; i < kMaxNpcSlots; ++i) {
            NpcSlot& s = g_npc_slots[i];
            if (!s.inner || !s.prev.valid || !s.curr.valid || s.teleport) continue;
            __try {
                // Save before writing.
                auto* base = static_cast<uint8_t*>(s.inner);
                std::memcpy(s.saved.pos, base + kPlayerPosOffset, kPlayerPosBytes);
                std::memcpy(s.saved.rot, base + kPlayerRotOffset, kPlayerRotBytes);
                s.save_valid = true;

                PlayerSnap out;
                compose_player_interp(s.prev, s.curr, alpha, out);
                std::memcpy(base + kPlayerPosOffset, out.pos, kPlayerPosBytes);
                std::memcpy(base + kPlayerRotOffset, out.rot, kPlayerRotBytes);
                g_npc_interp_writes.fetch_add(1, std::memory_order_relaxed);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                s = NpcSlot{};
            }
        }
    }

    return rc;
}

} // namespace

void install() {
    ensure_qpc_freq();
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
}

void on_sim_tick_post() {
    g_curr_dirty.store(true, std::memory_order_release);
}

// M4 hook entry points called from sim_decouple's hk_sim_aggregator.
// PRE-orig: restore. POST-orig: snapshot.

void pre_sim_restore_player() {
    if (!g_player_save_valid.load(std::memory_order_acquire)) return;
    void* p = g_player_handle.load(std::memory_order_acquire);
    if (!p) {
        g_player_save_valid.store(false, std::memory_order_release);
        return;
    }
    write_player_from(g_saved_player, p);
    g_player_save_valid.store(false, std::memory_order_release);
}

// M5 hook entry points (called from sim_decouple's hk_sim_aggregator).

void pre_sim_restore_npcs() {
    if (!g_npc_interp_enabled.load(std::memory_order_relaxed)) return;
    for (size_t i = 0; i < kMaxNpcSlots; ++i) {
        NpcSlot& s = g_npc_slots[i];
        if (!s.inner || !s.save_valid) continue;
        // Surround the entity-memory writes with SEH so a stale slot
        // (entity already freed and our age-out hasn't fired yet) can't
        // crash the game. Cost: __try is essentially free on x86.
        __try {
            auto* base = static_cast<uint8_t*>(s.inner);
            std::memcpy(base + kPlayerPosOffset, s.saved.pos, kPlayerPosBytes);
            std::memcpy(base + kPlayerRotOffset, s.saved.rot, kPlayerRotBytes);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Slot points at freed memory; clear it so we don't keep
            // touching the dangling address.
            s = NpcSlot{};
            continue;
        }
        s.save_valid = false;
    }
}

void post_sim_capture_npcs() {
    if (!g_npc_interp_enabled.load(std::memory_order_relaxed)) return;
    const int tick = g_npc_tick_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    walk_transform_list([&](uint8_t* /*node*/, void* inner) {
        NpcSlot* s = find_or_alloc_npc(inner);
        if (!s) return;
        s->last_seen_tick = tick;
        if (s->curr.valid) {
            s->prev = s->curr;
        }
        // Snapshot from the entity's actual memory.
        __try {
            const auto* base = static_cast<const uint8_t*>(inner);
            std::memcpy(s->curr.pos, base + kPlayerPosOffset, kPlayerPosBytes);
            std::memcpy(s->curr.rot, base + kPlayerRotOffset, kPlayerRotBytes);
            s->curr.qpc   = qpc_now();
            s->curr.valid = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *s = NpcSlot{};
            return;
        }
        // Per-entity teleport: same threshold as player (uses g_player_teleport_threshold).
        if (s->prev.valid && s->curr.valid) {
            const float dx = s->curr.pos[0] - s->prev.pos[0];
            const float dy = s->curr.pos[1] - s->prev.pos[1];
            const float dz = s->curr.pos[2] - s->prev.pos[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            const float t  = g_player_teleport_threshold.load(std::memory_order_relaxed);
            const bool tp  = d2 > t * t;
            s->teleport = tp;
            if (tp) g_npc_teleports.fetch_add(1, std::memory_order_relaxed);
        }
    });
    age_out_npc_slots(tick);
}

void post_sim_capture_player() {
    // Gate on the toggle so a crash-on-boot path can't fire before any
    // user opt-in. Matches how post_sim_capture_npcs gates on its toggle.
    // Without this gate the unconditional g_entity_lookup call below
    // fires from the very first sim tick during engine init, before the
    // actor world is initialized — which AVs out of the lookup machinery.
    if (!g_player_interp_enabled.load(std::memory_order_relaxed)) return;
    void* p = get_player();
    if (!p) return;
    __try {
        if (g_curr_player.valid) {
            g_prev_player = g_curr_player;
        }
        capture_player_into(g_curr_player, p);

        // Teleport detect: |pos delta| > threshold -> snap, no interp this window.
        if (g_prev_player.valid && g_curr_player.valid) {
            const float dx = g_curr_player.pos[0] - g_prev_player.pos[0];
            const float dy = g_curr_player.pos[1] - g_prev_player.pos[1];
            const float dz = g_curr_player.pos[2] - g_prev_player.pos[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            const float t  = g_player_teleport_threshold.load(std::memory_order_relaxed);
            const bool tp  = d2 > t * t;
            g_player_teleport_for_curr.store(tp, std::memory_order_relaxed);
            if (tp) {
                g_player_teleports.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Player handle stale or entity freed mid-capture. Drop the
        // cached pointer + invalidate snapshots so the next call
        // re-lookups cleanly.
        g_player_handle.store(nullptr, std::memory_order_release);
        g_curr_player.valid = false;
        g_prev_player.valid = false;
    }
}

const Snapshot& prev() { return g_prev; }
const Snapshot& curr() { return g_curr; }

float current_alpha() {
    if (!g_curr.valid) return 1.0f;
    ensure_qpc_freq();
    if (!g_qpc_freq) return 1.0f;
    const int hz = mtr::sim_decouple::target_hz();
    if (hz <= 0) return 1.0f;
    const uint64_t step = g_qpc_freq / static_cast<uint64_t>(hz);
    if (!step) return 1.0f;
    const uint64_t now = qpc_now();
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

float cut_translation_threshold()         { return g_cut_translation.load(std::memory_order_relaxed); }
void  set_cut_translation_threshold(float v) {
    if (v < 0.0f) v = 0.0f;
    g_cut_translation.store(v, std::memory_order_relaxed);
}

float cut_rotation_threshold_deg()        { return g_cut_rotation_deg.load(std::memory_order_relaxed); }
void  set_cut_rotation_threshold_deg(float deg) {
    if (deg < 0.0f) deg = 0.0f;
    if (deg > 180.0f) deg = 180.0f;
    g_cut_rotation_deg.store(deg, std::memory_order_relaxed);
}

uint64_t snapshots_taken() { return g_snapshots_taken.load(std::memory_order_relaxed); }
uint64_t cuts_detected()   { return g_cuts_detected.load(std::memory_order_relaxed); }

bool view_interp_enabled() { return g_view_interp_enabled.load(std::memory_order_relaxed); }
void set_view_interp_enabled(bool on) {
    const bool prev_v = g_view_interp_enabled.exchange(on, std::memory_order_relaxed);
    if (prev_v != on) {
        mtr::log::info("interp: view_interp %s", on ? "enabled" : "disabled");
    }
}
uint64_t view_interp_writes() { return g_view_interp_writes.load(std::memory_order_relaxed); }

// M3.2 -----------------------------------------------------------------

bool aim_snap_enabled() { return g_aim_snap_enabled.load(std::memory_order_relaxed); }
void set_aim_snap_enabled(bool on) {
    g_aim_snap_enabled.store(on, std::memory_order_relaxed);
}
int  aim_snap_vk() { return g_aim_snap_vk.load(std::memory_order_relaxed); }
void set_aim_snap_vk(int vk) {
    g_aim_snap_vk.store(vk, std::memory_order_relaxed);
}
bool aim_snap_active() { return g_aim_snap_active.load(std::memory_order_relaxed); }

// M4 -----------------------------------------------------------------------

bool player_interp_enabled() { return g_player_interp_enabled.load(std::memory_order_relaxed); }
void set_player_interp_enabled(bool on) {
    const bool prev_v = g_player_interp_enabled.exchange(on, std::memory_order_relaxed);
    if (prev_v != on) {
        mtr::log::info("interp: player_interp %s", on ? "enabled" : "disabled");
    }
    if (!on) {
        // Make sure any in-flight save gets flushed back to the entity so
        // toggling off mid-frame doesn't leave the entity stuck on an
        // interp value.
        pre_sim_restore_player();
    }
}
uint64_t player_interp_writes() { return g_player_interp_writes.load(std::memory_order_relaxed); }

float player_teleport_threshold() {
    return g_player_teleport_threshold.load(std::memory_order_relaxed);
}
void set_player_teleport_threshold(float v) {
    if (v < 0.0f) v = 0.0f;
    g_player_teleport_threshold.store(v, std::memory_order_relaxed);
}
uint64_t player_teleports_detected() {
    return g_player_teleports.load(std::memory_order_relaxed);
}

// M5 -----------------------------------------------------------------------

bool npc_interp_enabled() { return g_npc_interp_enabled.load(std::memory_order_relaxed); }
void set_npc_interp_enabled(bool on) {
    const bool prev_v = g_npc_interp_enabled.exchange(on, std::memory_order_relaxed);
    if (prev_v != on) {
        mtr::log::info("interp: npc_interp %s", on ? "enabled" : "disabled");
    }
    if (!on) {
        // Flush any in-flight saves so toggling off cleanly returns
        // every entity to actual sim state.
        pre_sim_restore_npcs();
        // Then clear the slot table so a future re-enable starts fresh.
        for (size_t i = 0; i < kMaxNpcSlots; ++i) g_npc_slots[i] = NpcSlot{};
    }
}
uint64_t npc_interp_writes() { return g_npc_interp_writes.load(std::memory_order_relaxed); }
uint64_t npc_active_slots()  {
    uint64_t n = 0;
    for (size_t i = 0; i < kMaxNpcSlots; ++i) if (g_npc_slots[i].inner) ++n;
    return n;
}
uint64_t npc_teleports_detected() { return g_npc_teleports.load(std::memory_order_relaxed); }

} // namespace mtr::interp
