// Private header for the interp module's split TUs.
//
// interp.cpp owns the Snapshot infra + camera_apply hook + math helpers +
// aim-snap state. It dispatches per-render-frame work to the three
// sibling TUs (interp_view / interp_player / interp_npc) via the
// detail::*_apply_interp_for_render_frame() functions declared below.
//
// Each apply function reads its own enabled flag and bails out fast when
// off, so the orchestrator can call all three unconditionally.
//
// Lives in src/, intentionally not in include/, because it is internal to
// the interp module's TUs.

#pragma once

#include <cstdint>
#include <cstring>

namespace mtr::interp::detail {

// === Engine VAs that span TUs =============================================

constexpr uintptr_t kViewMatrixGlobalVA  = 0x00724C10;  // 16 floats
constexpr uintptr_t kWorldMatrixGlobalVA = 0x00724C50;  // 16 floats
constexpr uintptr_t kTransformListHeadVA = 0x00724DE4;  // singly-linked list head

// Player + NPC entity layout (verified via ai_resolve_nav_vector +
// entity_transform_tick): pos at +0x58 (12 bytes), rotation 3x3 at +0x70
// (36 bytes, row-major float[9]).
constexpr uintptr_t kPlayerPosOffset = 0x58;
constexpr uintptr_t kPlayerRotOffset = 0x70;
constexpr size_t    kPlayerPosBytes  = 12;
constexpr size_t    kPlayerRotBytes  = 36;

// === Per-entity snapshot =================================================
// Used by both M4 (player) and M5 (NPC) so the helpers below can target
// the same memcpy pattern.

struct PlayerSnap {
    float    pos[3];
    float    rot[9];
    uint64_t qpc;
    bool     valid;
};

// === Math helpers (defined in interp.cpp) ================================

struct Quat { float x, y, z, w; };

Quat mat3x3_to_quat(const float* m);
void quat_to_mat3x3(const Quat& q, float* m_out);
Quat quat_slerp(Quat q0, Quat q1, float t);

void extract_camera_world_pos(const float* V, float* C);
void compose_interp_matrix(const float* prev, const float* curr,
                           float alpha, float* out);
void compose_view_interp_camspace(const float* prev, const float* curr,
                                  float alpha, float* out);
void compose_player_interp(const PlayerSnap& a, const PlayerSnap& b,
                           float alpha, PlayerSnap& out);

// === QPC plumbing (defined in interp.cpp) ================================

uint64_t qpc_now();
void     ensure_qpc_freq();

// === Effective alpha (defined in interp.cpp) =============================
// Reads aim-snap state — when active, returns 1.0 (snap to freshest sim).
// Otherwise returns mtr::interp::current_alpha(). Centralises M3.2 logic
// so view + player + NPC writes all pick it up uniformly.

float effective_alpha();

// === Per-render-frame dispatch ===========================================
// Called from interp.cpp's hk_camera_apply_all_active POST. Each is a
// no-op when its enabled flag is off or other preconditions fail.

void view_apply_interp_for_render_frame(float alpha);
void player_apply_interp_for_render_frame(float alpha);
void npc_apply_interp_for_render_frame(float alpha);

// === Halo follow-fix install (M3.3) ======================================
// Installs the HaloComponent::Update PRE hook. Called once from
// interp.cpp's install(). The hook itself reads view_interp_enabled() and
// halo_interp_enabled() — the latter gates the override; when off the
// hook just trampolines straight to orig.

void halo_install();

// === Cross-TU player-handle accessor =====================================
// Owned by interp_player.cpp. interp_npc.cpp reads it to skip the player
// node during the transform-list walk (M4 covers the player; we don't
// want M5's later write overwriting M4's pose). Returns nullptr if the
// player hasn't been resolved yet.

void* current_player_handle();

// === Inline entity transform helpers =====================================
// Both M4 (single player) and M5 (NPC walk) use this exact memcpy pattern.
// SEH stays in the caller — each call site has different recovery needs.

inline void capture_pos_rot(PlayerSnap& dst, const void* entity) {
    const auto* base = static_cast<const uint8_t*>(entity);
    std::memcpy(dst.pos, base + kPlayerPosOffset, kPlayerPosBytes);
    std::memcpy(dst.rot, base + kPlayerRotOffset, kPlayerRotBytes);
    dst.qpc   = qpc_now();
    dst.valid = true;
}

inline void write_pos_rot(const PlayerSnap& src, void* entity) {
    auto* base = static_cast<uint8_t*>(entity);
    std::memcpy(base + kPlayerPosOffset, src.pos, kPlayerPosBytes);
    std::memcpy(base + kPlayerRotOffset, src.rot, kPlayerRotBytes);
}

} // namespace mtr::interp::detail
