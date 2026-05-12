// Prop visual debug overlay — Phase 1 implementation.
//
// Sister to mtr::npc_overlay. Walks dword_724DE4 (transform list head),
// per-entity calls entity_kv::get(entity, "propDisassembleable") and
// renders a label at the projected world pos for any entity whose value
// is non-null. Other prop tags (scannable / targetable / climbable / ...)
// shipped as Phase 1 toggles but disabled by default — single-key per
// entity per frame is the baseline; multi-key opt-in is for users
// debugging the broader prop taxonomy.
//
// Death-handling: FREE. The disassembler destroys a prop -> the engine
// removes the entity from the transform list -> our walker doesn't
// visit it next frame -> the label disappears. No event hook needed.
//
// Per the v2 plan in research/findings/prop-overlay-plan-2026-05-10.md
// (post audits afbf879e + aa16864a):
//   - Walker offset: node+0x5C for entity (matches npc_overlay; will be
//     verified by the diag log on first enable for prop entities
//     specifically — if `+0x5C` is wrong for props, kv calls return
//     null silently and we switch to `+0x40`).
//   - Per-entity SEH: pre-read node->next BEFORE the per-entity body so
//     a fault on entity N doesn't strand the walker at a stale node.
//     Different from npc_overlay's coarse-walker SEH.
//   - Phase 1 single-key: only `propDisassembleable` enabled by default.
//   - Walk-time profiling: when exporting, log per-frame walk-microseconds
//     so Phase 2 has data to budget against.

#include "mtr/prop_overlay.h"
#include "mtr/overlay_math.h"
#include "mtr/entity_kv.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::prop_overlay {

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

// Transform-node offsets (mirror npc_overlay).
constexpr uintptr_t kNodeNextOffset    = 0x04;
constexpr uintptr_t kNodeFlagsOffset   = 0x44;
constexpr uintptr_t kNodeEntityOffset  = 0x5C;
constexpr uint8_t   kNodeFlagsSkipBit  = 0x10;

constexpr uintptr_t kEntityRenderSubOffset = 0x48;
constexpr uintptr_t kEntityNameOffset      = 0x50;
constexpr uintptr_t kEntityPosOffset       = 0x58;
constexpr uintptr_t kSubRenderPosOffset    = 0x10;

constexpr int kMaxIterations = 8192;

// === Tag table =============================================================

struct TagDef {
    const char*       key;          // kv-bag key (engine string)
    const char*       short_label;  // label-ready short form
    std::atomic<bool> show;         // toggle
};

// Phase 1 default: only disassembleable ON. Other tags shipped (so the
// UI surface is complete) but default-OFF — single-key per entity per
// frame keeps the kv-call rate sane until profiling proves otherwise.
TagDef g_tags[] = {
    { "propDisassembleable", "disassemble", { true } },
    { "propScannable",       "scan",        { false } },
    { "propTargetable",      "target",      { false } },
    { "propClimbable",       "climb",       { false } },
    { "propPushPullable",    "push",        { false } },
    { "propLevitateMoveCmd", "levitate",    { false } },  // marker prop for levitate
    { "propLockToPath",      "path",        { false } },
    { "propSSTarget",        "ss",          { false } },
};
constexpr int kNumTags = sizeof(g_tags) / sizeof(g_tags[0]);

// === User toggles ==========================================================

std::atomic<bool>     g_enabled{false};
std::atomic<bool>     g_show_name{true};
std::atomic<bool>     g_show_pos{false};
std::atomic<bool>     g_show_distance{false};
std::atomic<bool>     g_show_tags{true};
std::atomic<float>    g_distance_limit{0.0f};
std::atomic<int>      g_last_visible{0};
std::atomic<int>      g_export_frames{0};
std::atomic<uint64_t> g_frame_seq{0};
std::atomic<bool>     g_diag_logged{false};

// === Memory safety helpers (mirror npc_overlay) ============================

