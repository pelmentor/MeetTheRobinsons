// NPC visual debug overlay — Phase 1 implementation.
//
// Projects each NPC's world position to screen-space and draws a text
// label (name + pos + distance) at the projected coords. Sister module
// to mtr::trigger_overlay; shares projection math via mtr/overlay_math.h.
//
// Per the v2 plan in research/findings/npc-overlay-plan-2026-05-09.md
// (post audits aaab9a06 + aea25030):
//   - Walk dword_724DE4 (transform list head). Entity pointer is at
//     node+0x5C (NOT +0x40 as v1 of the plan claimed; both audits caught
//     this — interp_npc.cpp:115-117 and freecam.cpp:188 both read
//     entity-side data through +0x5C).
//   - Skip the player (we already have freecam-side player handling;
//     overlay duplication on the player is just clutter).
//   - For each entity: read pos via *(entity+0x48)+0x10 if non-NULL
//     (renderer's pos source for character entities, including any
//     M4/M5 interp writes), else fall back to entity+0x58.
//   - Read name via entity+0x50 char* with STRICT validation:
//     1) pointer is non-null and in [0x10000, 0x7FFE0000]
//     2) target page is committed + readable + non-guard
//     3) first byte is printable ASCII (0x20-0x7E)
//     4) NUL terminator within 64 bytes
//   - Project pos via row_vec * View * Proj (shared math).
//   - Use point_in_frustum (full 6-plane test) BEFORE the perspective
//     divide. Single-point reject on clip.w <= 0 alone is insufficient —
//     would waste AddText on off-screen-but-in-front NPCs.
//   - SEH-wrap the entire per-NPC iteration body. An entity mid-
//     destruction can have valid +0x48 pointer pointing to freed memory
//     that's still mapped, faulting on *(+0x48)+0x10.
//
// Phase 2 (anim state from entity+0x158) and Phase 3 (kv_get registry
// dump) NOT YET implemented — gated on Phase 0C RE work.

#include "mtr/npc_overlay.h"
#include "mtr/overlay_math.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::npc_overlay {