bool is_safe_to_read(const void* p, size_t n) {
    if (!p || n == 0) return false;
    auto v = reinterpret_cast<uintptr_t>(p);
    if (v < 0x00010000) return false;
    if (v + n < v) return false;
    if (v + n > 0x7FFE0000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return v + n <= region_end;
}

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

bool read_entity_pos(uintptr_t entity_ptr, float out[3]) {
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
    const uintptr_t pos_addr = entity_ptr + kEntityPosOffset;
    if (!is_safe_to_read(reinterpret_cast<void*>(pos_addr), 12)) return false;
    const float* p = reinterpret_cast<const float*>(pos_addr);
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    return true;
}

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

bool read_cam_pos(float out[3]) {
    float world[16];
    if (!read_matrix(kWorldMatrixGlobalVA, world)) return false;
    if (!std::isfinite(world[12]) || !std::isfinite(world[13]) || !std::isfinite(world[14])) {
        return false;
    }
    out[0] = world[12]; out[1] = world[13]; out[2] = world[14];
    return true;
}

// === Per-entity prop check =================================================

// Returns the index of the first matching tag (per the show-toggles)
// whose kv value is non-null on this entity, plus appends any additional
// matching tag short-labels to `tags_out` (for the "[disassemble, push]"
// label suffix). Returns -1 if no enabled tag matches.
//
// Single-key fast path: when only one tag is enabled (Phase 1 default),
// this is one kv call per entity. With more tags enabled the cost scales
// linearly; profiling instrumentation in tick() exposes the cost.
int classify_prop(void* entity, char* tags_out, size_t tags_cap) {
    int first_match = -1;
    size_t out_off = 0;
    if (tags_out && tags_cap > 0) tags_out[0] = '\0';
    for (int i = 0; i < kNumTags; ++i) {
        if (!g_tags[i].show.load(std::memory_order_relaxed)) continue;
        if (mtr::entity_kv::has(entity, g_tags[i].key)) {
            if (first_match < 0) first_match = i;
            if (tags_out && tags_cap > 0 && g_show_tags.load(std::memory_order_relaxed)) {
                const int n = std::snprintf(tags_out + out_off, tags_cap - out_off,
                                            "%s%s",
                                            out_off > 0 ? "," : "",
                                            g_tags[i].short_label);
                if (n > 0 && static_cast<size_t>(n) < tags_cap - out_off) {
                    out_off += static_cast<size_t>(n);
                }
            }
        }
    }
    return first_match;
}

// === Walker (per-entity SEH scope) =========================================
//
// Differs from npc_overlay's walker in that the SEH guard wraps the
// per-entity body, NOT the whole walk. Pre-reads node->next BEFORE the
// body so a fault on entity N resumes at N+1 instead of stranding the
// walker at a stale node pointer.
template <typename Fn>
void walk_transform_list_per_entity_seh(Fn cb) {
    auto* node = *reinterpret_cast<uint8_t**>(kTransformListHeadVA);
    int safety = kMaxIterations;
    while (node && safety-- > 0) {
        // Pre-read both the next pointer AND the flags+entity. If the
        // node itself is corrupt and we fault on these reads, that's
        // still the per-frame crash boundary — bail out.
        uint8_t* next  = nullptr;
        uint8_t  flags = 0;
        void*    entity = nullptr;
        bool     node_readable = false;
        __try {
            next   = *reinterpret_cast<uint8_t**>(node + kNodeNextOffset);
            flags  = *(node + kNodeFlagsOffset);
            entity = *reinterpret_cast<void**>(node + kNodeEntityOffset);
            node_readable = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            node_readable = false;
        }
        if (!node_readable) return;  // can't safely advance to next; abandon walk

        if ((flags & kNodeFlagsSkipBit) == 0 && entity) {
            // Per-entity body in its own SEH scope. A fault here resumes
            // the walk at `next` rather than killing the rest of the
            // frame's labels.
            __try {
                cb(reinterpret_cast<uintptr_t>(entity));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Swallow; continue the walk.
            }
        }
        node = next;
    }
}

// === Diag log (one-shot on first enable) ===================================

void log_first_props_once() {
    if (g_diag_logged.exchange(true)) return;
    int n_total = 0;
    int n_props = 0;
    walk_transform_list_per_entity_seh([&](uintptr_t entity) {
        if (n_total >= 32) return;
        ++n_total;
        const char* name = validate_name_at(entity);
        const char* dis  = mtr::entity_kv::get(reinterpret_cast<void*>(entity),
                                               "propDisassembleable");
        const char* cls  = mtr::entity_kv::get(reinterpret_cast<void*>(entity),
                                               "class");
        if (dis || cls) {
            mtr::log::info("prop_overlay: diag entity[%d] ptr=0x%08X "
                           "name=%s class=%s propDisassembleable=%s",
                           n_total - 1, (unsigned)entity,
                           name ? name : "(invalid)",
                           cls  ? cls  : "(none)",
                           dis  ? dis  : "(none)");
            if (dis) ++n_props;
        }
    });
    mtr::log::info("prop_overlay: diag dump complete "
                   "(%d entities sampled, %d props found)",
                   n_total, n_props);
}

// === Per-prop label render =================================================

bool render_label_for_prop(uintptr_t entity, const float view_proj[16],
                            const float cam_pos[3],
                            float vp_w, float vp_h,
                            bool exporting, uint64_t frame_seq, int prop_idx,
                            ImDrawList* dl) {
    char tags_buf[128] = {0};
    const int first_tag = classify_prop(reinterpret_cast<void*>(entity),
                                        tags_buf, sizeof(tags_buf));
    if (first_tag < 0) return false;  // not a prop (under current toggles)

    const char* name = validate_name_at(entity);
    float pos[3] = {0.0f, 0.0f, 0.0f};
    if (!read_entity_pos(entity, pos)) {
        if (exporting) {
            mtr::log::info("PROP_OVERLAY_PROP f=%llu idx=%d ptr=0x%08X "
                           "result=pos-read-failed",
                           (unsigned long long)frame_seq, prop_idx,
                           (unsigned)entity);
        }
        return false;
    }

    const Vec4 world = { pos[0], pos[1], pos[2], 1.0f };
    const Vec4 clip  = row_mul(world, view_proj);

    if (!point_in_frustum(clip)) {
        if (exporting) {
            mtr::log::info("PROP_OVERLAY_PROP f=%llu idx=%d ptr=0x%08X "
                           "name=\"%s\" tags=\"%s\" pos=%.4f,%.4f,%.4f "
                           "result=outside-frustum",
                           (unsigned long long)frame_seq, prop_idx,
                           (unsigned)entity, name ? name : "",
                           tags_buf, pos[0], pos[1], pos[2]);
        }
        return false;
    }

    float sx = 0.0f, sy = 0.0f;
    ndc_to_screen(clip, vp_w, vp_h, &sx, &sy);
    if (!std::isfinite(sx) || !std::isfinite(sy)) return false;

    const float dx = pos[0] - cam_pos[0];
    const float dy = pos[1] - cam_pos[1];
    const float dz = pos[2] - cam_pos[2];
    const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    const float dist_limit = g_distance_limit.load(std::memory_order_relaxed);
    float alpha_scale = 1.0f;
    if (dist_limit > 0.0f) {
        if (dist > dist_limit) return false;
        const float fade_start = dist_limit * 0.75f;
        if (dist > fade_start) {
            alpha_scale = 1.0f - (dist - fade_start) / (dist_limit - fade_start);
            if (alpha_scale < 0.0f) alpha_scale = 0.0f;
        }
    }

    char label[192];
    int written = 0;
    const bool sn = g_show_name.load(std::memory_order_relaxed);
    const bool sp = g_show_pos.load(std::memory_order_relaxed);
    const bool sd = g_show_distance.load(std::memory_order_relaxed);
    const bool st = g_show_tags.load(std::memory_order_relaxed);

    if (sn && name && name[0]) {
        written += std::snprintf(label + written, sizeof(label) - written,
                                 "%s", name);
    }
    if (st && tags_buf[0]) {
        written += std::snprintf(label + written, sizeof(label) - written,
                                 "%s[%s]",
                                 written > 0 ? " " : "", tags_buf);
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
                                 written > 0 ? "  " : "", dist);
    }
    if (written <= 0) return false;

    // Yellow-ish for disassembleable; cyan-ish for scannable; etc. Phase 1
    // ships single-color (yellow). Per-tag color in Phase 3 (UX polish).
    const ImU32 col = IM_COL32(255, 220, 80,
                               (unsigned char)(alpha_scale * 240.0f));
    const ImU32 dot = IM_COL32(255, 200, 60,
                               (unsigned char)(alpha_scale * 200.0f));

    dl->AddCircleFilled(ImVec2(sx, sy), 4.0f, dot);
    dl->AddText(ImVec2(sx + 7.0f, sy - 8.0f), col, label);

    if (exporting) {
        mtr::log::info("PROP_OVERLAY_PROP f=%llu idx=%d ptr=0x%08X "
                       "name=\"%s\" tags=\"%s\" pos=%.4f,%.4f,%.4f "
                       "dist=%.4f screen=%.4f,%.4f label=\"%s\"",
                       (unsigned long long)frame_seq, prop_idx,
                       (unsigned)entity, name ? name : "",
                       tags_buf, pos[0], pos[1], pos[2],
                       dist, sx, sy, label);
    }
    return true;
}

} // namespace

// === Public API ============================================================

bool enabled()                  { return g_enabled.load(std::memory_order_acquire); }
void set_enabled(bool v) {
    g_enabled.store(v, std::memory_order_release);
    mtr::log::info("prop_overlay: enabled=%d", v ? 1 : 0);
    if (v) g_diag_logged.store(false);
}

int  visible_prop_count()       { return g_last_visible.load(std::memory_order_acquire); }

#define DEFINE_TAG_TOGGLE(NAME, INDEX)                                       \
    bool show_##NAME() {                                                     \
        return g_tags[INDEX].show.load(std::memory_order_acquire);           \
    }                                                                        \
    void set_show_##NAME(bool v) {                                           \
        g_tags[INDEX].show.store(v, std::memory_order_release);              \
    }

DEFINE_TAG_TOGGLE(disassembleable, 0)
DEFINE_TAG_TOGGLE(scannable,       1)
DEFINE_TAG_TOGGLE(targetable,      2)
DEFINE_TAG_TOGGLE(climbable,       3)
DEFINE_TAG_TOGGLE(push_pullable,   4)
DEFINE_TAG_TOGGLE(levitate,        5)
DEFINE_TAG_TOGGLE(lock_to_path,    6)
DEFINE_TAG_TOGGLE(ss_target,       7)