namespace {

using mtr::overlay_math::Vec4;
using mtr::overlay_math::row_mul;
using mtr::overlay_math::mat_mul;
using mtr::overlay_math::point_in_frustum;
using mtr::overlay_math::ndc_to_screen;

// === Engine globals ========================================================

constexpr uintptr_t kViewMatrixGlobalVA  = 0x00724C10;
constexpr uintptr_t kWorldMatrixGlobalVA = 0x00724C50;  // row 3 = cam_pos
constexpr uintptr_t kProjMatrixGlobalVA  = 0x00745AA0;
constexpr uintptr_t kTransformListHeadVA = 0x00724DE4;

// Transform-node offsets — confirmed by interp_npc.cpp + freecam.cpp.
constexpr uintptr_t kNodeNextOffset    = 0x04;
constexpr uintptr_t kNodeFlagsOffset   = 0x44;
constexpr uintptr_t kNodeEntityOffset  = 0x5C;  // logical NPC pointer
constexpr uint8_t   kNodeFlagsSkipBit  = 0x10;

// Entity offsets — confirmed by entity-system.md + player-entity-layout-2026-05-09.md.
constexpr uintptr_t kEntityRenderSubOffset = 0x48;  // ptr to render sub-component
constexpr uintptr_t kEntityNameOffset      = 0x50;  // char**
constexpr uintptr_t kEntityPosOffset       = 0x58;  // vec3 (game-logic pos)
constexpr uintptr_t kSubRenderPosOffset    = 0x10;  // vec3 (render pos)

constexpr int kMaxIterations = 8192;  // safety cap on transform-list traversal

// === User toggles ==========================================================

std::atomic<bool>     g_enabled{false};
std::atomic<bool>     g_show_name{true};
std::atomic<bool>     g_show_pos{true};
std::atomic<bool>     g_show_distance{true};
std::atomic<float>    g_distance_limit{0.0f};   // 0 = no limit
std::atomic<int>      g_last_visible{0};
std::atomic<int>      g_export_frames{0};
std::atomic<uint64_t> g_frame_seq{0};

// One-shot diagnostic: log the first N entities we encounter so we can
// verify that +0x5C/+0x50/+0x58 are reading sane values on first launch.
std::atomic<bool>     g_diag_logged{false};

// === Memory safety helpers =================================================
//
// `is_safe_to_read` mirrors widget_probe's pattern: VirtualQuery + commit /
// readable / non-guard checks. Returns true iff [p, p+n) is safe to read
// without faulting. Used inside SEH-guarded blocks as a fast pre-check.
bool is_safe_to_read(const void* p, size_t n) {
    if (!p || n == 0) return false;
    auto v = reinterpret_cast<uintptr_t>(p);
    if (v < 0x00010000) return false;
    if (v + n < v) return false;                    // overflow
    if (v + n > 0x7FFE0000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return v + n <= region_end;
}

// Strict name validator. The audit specifically called out that
// `is_safe_to_read` alone is insufficient — `+0x50` may be a non-name
// pointer for some entity classes. Required checks:
//   1. dereference is_safe (4 bytes)
//   2. pointer in user-mode range [0x10000, 0x7FFE0000]
//   3. first 64 bytes is_safe (string body)
//   4. first byte is printable ASCII (0x20-0x7E)
//   5. NUL terminator within first 64 bytes
//   6. all bytes before NUL are printable ASCII
// Returns the validated char* on success, nullptr otherwise.
const char* validate_name_at(uintptr_t entity_ptr) {
    const uintptr_t name_slot = entity_ptr + kEntityNameOffset;
    if (!is_safe_to_read(reinterpret_cast<void*>(name_slot), sizeof(uintptr_t))) {
        return nullptr;
    }
    const uintptr_t name_ptr = *reinterpret_cast<uintptr_t*>(name_slot);
    if (name_ptr < 0x00010000u || name_ptr > 0x7FFE0000u) return nullptr;
    if (!is_safe_to_read(reinterpret_cast<void*>(name_ptr), 64)) return nullptr;
    const char* s = reinterpret_cast<const char*>(name_ptr);
    if (s[0] < 0x20 || s[0] > 0x7E) return nullptr;
    bool found_nul = false;
    for (int i = 0; i < 64; ++i) {
        const char c = s[i];
        if (c == '\0') { found_nul = true; break; }
        if (c < 0x20 || c > 0x7E) return nullptr;
    }
    if (!found_nul) return nullptr;
    return s;
}

// Read pos for an entity. Prefer *(entity+0x48)+0x10 (renderer's pos for
// character entities; also where M4/M5 interp writes); fall back to
// entity+0x58 (game-logic pos) when the sub-component is NULL.
// Writes to out[3] on success.
bool read_entity_pos(uintptr_t entity_ptr, float out[3]) {
    // Try the render sub-component first.
    const uintptr_t sub_slot = entity_ptr + kEntityRenderSubOffset;
    if (is_safe_to_read(reinterpret_cast<void*>(sub_slot), sizeof(uintptr_t))) {
        const uintptr_t sub_ptr = *reinterpret_cast<uintptr_t*>(sub_slot);
        if (sub_ptr >= 0x00010000u && sub_ptr <= 0x7FFE0000u) {
            const uintptr_t pos_addr = sub_ptr + kSubRenderPosOffset;
            if (is_safe_to_read(reinterpret_cast<void*>(pos_addr), 12)) {
                const float* p = reinterpret_cast<const float*>(pos_addr);
                if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
                    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
                    return true;
                }
            }
        }
    }
    // Fallback: game-logic pos at entity+0x58.
    const uintptr_t pos_addr = entity_ptr + kEntityPosOffset;
    if (!is_safe_to_read(reinterpret_cast<void*>(pos_addr), 12)) return false;
    const float* p = reinterpret_cast<const float*>(pos_addr);
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    return true;
}

// === Matrix capture ========================================================

bool read_matrix(uintptr_t va, float out[16]) {
    bool ok = false;
    __try {
        const float* src = reinterpret_cast<const float*>(va);
        for (int i = 0; i < 16; ++i) out[i] = src[i];
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// Camera world position from the world matrix at 0x00724C50 (which is
// inverse(view) for rigid LookAt-RH; row 3 is cam_pos directly).
// Returns false on read fault or non-finite values.
bool read_cam_pos(float out[3]) {
    float world[16];
    if (!read_matrix(kWorldMatrixGlobalVA, world)) return false;
    if (!std::isfinite(world[12]) || !std::isfinite(world[13]) || !std::isfinite(world[14])) {
        return false;
    }
    out[0] = world[12]; out[1] = world[13]; out[2] = world[14];
    return true;
}

// === Transform-list walker (mirrors interp_npc.cpp pattern) ================
//
// Walks dword_724DE4 calling cb(entity_ptr) for every node we should
// consider. Skips:
//   - nodes with the +0x44 0x10 skip flag set
//   - nodes whose entity pointer fails validation
// Hard cap at 8192 iterations against pathological list cycles. The whole
// walk is wrapped in __try by the caller; this template does not itself
// SEH-guard each pointer read because the read is already inside the
// caller's per-NPC try/except.
template <typename Fn>
void walk_transform_list(Fn cb) {
    auto* node = *reinterpret_cast<uint8_t**>(kTransformListHeadVA);
    int safety = kMaxIterations;
    while (node && safety-- > 0) {
        const uint8_t flags = *(node + kNodeFlagsOffset);
        uint8_t* next       = *reinterpret_cast<uint8_t**>(node + kNodeNextOffset);
        if ((flags & kNodeFlagsSkipBit) == 0) {
            void* entity = *reinterpret_cast<void**>(node + kNodeEntityOffset);
            if (entity) {
                cb(reinterpret_cast<uintptr_t>(entity));
            }
        }
        node = next;
    }
}

// === Phase 1 visual diagnostic logging =====================================

void log_first_entities_once() {
    if (g_diag_logged.exchange(true)) return;
    int n = 0;
    __try {
        walk_transform_list([&](uintptr_t entity) {
            if (n >= 16) return;
            const char* name = validate_name_at(entity);
            float pos[3] = {0.0f, 0.0f, 0.0f};
            const bool got_pos = read_entity_pos(entity, pos);
            mtr::log::info("npc_overlay: diag entity[%d] ptr=0x%08X name=%s "
                           "pos=(%.2f, %.2f, %.2f)%s",
                           n,
                           (unsigned)entity,
                           name ? name : "(invalid)",
                           pos[0], pos[1], pos[2],
                           got_pos ? "" : " (pos-read-failed)");
            ++n;
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("npc_overlay: diag walker faulted at n=%d", n);
    }
    mtr::log::info("npc_overlay: diag dump complete (%d entities sampled)", n);
}

// === Per-NPC label render ==================================================

// Returns true if a label was drawn for this entity, false otherwise.
// Called inside the caller's SEH guard.
bool render_label_for_entity(uintptr_t entity, const float view_proj[16],
                             const float cam_pos[3],
                             float vp_w, float vp_h,
                             bool exporting, uint64_t frame_seq, int npc_idx,
                             ImDrawList* dl) {
    const char* name = validate_name_at(entity);
    float pos[3] = {0.0f, 0.0f, 0.0f};
    if (!read_entity_pos(entity, pos)) {
        if (exporting) {
            mtr::log::info("NPC_OVERLAY_NPC f=%llu idx=%d ptr=0x%08X "
                           "result=pos-read-failed",
                           (unsigned long long)frame_seq, npc_idx,
                           (unsigned)entity);
        }
        return false;
    }

    const Vec4 world = { pos[0], pos[1], pos[2], 1.0f };
    const Vec4 clip  = row_mul(world, view_proj);

    if (!point_in_frustum(clip)) {
        if (exporting) {
            mtr::log::info("NPC_OVERLAY_NPC f=%llu idx=%d ptr=0x%08X "
                           "name=\"%s\" pos=%.4f,%.4f,%.4f result=outside-frustum",
                           (unsigned long long)frame_seq, npc_idx,
                           (unsigned)entity, name ? name : "",
                           pos[0], pos[1], pos[2]);
        }
        return false;
    }

    float sx = 0.0f, sy = 0.0f;
    ndc_to_screen(clip, vp_w, vp_h, &sx, &sy);
    if (!std::isfinite(sx) || !std::isfinite(sy)) return false;

    // Distance to camera (used for fade and the optional "dist" field).
    const float dx = pos[0] - cam_pos[0];
    const float dy = pos[1] - cam_pos[1];
    const float dz = pos[2] - cam_pos[2];
    const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    const float dist_limit = g_distance_limit.load(std::memory_order_relaxed);
    float alpha_scale = 1.0f;
    if (dist_limit > 0.0f) {
        if (dist > dist_limit) return false;     // beyond cutoff entirely
        // Fade in over the last 25% of the limit.
        const float fade_start = dist_limit * 0.75f;
        if (dist > fade_start) {
            alpha_scale = 1.0f - (dist - fade_start) / (dist_limit - fade_start);
            if (alpha_scale < 0.0f) alpha_scale = 0.0f;
        }
    }

    // Build the label string — show only the toggled fields.
    char label[160];
    int written = 0;
    const bool sn = g_show_name.load(std::memory_order_relaxed);
    const bool sp = g_show_pos.load(std::memory_order_relaxed);
    const bool sd = g_show_distance.load(std::memory_order_relaxed);

    if (sn && name && name[0]) {
        written += std::snprintf(label + written, sizeof(label) - written,
                                 "%s", name);
    }
    if (sp) {
        written += std::snprintf(label + written, sizeof(label) - written,
                                 "%s(%.0f, %.0f, %.0f)",
                                 written > 0 ? " " : "",
                                 pos[0], pos[1], pos[2]);
    }
    if (sd) {
        written += std::snprintf(label + written, sizeof(label) - written,
                                 "%sd=%.0f",
                                 written > 0 ? "  " : "",
                                 dist);
    }
    if (written <= 0) return false;

    const ImU32 col = IM_COL32(255, 240, 180,
                               (unsigned char)(alpha_scale * 240.0f));
    const ImU32 dot = IM_COL32(255, 220, 140,
                               (unsigned char)(alpha_scale * 200.0f));

    // Anchor dot at the projected world pos.
    dl->AddCircleFilled(ImVec2(sx, sy), 3.0f, dot);
    // Label slightly above-right of the dot.
    dl->AddText(ImVec2(sx + 6.0f, sy - 8.0f), col, label);

    if (exporting) {
        mtr::log::info("NPC_OVERLAY_NPC f=%llu idx=%d ptr=0x%08X name=\"%s\" "
                       "pos=%.4f,%.4f,%.4f dist=%.4f screen=%.4f,%.4f label=\"%s\"",
                       (unsigned long long)frame_seq, npc_idx,
                       (unsigned)entity,
                       name ? name : "",
                       pos[0], pos[1], pos[2], dist, sx, sy, label);
    }
    return true;
}

} // namespace

// === Public API ============================================================

bool enabled()                  { return g_enabled.load(std::memory_order_acquire); }
void set_enabled(bool v) {
    g_enabled.store(v, std::memory_order_release);
    mtr::log::info("npc_overlay: enabled=%d", v ? 1 : 0);
    if (v) g_diag_logged.store(false);  // re-fire one-shot diag on next enable
}

int  visible_npc_count()        { return g_last_visible.load(std::memory_order_acquire); }

bool show_name()                { return g_show_name.load(std::memory_order_acquire); }
void set_show_name(bool v)      { g_show_name.store(v, std::memory_order_release); }

bool show_pos()                 { return g_show_pos.load(std::memory_order_acquire); }
void set_show_pos(bool v)       { g_show_pos.store(v, std::memory_order_release); }

bool show_distance()            { return g_show_distance.load(std::memory_order_acquire); }
void set_show_distance(bool v)  { g_show_distance.store(v, std::memory_order_release); }

float distance_limit()          { return g_distance_limit.load(std::memory_order_acquire); }
void  set_distance_limit(float v) {
    if (v < 0.0f) v = 0.0f;
    g_distance_limit.store(v, std::memory_order_release);
}

void set_export_frames(int n) {
    if (n < 0) n = 0;
    g_export_frames.store(n, std::memory_order_release);
    mtr::log::info("npc_overlay: export_frames=%d", n);
}
int  export_frames_remaining() { return g_export_frames.load(std::memory_order_acquire); }

void tick(IDirect3DDevice9* /*dev*/) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        g_last_visible.store(0, std::memory_order_release);
        return;
    }

    log_first_entities_once();

    float view[16], proj[16];
    if (!read_matrix(kViewMatrixGlobalVA, view) ||
        !read_matrix(kProjMatrixGlobalVA, proj)) {
        g_last_visible.store(0, std::memory_order_release);
        return;
    }

    float cam_pos[3] = {0.0f, 0.0f, 0.0f};
    read_cam_pos(cam_pos);   // best-effort; (0,0,0) fallback is fine for distance display

    float view_proj[16];
    mat_mul(view, proj, view_proj);

    const ImGuiIO& io = ImGui::GetIO();
    const float vp_w = io.DisplaySize.x;
    const float vp_h = io.DisplaySize.y;
    if (vp_w <= 0.0f || vp_h <= 0.0f) {
        g_last_visible.store(0, std::memory_order_release);
        return;
    }

    const int export_remaining = g_export_frames.load(std::memory_order_acquire);
    const bool exporting = export_remaining > 0;
    const uint64_t frame_seq = g_frame_seq.fetch_add(1, std::memory_order_relaxed);

    if (exporting) {
        mtr::log::info("NPC_OVERLAY_FRAME_BEGIN f=%llu vp=%.4f,%.4f cam=%.4f,%.4f,%.4f",
                       (unsigned long long)frame_seq, vp_w, vp_h,
                       cam_pos[0], cam_pos[1], cam_pos[2]);
        mtr::log::info("NPC_OVERLAY_VIEW f=%llu "
                       "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
                       "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
                       (unsigned long long)frame_seq,
                       view[0],  view[1],  view[2],  view[3],
                       view[4],  view[5],  view[6],  view[7],
                       view[8],  view[9],  view[10], view[11],
                       view[12], view[13], view[14], view[15]);
        mtr::log::info("NPC_OVERLAY_PROJ f=%llu "
                       "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
                       "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
                       (unsigned long long)frame_seq,
                       proj[0],  proj[1],  proj[2],  proj[3],
                       proj[4],  proj[5],  proj[6],  proj[7],
                       proj[8],  proj[9],  proj[10], proj[11],
                       proj[12], proj[13], proj[14], proj[15]);
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    int visible = 0;
    int idx = 0;

    // Single SEH wrapping the entire walk + render. An entity mid-
    // destruction or a corrupted node anywhere in the chain bails out
    // cleanly — we abandon the rest of this frame's overlay.
    __try {
        walk_transform_list([&](uintptr_t entity) {
            if (render_label_for_entity(entity, view_proj, cam_pos,
                                        vp_w, vp_h, exporting, frame_seq, idx, dl)) {
                ++visible;
            }
            ++idx;
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (exporting) {
            mtr::log::info("NPC_OVERLAY_NPC f=%llu idx=%d result=walker-faulted",
                           (unsigned long long)frame_seq, idx);
        }
    }

    if (exporting) {
        mtr::log::info("NPC_OVERLAY_FRAME_END f=%llu visible=%d total_iter=%d",
                       (unsigned long long)frame_seq, visible, idx);
        g_export_frames.fetch_sub(1, std::memory_order_acq_rel);
    }

    g_last_visible.store(visible, std::memory_order_release);
}

} // namespace mtr::npc_overlay