#undef DEFINE_TAG_TOGGLE

bool show_name()                { return g_show_name.load(std::memory_order_acquire); }
void set_show_name(bool v)      { g_show_name.store(v, std::memory_order_release); }
bool show_pos()                 { return g_show_pos.load(std::memory_order_acquire); }
void set_show_pos(bool v)       { g_show_pos.store(v, std::memory_order_release); }
bool show_distance()            { return g_show_distance.load(std::memory_order_acquire); }
void set_show_distance(bool v)  { g_show_distance.store(v, std::memory_order_release); }
bool show_tags()                { return g_show_tags.load(std::memory_order_acquire); }
void set_show_tags(bool v)      { g_show_tags.store(v, std::memory_order_release); }

float distance_limit()          { return g_distance_limit.load(std::memory_order_acquire); }
void  set_distance_limit(float v) {
    if (v < 0.0f) v = 0.0f;
    g_distance_limit.store(v, std::memory_order_release);
}

void set_export_frames(int n) {
    if (n < 0) n = 0;
    g_export_frames.store(n, std::memory_order_release);
    mtr::log::info("prop_overlay: export_frames=%d", n);
}
int  export_frames_remaining() { return g_export_frames.load(std::memory_order_acquire); }

void tick(IDirect3DDevice9* /*dev*/) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        g_last_visible.store(0, std::memory_order_release);
        return;
    }

    log_first_props_once();

    float view[16], proj[16];
    if (!read_matrix(kViewMatrixGlobalVA, view) ||
        !read_matrix(kProjMatrixGlobalVA, proj)) {
        g_last_visible.store(0, std::memory_order_release);
        return;
    }

    float cam_pos[3] = {0.0f, 0.0f, 0.0f};
    read_cam_pos(cam_pos);

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

    LARGE_INTEGER qpc_start{};
    if (exporting) {
        QueryPerformanceCounter(&qpc_start);
        mtr::log::info("PROP_OVERLAY_FRAME_BEGIN f=%llu vp=%.4f,%.4f cam=%.4f,%.4f,%.4f",
                       (unsigned long long)frame_seq, vp_w, vp_h,
                       cam_pos[0], cam_pos[1], cam_pos[2]);
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    int visible = 0;
    int idx = 0;

    walk_transform_list_per_entity_seh([&](uintptr_t entity) {
        if (render_label_for_prop(entity, view_proj, cam_pos,
                                  vp_w, vp_h, exporting, frame_seq, idx, dl)) {
            ++visible;
        }
        ++idx;
    });

    if (exporting) {
        LARGE_INTEGER qpc_end{}, qpc_freq{};
        QueryPerformanceCounter(&qpc_end);
        QueryPerformanceFrequency(&qpc_freq);
        const double walk_us = qpc_freq.QuadPart > 0
            ? double(qpc_end.QuadPart - qpc_start.QuadPart) * 1.0e6
              / double(qpc_freq.QuadPart)
            : 0.0;
        mtr::log::info("PROP_OVERLAY_FRAME_END f=%llu visible=%d total_iter=%d "
                       "walk_us=%.1f",
                       (unsigned long long)frame_seq, visible, idx, walk_us);
        g_export_frames.fetch_sub(1, std::memory_order_acq_rel);
    }

    g_last_visible.store(visible, std::memory_order_release);
}

} // namespace mtr::prop_overlay
