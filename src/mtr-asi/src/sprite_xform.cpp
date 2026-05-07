// Per-sprite-entry live transform editor — v2 (composite identity).
//
// v1 (initial 2026-05-06) keyed slots by raw state_key only. Live test
// revealed that's too coarse: shared atlases (one font texture serving
// menu+HUD text) collapse logically distinct UI into one bucket, so
// labelling "menu text" inevitably also moved HUD text.
//
// v2 keys slots by a 4-tuple "composite key":
//
//     state_key       — texture/asset pointer (always exact)
//     uv_bucket       — hash of the entry's inline_uvs (separates glyphs
//                       and icons sharing an atlas)
//     screen_context  — hash of (active screen depth + top screen name)
//                       at the moment process_list runs (separates same
//                       asset rendered in different screens — HUD vs pause)
//     bbox_quadrant   — 3x3 bin of the entry's centroid (last-resort
//                       discriminator for same asset+UV+screen at different
//                       positions, e.g. mirrored arms of a robot)
//
// Wildcards: any of uv_bucket / screen_context / bbox_quadrant may be set
// to a sentinel "match anything" value in a slot's pattern. v1 entries
// migrate forward as state_key-only patterns (all three wildcards), so
// pre-v2 user labels keep working unchanged.
//
// Lookup: walk all slots whose state_key matches; pick the one with the
// highest specificity (= count of non-wildcard components matched).
// Tiebreak by most-recently-modified (later wins). State_key is hashed,
// so the inner walk is bounded to the small set of variants per state_key
// (typically 1-3).
//
// Limitations (unchanged from v1):
//   - state_key is a heap pointer; values shift across game sessions, so
//     persistence is best-effort. Phase D (texture-loader RE) addresses
//     this by mapping path → state_key at load time.
//   - Only inline_positions (entry+0x28) modified; ext_positions left
//     alone (could be const memory).

#include <windows.h>
#include <atomic>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mtr::log { void info(const char* fmt, ...); }

// Pulled from the screen-push mirror so we can stamp each frame with the
// current top-screen identity (used by composite-key screen_context). The
// mirror updates on push/pop; we sample it once per frame at the top of
// process_list so all entries within one frame share the same context.
namespace mtr::screen_push {
    bool current_top_name(char* out, size_t out_size);
    int  stack_depth();
}

// Auto-naming via state_key target memory read (Phase D.4). The probe
// confirmed that every state_key target has a 32-byte path buffer at
// offset 0x2C. read_state_key_path safely reads and validates it.
namespace mtr::state_key_probe {
    size_t read_state_key_path(uint32_t state_key, char* out, size_t out_size);
}

// Quad capture for click-picking + gizmo overlay. process_list submits
// each entry's final post-transform 4-corner quad along with the slot
// it resolved to, so the menu can hit-test cursor positions and draw
// gizmos on the right sprite.
namespace mtr::sprite_picking {
    void begin_frame();
    void submit(int slot_idx, uint32_t state_key, const float* inline_positions);
    void end_frame();
}

namespace mtr::sprite_xform {

namespace {

constexpr uintptr_t kSpriteListHeadVA = 0x007271E8;

#pragma pack(push, 4)
struct SpriteEntry {
    /* +0x00 */ SpriteEntry* next;
    /* +0x04 */ uint32_t     unk_04;
    /* +0x08 */ uint32_t     flags;
    /* +0x0C */ uint32_t     unk_0C;
    /* +0x10 */ uint32_t     state_key;
    /* +0x14 */ uint16_t     unk_14;
    /* +0x16 */ uint16_t     sort_key;
    /* +0x18 */ uint32_t     unk_18;
    /* +0x1C */ uint8_t      alpha_mod;
    /* +0x1D */ uint8_t      blend_mode;
    /* +0x1E */ uint16_t     unk_1E;
    /* +0x20 */ uint8_t      pad_20[8];
    /* +0x28 */ float        inline_positions[12];
    /* +0x58 */ float*       ext_positions;
    /* +0x5C */ float        inline_uvs[8];
    /* +0x7C */ float*       ext_uvs;
    /* +0x80 */ uint32_t     inline_colors[4];
};
#pragma pack(pop)
static_assert(sizeof(SpriteEntry) == 0x90, "SpriteEntry layout drift");

constexpr int kMaxKeys = 64;

constexpr size_t kNameLen  = 48;
constexpr size_t kGroupLen = 32;

// Wildcard sentinels for composite-key components. Picked at the top of
// each component's value range so concrete values can never collide.
constexpr uint16_t kUVWildcard      = 0xFFFF;
constexpr uint8_t  kScreenWildcard  = 0xFF;
constexpr uint8_t  kQuadWildcard    = 0xFF;  // concrete quadrant is 0..8

struct CompositeKey {
    uint32_t state_key      = 0;
    uint16_t uv_bucket      = kUVWildcard;
    uint8_t  screen_context = kScreenWildcard;
    uint8_t  bbox_quadrant  = kQuadWildcard;

    bool is_wildcard_pattern() const {
        return uv_bucket      == kUVWildcard
            && screen_context == kScreenWildcard
            && bbox_quadrant  == kQuadWildcard;
    }
};

// Per-slot identity: the slot's stored *pattern* (which may have wildcards)
// plus the most-recent *concrete* key that matched it. The concrete key is
// only meaningful for diagnostic display in the UI ("this row matched a
// sprite with uv=0x4F2A in screen MainMenu") and as the source for the
// Specialize button — it copies the concrete components into a new slot.
struct Slot {
    CompositeKey key;             // pattern (may have wildcards)
    CompositeKey last_concrete;   // most recent concrete key seen
    bool         last_concrete_valid = false;

    uint64_t total_count     = 0;
    uint32_t frame_count     = 0;
    uint32_t last_seen_frame = 0;

    char     name[kNameLen]   = {0};
    char     group[kGroupLen] = {0};

    float    offset_x = 0.0f;
    float    offset_y = 0.0f;
    float    scale_x  = 1.0f;
    float    scale_y  = 1.0f;
    bool     hidden   = false;
    bool     highlight = false;

    // Monotonic edit counter — increments on user state mutation. Used as
    // tiebreaker when two equally-specific patterns match (most-recently-
    // edited wins, so a freshly specialised slot takes precedence over its
    // wildcard parent).
    uint64_t edit_seq = 0;

    // Auto-naming bookkeeping (Phase D.4):
    //  - `auto_named` true when `name` was populated by reading the
    //    state_key target's +0x2C path field. Lets the UI render auto-
    //    populated names with a different style.
    //  - `auto_name_attempted` records that we've already tried the
    //    target-memory read for this slot. Prevents per-frame retry when
    //    the read failed (e.g. target memory unreadable, or path field
    //    contains non-ASCII garbage).
    bool auto_named          = false;
    bool auto_name_attempted = false;

    // Phase D.5 — auto-derived asset path. Populated alongside `name` on
    // first auto-name read, but NEVER overwritten by user edits to the
    // name field (so even if the user types "wilbur smile" we still know
    // what asset this slot represents). Used by save/load for cross-
    // session matching: persistence is keyed by path, not raw state_key
    // (state_keys are heap pointers that shift across sessions; paths are
    // engine-level identifiers that stay stable).
    char path[kNameLen] = {0};
};

bool key_matches_pattern(const CompositeKey& pattern, const CompositeKey& concrete) {
    if (pattern.state_key != concrete.state_key) return false;
    if (pattern.uv_bucket      != kUVWildcard     && pattern.uv_bucket      != concrete.uv_bucket)      return false;
    if (pattern.screen_context != kScreenWildcard && pattern.screen_context != concrete.screen_context) return false;
    if (pattern.bbox_quadrant  != kQuadWildcard   && pattern.bbox_quadrant  != concrete.bbox_quadrant)  return false;
    return true;
}

int key_specificity(const CompositeKey& pattern) {
    int s = 1;  // state_key always counts
    if (pattern.uv_bucket      != kUVWildcard)     ++s;
    if (pattern.screen_context != kScreenWildcard) ++s;
    if (pattern.bbox_quadrant  != kQuadWildcard)   ++s;
    return s;
}

std::atomic<bool>     g_enabled{false};
std::atomic<uint64_t> g_frame_no{0};
std::atomic<uint64_t> g_last_total_entries{0};
std::atomic<uint64_t> g_edit_seq{1};
std::mutex            g_mu;
Slot                  g_slots[kMaxKeys];

// Hash index: state_key → list of slot indices that have that state_key.
// One state_key can have multiple slots (a wildcard "default" plus several
// specialised variants); the inner vector is small (typically 1-3) so the
// best-match walk over it is cheap. Maintained alongside g_slots[]; insert
// / evict / clear paths update both.
std::unordered_map<uint32_t, std::vector<int>> g_state_index;

// Phase D.5 — pending-by-path queue. Populated at ini load time with one
// entry per persisted slot that has a non-empty path. Each entry sits in
// the queue until process_list auto-names a slot with a matching path —
// then the queued state (transform/name/group) is applied to that slot
// and the queue entry is dropped.
//
// Why not apply at load time? Because state_keys at load time refer to
// heap objects from the *previous* session (now invalidated by ASLR /
// allocator nondeterminism). The current-session state_keys won't be
// known until process_list walks the live sprite list and we read the
// path field from each one's target memory.
struct PendingByPath {
    char     path[kNameLen]   = {0};
    char     name[kNameLen]   = {0};   // restored manual name (empty if was auto)
    char     group[kGroupLen] = {0};
    float    offset_x = 0.0f, offset_y = 0.0f;
    float    scale_x  = 1.0f, scale_y  = 1.0f;
    bool     hidden   = false;
    // Composite-key components (for v2 specialisation): when not all
    // wildcard, the matching slot gets these as its pattern (creates a
    // specialised variant if needed). v1 entries (all-wildcard) load as
    // wildcard pattern.
    uint16_t uv_bucket      = 0xFFFF;
    uint8_t  screen_context = 0xFF;
    uint8_t  bbox_quadrant  = 0xFF;
};
std::vector<PendingByPath> g_pending_by_path;

// Highlight overlay state: cleared at the top of every process_list, then
// each highlighted slot's matching entries have their post-transform
// bboxes pushed here. The menu draw path then reads g_highlight_boxes
// and renders ImGui rectangles on top of the game frame at those
// positions — that's the actual "Hilite" indicator (the engine-side
// alpha_mod=255 write is a no-op for most HUD entries because the
// modulation flag bits 0x100/0x400 aren't set on them).
struct HighlightBoxInternal {
    float x0, y0, x1, y1;
    uint32_t state_key;       // for color hashing in the overlay
};
std::vector<HighlightBoxInternal> g_highlight_boxes;

// Derive a group label from an asset path.
//
// Heuristic chain — first that produces a non-empty result wins:
//   1. Last-directory-before-filename (e.g. "ui/menu/foo.dds" → "menu").
//   2. Basename without extension (e.g. "wilbur_smile.dds" → "wilbur_smile").
//   3. Path as-is (truncated to fit kGroupLen).
//
// Rationale: many engines store asset paths as plain filenames in the
// runtime object (no directory tree retained at load), so requiring a
// directory separator would leave most slots ungrouped. Falling back to
// the basename means each unique texture file becomes its own group;
// every variant slot pinned to that texture's specific UV / screen /
// quadrant goes into the same group, so "drag all letters of this font
// at once" works after auto-grouping.
void derive_group_from_path(const char* path, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!path || !path[0]) return;

    // Locate last and second-to-last separators (handles both / and \).
    const char* last_sep = nullptr;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (last_sep) {
        const char* second_last = nullptr;
        for (const char* p = path; p < last_sep; ++p) {
            if (*p == '/' || *p == '\\') second_last = p;
        }
        const char* start = second_last ? (second_last + 1) : path;
        size_t len = static_cast<size_t>(last_sep - start);
        if (len > 0) {
            if (len >= out_size) len = out_size - 1;
            std::memcpy(out, start, len);
            out[len] = 0;
            return;
        }
    }

    // No directory: use basename without extension.
    const char* basename_start = last_sep ? (last_sep + 1) : path;
    const char* dot = nullptr;
    for (const char* p = basename_start; *p; ++p) {
        if (*p == '.') dot = p;
    }
    size_t base_len = dot
        ? static_cast<size_t>(dot - basename_start)
        : std::strlen(basename_start);
    if (base_len == 0) return;
    if (base_len >= out_size) base_len = out_size - 1;
    std::memcpy(out, basename_start, base_len);
    out[base_len] = 0;
}

void index_add(uint32_t state_key, int slot_idx) {
    g_state_index[state_key].push_back(slot_idx);
}
void index_remove(uint32_t state_key, int slot_idx) {
    auto it = g_state_index.find(state_key);
    if (it == g_state_index.end()) return;
    auto& v = it->second;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == slot_idx) {
            v[i] = v.back();
            v.pop_back();
            break;
        }
    }
    if (v.empty()) g_state_index.erase(it);
}

// === Concrete-key derivation from a SpriteEntry =============================

// Cheap 16-bit hash over the 8-float UV array, quantised to 8 bits per
// float. crc16-style with a known seed. Returns a value in [0, 0xFFFE]
// (avoids the wildcard sentinel 0xFFFF — clamp if we ever land on it).
uint16_t compute_uv_bucket(const float* uvs8) {
    uint16_t h = 0xA5A5;
    for (int i = 0; i < 8; ++i) {
        // Quantise to 8 bits: 0..255 over the typical UV range. UVs are
        // [0,1] for menu sprites and can extend past for HUD; we clamp
        // implicitly by truncation.
        const float v = uvs8[i];
        const int q  = static_cast<int>(v * 255.0f);
        const uint8_t b = static_cast<uint8_t>(q & 0xFF);
        h = static_cast<uint16_t>((h ^ b) * 0x101);
        h ^= static_cast<uint16_t>(h >> 5);
    }
    if (h == kUVWildcard) h = 0xFFFE;
    return h;
}

// 8-bit screen context: high nibble = clamped stack depth (0..15), low
// nibble = djb2-low-4 of top-screen-name. Sampled once per frame at the
// top of process_list so all entries within one frame share the value.
uint8_t compute_screen_context_for_frame() {
    char top[64] = {0};
    mtr::screen_push::current_top_name(top, sizeof(top));
    uint32_t h = 5381;
    for (const char* p = top; *p; ++p) {
        h = ((h << 5) + h) ^ static_cast<uint8_t>(*p);
    }
    int depth = mtr::screen_push::stack_depth();
    if (depth < 0)  depth = 0;
    if (depth > 15) depth = 15;
    uint8_t v = static_cast<uint8_t>((depth & 0x0F) << 4) | static_cast<uint8_t>(h & 0x0F);
    if (v == kScreenWildcard) v = 0xFE;
    return v;
}

// 3x3 quadrant of the entry's centroid in screen-normalised space. Engine
// authoring convention is roughly [0,1]² for menu sprites (M3.2 capture),
// HUD often extends outside; bins clamp to the edge buckets at the
// boundary so out-of-frame stuff still gets a stable bin.
//   0 1 2     bin = (y_bin*3) + x_bin
//   3 4 5
//   6 7 8
uint8_t compute_bbox_quadrant(const float* pos12) {
    const float cx = (pos12[0] + pos12[3] + pos12[6] + pos12[ 9]) * 0.25f;
    const float cy = (pos12[1] + pos12[4] + pos12[7] + pos12[10]) * 0.25f;
    int xb;
    if      (cx < (1.0f / 3.0f)) xb = 0;
    else if (cx < (2.0f / 3.0f)) xb = 1;
    else                         xb = 2;
    int yb;
    if      (cy < (1.0f / 3.0f)) yb = 0;
    else if (cy < (2.0f / 3.0f)) yb = 1;
    else                         yb = 2;
    return static_cast<uint8_t>(yb * 3 + xb);
}

CompositeKey concrete_key_for(const SpriteEntry* e, uint8_t frame_screen_ctx) {
    CompositeKey k;
    k.state_key      = e->state_key;
    k.uv_bucket      = compute_uv_bucket(e->inline_uvs);
    k.screen_context = frame_screen_ctx;
    k.bbox_quadrant  = compute_bbox_quadrant(e->inline_positions);
    return k;
}

// === Lookup primitives ======================================================
//
// All lookup helpers below assume the caller holds g_mu.

// Best-match lookup: among slots sharing the concrete key's state_key, find
// the one whose pattern matches AND has the highest specificity. Tiebreak
// by edit_seq (most-recent edit wins). Returns -1 if no match.
int lookup_best_match(const CompositeKey& concrete) {
    if (concrete.state_key == 0) return -1;
    auto it = g_state_index.find(concrete.state_key);
    if (it == g_state_index.end()) return -1;

    int best_idx  = -1;
    int best_spec = -1;
    uint64_t best_seq = 0;
    for (int idx : it->second) {
        const Slot& s = g_slots[idx];
        if (!key_matches_pattern(s.key, concrete)) continue;
        const int spec = key_specificity(s.key);
        if (spec > best_spec || (spec == best_spec && s.edit_seq > best_seq)) {
            best_idx  = idx;
            best_spec = spec;
            best_seq  = s.edit_seq;
        }
    }
    return best_idx;
}

// Find the wildcard slot for state_key — the legacy "v1" lookup used by
// the back-compat get_*/set_* APIs. Returns -1 if no wildcard exists.
int lookup_wildcard_slot(uint32_t state_key) {
    if (state_key == 0) return -1;
    auto it = g_state_index.find(state_key);
    if (it == g_state_index.end()) return -1;
    for (int idx : it->second) {
        if (g_slots[idx].key.is_wildcard_pattern()) return idx;
    }
    return -1;
}

// Allocate an empty slot (or evict LRU). Returns -1 on full table.
// Eviction policy: oldest last_seen_frame among slots that have NO
// user-edited state (wildcard pattern AND identity transform AND no
// label). User-edited slots are pinned. With kMaxKeys=64, this allows
// the user to label up to 64 slots without churn.
int alloc_slot() {
    int free_idx = -1;
    int lru_idx  = -1;
    uint32_t lru_frame = UINT32_MAX;
    for (int i = 0; i < kMaxKeys; ++i) {
        const Slot& s = g_slots[i];
        if (s.key.state_key == 0) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        // Slot is "touched" by the user if any external API has bumped
        // edit_seq, or any of the standard markers indicate user state.
        // Sticky edit_seq protects slots whose values were explicitly
        // reset to defaults — without it, "user reset offset to 0"
        // would fail has_user_state and the slot would silently evict,
        // erasing the value the user just typed.
        const bool has_user_state =
            s.edit_seq > 0 ||
            s.name[0] != 0 || s.group[0] != 0 ||
            s.offset_x != 0.0f || s.offset_y != 0.0f ||
            s.scale_x != 1.0f || s.scale_y != 1.0f ||
            s.hidden ||
            !s.key.is_wildcard_pattern();
        if (!has_user_state && s.last_seen_frame < lru_frame) {
            lru_frame = s.last_seen_frame;
            lru_idx   = i;
        }
    }
    int dst_idx = (free_idx >= 0) ? free_idx : lru_idx;
    if (dst_idx < 0) return -1;
    Slot& dst = g_slots[dst_idx];
    if (dst.key.state_key != 0) {
        index_remove(dst.key.state_key, dst_idx);
    }
    dst = {};
    return dst_idx;
}

// v1 back-compat: ensure a wildcard slot exists for state_key. Returns
// the slot index (or -1 on full table). Used by all v1-API paths.
int find_or_create_wildcard(uint32_t state_key) {
    if (state_key == 0) return -1;
    if (int idx = lookup_wildcard_slot(state_key); idx >= 0) return idx;
    int idx = alloc_slot();
    if (idx < 0) return -1;
    Slot& s = g_slots[idx];
    s.key.state_key      = state_key;
    s.key.uv_bucket      = kUVWildcard;
    s.key.screen_context = kScreenWildcard;
    s.key.bbox_quadrant  = kQuadWildcard;
    index_add(state_key, idx);
    return idx;
}

// Apply transform to a 4-vert XYZ position array (12 floats). Scale is
// applied around the per-entry centroid so "scale 0.5x" means "shrink in
// place" rather than "shrink toward origin". Offsets nudge the result.
void apply_transform(float* pos, float ox, float oy, float sx, float sy) {
    if (sx == 1.0f && sy == 1.0f && ox == 0.0f && oy == 0.0f) return;
    const float cx = (pos[0] + pos[3] + pos[6] + pos[9])  * 0.25f;
    const float cy = (pos[1] + pos[4] + pos[7] + pos[10]) * 0.25f;
    for (int v = 0; v < 4; ++v) {
        const float x = pos[v * 3 + 0];
        const float y = pos[v * 3 + 1];
        pos[v * 3 + 0] = (x - cx) * sx + cx + ox;
        pos[v * 3 + 1] = (y - cy) * sy + cy + oy;
    }
}

// === Phase A diagnostics ====================================================
// Per-frame distribution counters (always-on, single-frame snapshot) +
// a one-shot "dump entire entry list to CSV" trigger. The point of
// these is to settle empirically what differentiates pickable from
// non-pickable sprites — guesswork has produced two bad fixes already.
// Once we have the data, the right fix is obvious.

struct FrameDiag {
    uint32_t total            = 0;
    uint32_t state_key_zero   = 0;
    uint32_t ext_pos_used     = 0;
    uint32_t ext_uvs_used     = 0;
    uint32_t flag_bit_0x1     = 0;   // mask bit 0
    uint32_t flag_bit_0x2     = 0;
    uint32_t flag_bit_0x4     = 0;
    uint32_t flag_bit_0x100   = 0;   // alpha-mod-honoring
    uint32_t flag_bit_0x400   = 0;   // alpha-mod-honoring (alt)
    uint32_t flag_bit_other   = 0;   // anything outside the above set
    uint32_t degenerate_quad  = 0;   // zero-area in inline_positions
    uint32_t zero_alpha       = 0;
    uint32_t pickable_today   = 0;   // matches our current submit gate
};
FrameDiag g_diag{};

// One-shot CSV dump trigger. When set, the next process_list captures
// every entry verbatim and writes Game/mtr-asi-entries.csv. Cleared
// after the write completes so a second click is needed for a fresh
// capture (avoids accidentally streaming megs of data).
std::atomic<bool> g_csv_dump_requested{false};
std::atomic<uint32_t> g_csv_last_entry_count{0};

const char* g_csv_path_relative = "mtr-asi-entries.csv";

bool dump_entries_csv(SpriteEntry* head, const char* top_screen_name) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) return false;
    // Replace the executable filename with our CSV name. We're running
    // inside Game/<exe>.exe, so this lands next to the game executable.
    char* slash = nullptr;
    for (char* p = path; *p; ++p) if (*p == '\\' || *p == '/') slash = p;
    if (!slash) return false;
    *slash = 0;
    char full[MAX_PATH];
    if (std::snprintf(full, sizeof(full), "%s\\%s", path, g_csv_path_relative) <= 0)
        return false;

    FILE* fp = nullptr;
    if (fopen_s(&fp, full, "wb") != 0 || !fp) return false;

    // Header. Wide format so each row is self-describing in a spreadsheet.
    std::fprintf(fp,
        "idx,state_key,flags,alpha_mod,blend_mode,sort_key,"
        "ext_positions,ext_uvs,"
        "p0x,p0y,p0z,p1x,p1y,p1z,p2x,p2y,p2z,p3x,p3y,p3z,"
        "u0,v0,u1,v1,u2,v2,u3,v3,"
        "bbox_minx,bbox_miny,bbox_maxx,bbox_maxy,"
        "color0,color1,color2,color3,"
        "screen_top,pickable_today\n");

    uint32_t idx = 0;
    for (SpriteEntry* e = head; e; e = e->next, ++idx) {
        const float* pos = e->ext_positions ? e->ext_positions : e->inline_positions;
        const float* uvs = e->ext_uvs       ? e->ext_uvs       : e->inline_uvs;
        float minx = pos[0], maxx = pos[0];
        float miny = pos[1], maxy = pos[1];
        for (int v = 1; v < 4; ++v) {
            const float x = pos[v * 3 + 0];
            const float y = pos[v * 3 + 1];
            if (x < minx) minx = x; if (x > maxx) maxx = x;
            if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
        const bool pickable_today = (e->state_key != 0);
        std::fprintf(fp,
            "%u,0x%08X,0x%08X,%u,%u,%u,"
            "%p,%p,"
            "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%.6f,%.6f,%.6f,%.6f,"
            "0x%08X,0x%08X,0x%08X,0x%08X,"
            "%s,%d\n",
            idx, e->state_key, e->flags,
            (unsigned)e->alpha_mod, (unsigned)e->blend_mode, (unsigned)e->sort_key,
            (void*)e->ext_positions, (void*)e->ext_uvs,
            pos[0], pos[1], pos[2],
            pos[3], pos[4], pos[5],
            pos[6], pos[7], pos[8],
            pos[9], pos[10], pos[11],
            uvs[0], uvs[1], uvs[2], uvs[3],
            uvs[4], uvs[5], uvs[6], uvs[7],
            minx, miny, maxx, maxy,
            e->inline_colors[0], e->inline_colors[1],
            e->inline_colors[2], e->inline_colors[3],
            top_screen_name ? top_screen_name : "",
            pickable_today ? 1 : 0);
    }
    std::fclose(fp);
    g_csv_last_entry_count.store(idx);
    mtr::log::info("sprite_xform: entry CSV written (%u entries) -> %s",
                   idx, full);
    return true;
}

} // namespace

bool enabled()      { return g_enabled.load(); }
void set_enabled(bool v) {
    g_enabled.store(v);
    mtr::log::info("sprite_xform: enabled = %d", v ? 1 : 0);
}

// Group-drag mode: when on, the gizmo's per-frame translate / scale
// delta is applied to EVERY slot sharing the selected slot's group, not
// just the selected one. Lets the user grab one element and move the
// whole group together (e.g. recompose all HUD pieces at once).
std::atomic<bool> g_drag_group{false};
bool drag_group()             { return g_drag_group.load(); }
void set_drag_group(bool v)   { g_drag_group.store(v); }

// Auto-group toggle: when on, process_list derives group from the
// auto-named asset path (last directory component) for any slot whose
// group is empty AND has a path. Doesn't override manual groups.
std::atomic<bool> g_auto_group_from_path{false};
bool auto_group_from_path()           { return g_auto_group_from_path.load(); }
void set_auto_group_from_path(bool v) { g_auto_group_from_path.store(v); }

uint64_t last_total_entries() { return g_last_total_entries.load(); }

// Called from the wrapper before render_sprite_batcher runs. Walks the
// list, computes each entry's concrete composite key, looks up the best-
// matching slot, accumulates counts, stamps last_concrete on the slot,
// and (if enabled) applies transforms. Pure pass-through for entries
// with no matching slot.
void process_list() {
    SpriteEntry* head = *reinterpret_cast<SpriteEntry**>(kSpriteListHeadVA);
    if (!head) {
        g_last_total_entries.store(0);
        return;
    }
    const uint32_t fno = static_cast<uint32_t>(g_frame_no.fetch_add(1));
    // Sample screen context once per frame; all entries this frame share
    // it (the screen-stack only changes on push/pop boundaries, which sit
    // outside the per-frame render-list pipeline).
    const uint8_t frame_screen_ctx = compute_screen_context_for_frame();

    {
        std::scoped_lock lk(g_mu);
        for (auto& s : g_slots) s.frame_count = 0;
        g_highlight_boxes.clear();
    }
    mtr::sprite_picking::begin_frame();

    // Phase A: per-frame distribution tally + one-shot CSV dump. Walk
    // the list once read-only, BEFORE any of our mutations (so the CSV
    // captures the engine-emitted geometry, not our re-written
    // inline_positions). Tally is cheap; CSV path only runs when armed.
    FrameDiag diag{};
    constexpr uint32_t kKnownFlagBits = 0x1u | 0x2u | 0x4u | 0x100u | 0x400u;
    for (SpriteEntry* e = head; e; e = e->next) {
        ++diag.total;
        if (e->state_key == 0)   ++diag.state_key_zero;
        if (e->ext_positions)    ++diag.ext_pos_used;
        if (e->ext_uvs)          ++diag.ext_uvs_used;
        if (e->flags & 0x1u)     ++diag.flag_bit_0x1;
        if (e->flags & 0x2u)     ++diag.flag_bit_0x2;
        if (e->flags & 0x4u)     ++diag.flag_bit_0x4;
        if (e->flags & 0x100u)   ++diag.flag_bit_0x100;
        if (e->flags & 0x400u)   ++diag.flag_bit_0x400;
        if (e->flags & ~kKnownFlagBits) ++diag.flag_bit_other;
        if (e->alpha_mod == 0)   ++diag.zero_alpha;
        const float* pos = e->ext_positions ? e->ext_positions : e->inline_positions;
        const float w = (pos[3]  - pos[0]) + (pos[6]  - pos[9]);
        const float h = (pos[10] - pos[1]) + (pos[7]  - pos[4]);
        if ((w == 0.0f && h == 0.0f)) ++diag.degenerate_quad;
        if (e->state_key != 0) ++diag.pickable_today;
    }
    g_diag = diag;

    if (g_csv_dump_requested.exchange(false)) {
        char top[64] = {0};
        mtr::screen_push::current_top_name(top, sizeof(top));
        dump_entries_csv(head, top);
    }

    uint64_t total = 0;
    {
        std::scoped_lock lk(g_mu);
        for (SpriteEntry* e = head; e; e = e->next) {
            ++total;
            if (e->state_key == 0) continue;

            // Auto-create a wildcard slot the first time we see a new
            // state_key — preserves v1 behaviour ("the live list shows
            // every state_key currently rendering, transforms apply
            // when configured"). New variants only exist if the user
            // explicitly Specializes.
            if (g_state_index.find(e->state_key) == g_state_index.end()) {
                find_or_create_wildcard(e->state_key);
            }

            const CompositeKey concrete = concrete_key_for(e, frame_screen_ctx);
            const int slot_idx = lookup_best_match(concrete);
            if (slot_idx < 0) continue;

            Slot& s = g_slots[slot_idx];
            s.total_count++;
            s.frame_count++;
            s.last_seen_frame = fno;
            s.last_concrete = concrete;
            s.last_concrete_valid = true;

            // Auto-name (Phase D.4) + by-path persistence (Phase D.5): on
            // first sight of a slot, try reading the asset path from the
            // state_key target object's +0x2C field. Result populates BOTH
            // slot.path (always — used for cross-session matching) and
            // slot.name (only if name is empty — auto-fills the user-
            // facing label). The user can later type over the name; path
            // stays so persistence still resolves the slot's asset.
            //
            // After a successful path read, scan the pending-by-path queue
            // for a matching entry from the previous session's ini and
            // apply its transform/name/group/composite-key here.
            if (!s.auto_name_attempted) {
                char read_path[kNameLen] = {0};
                if (mtr::state_key_probe::read_state_key_path(
                        e->state_key, read_path, sizeof(read_path)) > 0) {
                    std::memcpy(s.path, read_path, kNameLen);
                    s.path[kNameLen - 1] = 0;
                    if (s.name[0] == 0) {
                        std::memcpy(s.name, read_path, kNameLen);
                        s.name[kNameLen - 1] = 0;
                        s.auto_named = true;
                    }
                    // Auto-group from path: derive the last-directory
                    // component as the group label, but only when the
                    // toggle is on AND the slot has no manual group yet.
                    // Manual groups always win — typing one in clears
                    // this auto-derivation for that slot.
                    if (g_auto_group_from_path.load() && s.group[0] == 0) {
                        char dg[kGroupLen] = {0};
                        derive_group_from_path(s.path, dg, sizeof(dg));
                        if (dg[0]) {
                            std::memcpy(s.group, dg, kGroupLen);
                            s.group[kGroupLen - 1] = 0;
                        }
                    }

                    // Resolve pending-by-path matches.
                    for (auto it = g_pending_by_path.begin(); it != g_pending_by_path.end(); ) {
                        if (std::strcmp(it->path, s.path) == 0) {
                            // Pattern: when pending entry has non-wildcard
                            // composite components, write them into the
                            // slot's pattern so it'll start matching only
                            // the same UV/screen/quad variant. When all
                            // wildcard (v1 / typical case), the slot
                            // stays a wildcard pattern.
                            const bool any_concrete =
                                it->uv_bucket      != 0xFFFF
                             || it->screen_context != 0xFF
                             || it->bbox_quadrant  != 0xFF;
                            if (any_concrete) {
                                s.key.uv_bucket      = it->uv_bucket;
                                s.key.screen_context = it->screen_context;
                                s.key.bbox_quadrant  = it->bbox_quadrant;
                            }
                            s.offset_x = it->offset_x;
                            s.offset_y = it->offset_y;
                            s.scale_x  = it->scale_x;
                            s.scale_y  = it->scale_y;
                            s.hidden   = it->hidden;
                            if (it->name[0]) {
                                std::strncpy(s.name, it->name, kNameLen - 1);
                                s.name[kNameLen - 1] = 0;
                                s.auto_named = false;
                            }
                            std::strncpy(s.group, it->group, kGroupLen - 1);
                            s.group[kGroupLen - 1] = 0;
                            s.edit_seq = g_edit_seq.fetch_add(1);
                            it = g_pending_by_path.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                s.auto_name_attempted = true;
            }

            if (!g_enabled.load()) continue;
            if (s.hidden) {
                // Hide reliably by collapsing the 4-vert quad to a single
                // point (zero area → 0 pixels rasterised). alpha_mod=0
                // alone is unreliable: per render_sprite_batcher, alpha_mod
                // only modulates color when the entry has flag bits 0x100
                // or 0x400 — most HUD entries lack those, so a pure
                // alpha_mod write is a no-op and the sprite stays visible.
                if (!(e->flags & 0x1)) {
                    for (int v = 0; v < 4; ++v) {
                        e->inline_positions[v * 3 + 0] = 0.0f;
                        e->inline_positions[v * 3 + 1] = 0.0f;
                        e->inline_positions[v * 3 + 2] = 0.0f;
                    }
                }
                e->alpha_mod = 0;  // belt-and-suspenders for 0x100/0x400 paths
                continue;
            }
            if (!(e->flags & 0x1)) {
                apply_transform(e->inline_positions,
                                s.offset_x, s.offset_y,
                                s.scale_x,  s.scale_y);
            }
            // Submit the post-transform quad to the picking module BEFORE
            // we compute the highlight bbox. We always submit (even for
            // flag-1 entries that apply_transform skipped), because the
            // user wants to PICK them regardless of whether transforms
            // currently work. Read from ext_positions when populated —
            // that's the geometry the engine actually rasterises for
            // many HUD entries (apply_transform leaves ext_positions
            // alone because it might be const memory; picking only
            // reads, so it's safe).
            const float* pick_pos = e->ext_positions
                                  ? e->ext_positions
                                  : e->inline_positions;
            mtr::sprite_picking::submit(slot_idx, e->state_key, pick_pos);
            if (s.highlight) {
                // Capture post-transform bbox so the menu can draw an
                // ImGui overlay rectangle on top of the game frame. The
                // engine-side alpha_mod write is a no-op for most HUD
                // entries (alpha modulation is gated on flag bits 0x100/
                // 0x400 which most entries lack) — so a pure overlay is
                // the only reliable visual indicator.
                //
                // Read from ext_positions when populated (matches what
                // the engine actually rasterises). Same logic as the
                // picking submission above.
                const float* hl_pos = e->ext_positions
                                    ? e->ext_positions
                                    : e->inline_positions;
                float x0 = FLT_MAX, y0 = FLT_MAX;
                float x1 = -FLT_MAX, y1 = -FLT_MAX;
                for (int v = 0; v < 4; ++v) {
                    const float x = hl_pos[v * 3 + 0];
                    const float y = hl_pos[v * 3 + 1];
                    if (x < x0) x0 = x;
                    if (y < y0) y0 = y;
                    if (x > x1) x1 = x;
                    if (y > y1) y1 = y;
                }
                g_highlight_boxes.push_back({x0, y0, x1, y1, e->state_key});
                e->alpha_mod = 255;  // best-effort for entries that honor it
            }
        }
    }
    mtr::sprite_picking::end_frame();
    g_last_total_entries.store(total);
}

// === UI accessors ===========================================================

// v1 snapshot: returns one row per *slot* with counts, sorted by
// frame_count desc. The row's state_key is shown in the legacy UI; the
// new v2 UI also reads back composite-key components via snapshot_slots
// to render variant rows distinctly.
int snapshot_keys(uint32_t* out_keys, uint32_t* out_frame_counts,
                  uint64_t* out_total_counts, int max_out) {
    Slot copy[kMaxKeys];
    {
        std::scoped_lock lk(g_mu);
        std::memcpy(copy, g_slots, sizeof(copy));
    }
    int n = 0;
    int idx[kMaxKeys];
    for (int i = 0; i < kMaxKeys; ++i) {
        if (copy[i].key.state_key != 0) idx[n++] = i;
    }
    for (int i = 1; i < n; ++i) {
        int j = i;
        while (j > 0 && copy[idx[j]].frame_count > copy[idx[j-1]].frame_count) {
            int t = idx[j]; idx[j] = idx[j-1]; idx[j-1] = t;
            --j;
        }
    }
    if (n > max_out) n = max_out;
    for (int i = 0; i < n; ++i) {
        out_keys[i]         = copy[idx[i]].key.state_key;
        out_frame_counts[i] = copy[idx[i]].frame_count;
        out_total_counts[i] = copy[idx[i]].total_count;
    }
    return n;
}

// v2 snapshot: returns one row per slot with full composite-key info.
// Used by the v2 UI to render variant rows. SlotInfo is intentionally
// POD-flat for ABI simplicity across the .cpp boundary.
struct SlotInfo {
    int      slot_idx;            // stable index into the slot table
    uint32_t state_key;
    uint16_t uv_bucket;           // 0xFFFF = wildcard
    uint8_t  screen_context;      // 0xFF   = wildcard
    uint8_t  bbox_quadrant;       // 0xFF   = wildcard
    uint16_t last_uv_bucket;      // 0xFFFF if !last_concrete_valid
    uint8_t  last_screen_context;
    uint8_t  last_bbox_quadrant;
    bool     last_concrete_valid;
    bool     auto_named;          // name was populated from state_key target memory
    uint32_t frame_count;
    uint64_t total_count;
    char     name[kNameLen];
    char     group[kGroupLen];
    float    offset_x, offset_y, scale_x, scale_y;
    bool     hidden;
};

int snapshot_slots(SlotInfo* out, int max_out) {
    Slot copy[kMaxKeys];
    {
        std::scoped_lock lk(g_mu);
        std::memcpy(copy, g_slots, sizeof(copy));
    }
    int n = 0;
    int idx[kMaxKeys];
    for (int i = 0; i < kMaxKeys; ++i) {
        if (copy[i].key.state_key != 0) idx[n++] = i;
    }
    // Sort by frame_count desc (matches v1 ordering).
    for (int i = 1; i < n; ++i) {
        int j = i;
        while (j > 0 && copy[idx[j]].frame_count > copy[idx[j-1]].frame_count) {
            int t = idx[j]; idx[j] = idx[j-1]; idx[j-1] = t;
            --j;
        }
    }
    if (n > max_out) n = max_out;
    for (int i = 0; i < n; ++i) {
        const Slot& s = copy[idx[i]];
        SlotInfo& o = out[i];
        o.slot_idx            = idx[i];
        o.state_key           = s.key.state_key;
        o.uv_bucket           = s.key.uv_bucket;
        o.screen_context      = s.key.screen_context;
        o.bbox_quadrant       = s.key.bbox_quadrant;
        o.last_uv_bucket      = s.last_concrete.uv_bucket;
        o.last_screen_context = s.last_concrete.screen_context;
        o.last_bbox_quadrant  = s.last_concrete.bbox_quadrant;
        o.last_concrete_valid = s.last_concrete_valid;
        o.auto_named          = s.auto_named;
        o.frame_count         = s.frame_count;
        o.total_count         = s.total_count;
        std::memcpy(o.name,  s.name,  kNameLen);
        std::memcpy(o.group, s.group, kGroupLen);
        o.offset_x = s.offset_x;
        o.offset_y = s.offset_y;
        o.scale_x  = s.scale_x;
        o.scale_y  = s.scale_y;
        o.hidden   = s.hidden;
    }
    return n;
}

struct Transform {
    float offset_x;
    float offset_y;
    float scale_x;
    float scale_y;
    bool  hidden;
};

// === v1 back-compat API (state_key only — operates on wildcard slot) ========

Transform get_transform(uint32_t state_key) {
    std::scoped_lock lk(g_mu);
    if (int idx = lookup_wildcard_slot(state_key); idx >= 0) {
        const Slot& s = g_slots[idx];
        return { s.offset_x, s.offset_y, s.scale_x, s.scale_y, s.hidden };
    }
    return { 0, 0, 1, 1, false };
}

void set_transform(uint32_t state_key, float ox, float oy, float sx, float sy, bool hidden) {
    std::scoped_lock lk(g_mu);
    int idx = find_or_create_wildcard(state_key);
    if (idx < 0) return;
    Slot& s = g_slots[idx];
    s.offset_x = ox;
    s.offset_y = oy;
    s.scale_x  = sx;
    s.scale_y  = sy;
    s.hidden   = hidden;
    s.edit_seq = g_edit_seq.fetch_add(1);
}

void set_highlight(uint32_t state_key, bool on) {
    std::scoped_lock lk(g_mu);
    if (int idx = lookup_wildcard_slot(state_key); idx >= 0) {
        g_slots[idx].highlight = on;
    }
}

void clear_all_highlights() {
    std::scoped_lock lk(g_mu);
    for (auto& s : g_slots) s.highlight = false;
}

// Public highlight-box snapshot. Menu draw path calls this each frame
// and renders an ImGui overlay rectangle for every box. Coordinates are
// engine-space (matches what the sprite-batcher feeds to draw); the
// menu converts them to screen pixels via a simple [0,1] → viewport
// mapping (good enough for typical menu sprites in [0,1]² space).
struct HighlightBox {
    float    x0, y0, x1, y1;
    uint32_t state_key;
};
int snapshot_highlight_boxes(HighlightBox* out, int max_out) {
    std::scoped_lock lk(g_mu);
    int n = static_cast<int>(g_highlight_boxes.size());
    if (n > max_out) n = max_out;
    for (int i = 0; i < n; ++i) {
        const auto& b = g_highlight_boxes[i];
        out[i] = { b.x0, b.y0, b.x1, b.y1, b.state_key };
    }
    return n;
}

void reset_transform(uint32_t state_key) {
    std::scoped_lock lk(g_mu);
    if (int idx = lookup_wildcard_slot(state_key); idx >= 0) {
        Slot& s = g_slots[idx];
        s.offset_x = 0; s.offset_y = 0;
        s.scale_x  = 1; s.scale_y  = 1;
        s.hidden = false; s.highlight = false;
        s.edit_seq = g_edit_seq.fetch_add(1);
    }
}

void get_name(uint32_t state_key, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    std::scoped_lock lk(g_mu);
    if (int idx = lookup_wildcard_slot(state_key); idx >= 0) {
        std::strncpy(out, g_slots[idx].name, out_size - 1);
        out[out_size - 1] = 0;
    }
}
void set_name(uint32_t state_key, const char* name) {
    std::scoped_lock lk(g_mu);
    int idx = find_or_create_wildcard(state_key);
    if (idx < 0) return;
    Slot& s = g_slots[idx];
    std::strncpy(s.name, name ? name : "", kNameLen - 1);
    s.name[kNameLen - 1] = 0;
    s.auto_named = false;  // user typed over the auto-name; treat as manual
    s.edit_seq = g_edit_seq.fetch_add(1);
}
void get_group(uint32_t state_key, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    std::scoped_lock lk(g_mu);
    if (int idx = lookup_wildcard_slot(state_key); idx >= 0) {
        std::strncpy(out, g_slots[idx].group, out_size - 1);
        out[out_size - 1] = 0;
    }
}
void set_group(uint32_t state_key, const char* group) {
    std::scoped_lock lk(g_mu);
    int idx = find_or_create_wildcard(state_key);
    if (idx < 0) return;
    Slot& s = g_slots[idx];
    std::strncpy(s.group, group ? group : "", kGroupLen - 1);
    s.group[kGroupLen - 1] = 0;
    s.edit_seq = g_edit_seq.fetch_add(1);
}

void reset_all_transforms() {
    std::scoped_lock lk(g_mu);
    for (auto& s : g_slots) {
        s.offset_x = 0; s.offset_y = 0;
        s.scale_x  = 1; s.scale_y  = 1;
        s.hidden = false; s.highlight = false;
    }
}

void forget_all_keys() {
    std::scoped_lock lk(g_mu);
    for (auto& s : g_slots) s = {};
    g_state_index.clear();
    g_pending_by_path.clear();
}

// Retroactive auto-grouping: walk every slot, and if it has a non-empty
// path AND empty group, derive the group from path. Returns the count
// of slots updated. Lets the user opt-in to auto-grouping after labels
// have already been built up; the per-frame auto_group_from_path toggle
// only acts on NEW slots' first auto-name moment.
int auto_group_all_from_paths() {
    std::scoped_lock lk(g_mu);
    int n = 0;
    for (auto& s : g_slots) {
        if (s.key.state_key == 0) continue;
        if (s.group[0] != 0) continue;          // respect manual groups
        if (s.path[0] == 0) continue;
        char dg[kGroupLen] = {0};
        derive_group_from_path(s.path, dg, sizeof(dg));
        if (!dg[0]) continue;
        std::memcpy(s.group, dg, kGroupLen);
        s.group[kGroupLen - 1] = 0;
        s.edit_seq = g_edit_seq.fetch_add(1);
        ++n;
    }
    return n;
}

// Group-drag delta application. Called by the gizmo when "drag affects
// whole group" is on. The selected slot already received its delta via
// the regular set_transform_at path — this function applies the SAME
// delta to every other slot sharing the selected slot's group, so the
// group moves / scales in unison.
//
// translate: edx/edy are engine-space deltas, added to offset_x/y.
// scale:     rx/ry are scale ratios (multiplicative on each member's
//            anchor scale, NOT on its current scale, so repeated frames
//            during a drag don't compound). The caller passes the
//            anchor map; we apply to each member.
//
// We don't anchor scale here because anchor_scale is captured per-slot
// at drag start. Instead, the gizmo computes the ratio from its single
// anchor (the picked slot's anchor) and we apply that ratio to each
// group member's CURRENT scale (i.e. multiplicatively each frame).
// To prevent compounding, the gizmo passes the full new scale_x/y for
// the picked slot and we use the RATIO between that new scale and the
// picked slot's current scale (1 if equal, < 1 / > 1 if changed).
//
// Simpler approach used here: the gizmo passes (delta_ox, delta_oy) for
// translate and (factor_sx, factor_sy) for scale, where factors are the
// per-frame multiplier. For a continuous drag we want the GROUP to
// move/scale by the same per-frame amount as the picked slot. The
// gizmo computes (new - anchor) for offset and (new / anchor) for
// scale, and passes those.
//
// Implementation choice: rather than per-member anchor tracking, we
// store delta-from-last-frame and apply incrementally. That's
// equivalent: integrating the per-frame deltas across the drag yields
// the total. No state survives between calls, so this fits the
// existing per-frame "set new transform" model.
void apply_group_translate_delta(const char* group, int exclude_slot_idx,
                                  float delta_ox, float delta_oy) {
    if (!group || group[0] == 0) return;
    if (delta_ox == 0.0f && delta_oy == 0.0f) return;
    std::scoped_lock lk(g_mu);
    for (int i = 0; i < kMaxKeys; ++i) {
        if (i == exclude_slot_idx) continue;
        Slot& s = g_slots[i];
        if (s.key.state_key == 0) continue;
        if (std::strcmp(s.group, group) != 0) continue;
        s.offset_x += delta_ox;
        s.offset_y += delta_oy;
        s.edit_seq = g_edit_seq.fetch_add(1);
    }
}

void apply_group_scale_factor(const char* group, int exclude_slot_idx,
                               float factor_sx, float factor_sy) {
    if (!group || group[0] == 0) return;
    if (factor_sx == 1.0f && factor_sy == 1.0f) return;
    std::scoped_lock lk(g_mu);
    for (int i = 0; i < kMaxKeys; ++i) {
        if (i == exclude_slot_idx) continue;
        Slot& s = g_slots[i];
        if (s.key.state_key == 0) continue;
        if (std::strcmp(s.group, group) != 0) continue;
        float nx = s.scale_x * factor_sx;
        float ny = s.scale_y * factor_sy;
        if (nx < 0.05f) nx = 0.05f; if (nx > 5.0f) nx = 5.0f;
        if (ny < 0.05f) ny = 0.05f; if (ny > 5.0f) ny = 5.0f;
        s.scale_x = nx;
        s.scale_y = ny;
        s.edit_seq = g_edit_seq.fetch_add(1);
    }
}

// Returns the group label of a slot (read-only snapshot). Used by the
// gizmo to determine which group to broadcast deltas to. Returns false
// (and writes "" to out) if the slot has no group or is invalid.
bool get_group_at_buf(int slot_idx, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys) return false;
    if (g_slots[slot_idx].key.state_key == 0) return false;
    std::strncpy(out, g_slots[slot_idx].group, out_size - 1);
    out[out_size - 1] = 0;
    return out[0] != 0;
}

// Phase D.5 — queue an entry for cross-session restoration. Called from
// ui_aspect_rules ini load for each persisted slot that has a non-empty
// path. When process_list later auto-names a slot whose path matches,
// this queue entry's state is applied and the entry is dropped.
//
// Pass kUVWildcard / kScreenWildcard / kQuadWildcard for v1-style
// wildcard patterns. Concrete components create / promote the matching
// slot to a specialised variant.
void add_pending_by_path(const char* path,
                         uint16_t uv_bucket,
                         uint8_t  screen_context,
                         uint8_t  bbox_quadrant,
                         float ox, float oy,
                         float sx, float sy, bool hidden,
                         const char* name, const char* group) {
    if (!path || !path[0]) return;
    PendingByPath p{};
    std::strncpy(p.path, path, kNameLen - 1);
    if (name)  std::strncpy(p.name,  name,  kNameLen - 1);
    if (group) std::strncpy(p.group, group, kGroupLen - 1);
    p.offset_x = ox;
    p.offset_y = oy;
    p.scale_x  = sx;
    p.scale_y  = sy;
    p.hidden   = hidden;
    p.uv_bucket      = uv_bucket;
    p.screen_context = screen_context;
    p.bbox_quadrant  = bbox_quadrant;
    std::scoped_lock lk(g_mu);
    g_pending_by_path.push_back(p);
}

// === v2 slot-index API (operates on a specific slot, used for variants) =====

Transform get_transform_at(int slot_idx) {
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys || g_slots[slot_idx].key.state_key == 0) {
        return { 0, 0, 1, 1, false };
    }
    const Slot& s = g_slots[slot_idx];
    return { s.offset_x, s.offset_y, s.scale_x, s.scale_y, s.hidden };
}

void set_transform_at(int slot_idx, float ox, float oy, float sx, float sy, bool hidden) {
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys) return;
    if (g_slots[slot_idx].key.state_key == 0) {
        // Slot was evicted between the UI read and our write. Log once
        // per occurrence so we can catch any remaining silent-fail
        // paths — the sticky edit_seq fix in alloc_slot's protection
        // ought to prevent this for any user-touched slot.
        static std::atomic<uint32_t> warn_count{0};
        if (warn_count.fetch_add(1) < 16) {
            mtr::log::info(
                "sprite_xform: set_transform_at on evicted slot %d "
                "(state_key=0); change discarded. Edit reached the wrong slot, "
                "or eviction ran between read and write.", slot_idx);
        }
        return;
    }
    Slot& s = g_slots[slot_idx];
    s.offset_x = ox;
    s.offset_y = oy;
    s.scale_x  = sx;
    s.scale_y  = sy;
    s.hidden   = hidden;
    s.edit_seq = g_edit_seq.fetch_add(1);
}

void reset_transform_at(int slot_idx) {
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys || g_slots[slot_idx].key.state_key == 0) return;
    Slot& s = g_slots[slot_idx];
    s.offset_x = 0; s.offset_y = 0;
    s.scale_x  = 1; s.scale_y  = 1;
    s.hidden = false; s.highlight = false;
    s.edit_seq = g_edit_seq.fetch_add(1);
}

void set_highlight_at(int slot_idx, bool on) {
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys || g_slots[slot_idx].key.state_key == 0) return;
    g_slots[slot_idx].highlight = on;
}

void get_name_at(int slot_idx, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys || g_slots[slot_idx].key.state_key == 0) return;
    std::strncpy(out, g_slots[slot_idx].name, out_size - 1);
    out[out_size - 1] = 0;
}
void set_name_at(int slot_idx, const char* name) {
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys || g_slots[slot_idx].key.state_key == 0) return;
    Slot& s = g_slots[slot_idx];
    std::strncpy(s.name, name ? name : "", kNameLen - 1);
    s.name[kNameLen - 1] = 0;
    s.auto_named = false;  // user typed over the auto-name; treat as manual
    s.edit_seq = g_edit_seq.fetch_add(1);
}
void get_group_at(int slot_idx, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys || g_slots[slot_idx].key.state_key == 0) return;
    std::strncpy(out, g_slots[slot_idx].group, out_size - 1);
    out[out_size - 1] = 0;
}
void set_group_at(int slot_idx, const char* group) {
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys || g_slots[slot_idx].key.state_key == 0) return;
    Slot& s = g_slots[slot_idx];
    std::strncpy(s.group, group ? group : "", kGroupLen - 1);
    s.group[kGroupLen - 1] = 0;
    s.edit_seq = g_edit_seq.fetch_add(1);
}

// Specialize: take a slot's last_concrete (the most recent matching
// concrete key) and create a NEW slot with that exact key — so the new
// slot's pattern matches ONLY entries with that exact uv/screen/quad
// combination. Inherits name/group/transform from the parent so the
// user's existing settings carry over.
//
// Returns the new slot's index, or -1 on error (table full, parent
// has no last_concrete yet, parent invalid).
int specialize_slot(int parent_slot_idx) {
    std::scoped_lock lk(g_mu);
    if (parent_slot_idx < 0 || parent_slot_idx >= kMaxKeys) return -1;
    const Slot& parent = g_slots[parent_slot_idx];
    if (parent.key.state_key == 0 || !parent.last_concrete_valid) return -1;

    // Don't double-specialise — if a slot already exists with the parent's
    // last_concrete components for this state_key, return its index.
    auto it = g_state_index.find(parent.key.state_key);
    if (it != g_state_index.end()) {
        for (int idx : it->second) {
            const Slot& s = g_slots[idx];
            if (s.key.state_key      == parent.last_concrete.state_key
             && s.key.uv_bucket      == parent.last_concrete.uv_bucket
             && s.key.screen_context == parent.last_concrete.screen_context
             && s.key.bbox_quadrant  == parent.last_concrete.bbox_quadrant) {
                return idx;
            }
        }
    }

    int idx = alloc_slot();
    if (idx < 0) return -1;
    Slot& s = g_slots[idx];
    s.key      = parent.last_concrete;
    s.last_concrete       = parent.last_concrete;
    s.last_concrete_valid = true;
    s.offset_x = parent.offset_x;
    s.offset_y = parent.offset_y;
    s.scale_x  = parent.scale_x;
    s.scale_y  = parent.scale_y;
    s.hidden   = parent.hidden;
    std::memcpy(s.name,  parent.name,  kNameLen);
    std::memcpy(s.group, parent.group, kGroupLen);
    s.edit_seq = g_edit_seq.fetch_add(1);
    index_add(s.key.state_key, idx);
    return idx;
}

// Remove a specific slot (used by UI's "Delete variant" button on
// non-wildcard slots — wildcards stay because they're the v1 default).
void remove_slot(int slot_idx) {
    std::scoped_lock lk(g_mu);
    if (slot_idx < 0 || slot_idx >= kMaxKeys) return;
    Slot& s = g_slots[slot_idx];
    if (s.key.state_key == 0) return;
    // Refuse to remove the wildcard slot via this API — the v1 path
    // uses forget_all_keys for that. Keeps the basic state_key entry
    // visible after a user creates a variant.
    if (s.key.is_wildcard_pattern()) return;
    index_remove(s.key.state_key, slot_idx);
    s = {};
}

// === Persistence ============================================================

bool is_identity_slot(const Slot& s) {
    // Treat auto-populated names as "no user-state" so we don't persist
    // them. They re-derive every session from the live state_key target's
    // path field, and persisting would freeze a stale value (since
    // state_keys themselves are session-bound). Only USER-typed names
    // should drive persistence.
    const bool has_user_name = (s.name[0] != 0) && !s.auto_named;
    return s.offset_x == 0.0f && s.offset_y == 0.0f
        && s.scale_x  == 1.0f && s.scale_y  == 1.0f
        && !s.hidden
        && !has_user_name && s.group[0] == 0
        && s.key.is_wildcard_pattern();
}

int save_count() {
    std::scoped_lock lk(g_mu);
    int n = 0;
    for (auto& s : g_slots) {
        if (s.key.state_key != 0 && !is_identity_slot(s)) ++n;
    }
    return n;
}

// Extended save: writes composite-key components alongside the v1 fields,
// plus the auto-derived asset path (Phase D.5 cross-session matching).
// Old callers using save_at (v1 schema) still work — they get the same
// state_key + transform + name + group, and silently ignore the new
// fields. ui_aspect_rules saves the full set via save_at_full.
bool save_at_full(int i, uint32_t* out_state_key,
                  uint16_t* out_uv_bucket,
                  uint8_t*  out_screen_context,
                  uint8_t*  out_bbox_quadrant,
                  float* out_ox, float* out_oy,
                  float* out_sx, float* out_sy,
                  bool* out_hidden,
                  char* out_name,  size_t out_name_sz,
                  char* out_group, size_t out_group_sz,
                  char* out_path,  size_t out_path_sz) {
    std::scoped_lock lk(g_mu);
    int n = 0;
    for (auto& s : g_slots) {
        if (s.key.state_key == 0 || is_identity_slot(s)) continue;
        if (n == i) {
            if (out_state_key)      *out_state_key      = s.key.state_key;
            if (out_uv_bucket)      *out_uv_bucket      = s.key.uv_bucket;
            if (out_screen_context) *out_screen_context = s.key.screen_context;
            if (out_bbox_quadrant)  *out_bbox_quadrant  = s.key.bbox_quadrant;
            if (out_ox) *out_ox = s.offset_x;
            if (out_oy) *out_oy = s.offset_y;
            if (out_sx) *out_sx = s.scale_x;
            if (out_sy) *out_sy = s.scale_y;
            if (out_hidden) *out_hidden = s.hidden;
            if (out_name && out_name_sz > 0) {
                // Auto-populated names re-derive every session from the
                // state_key target memory; don't freeze them in the
                // `name` field — write empty so the slot picks up the
                // live path next session. The path itself goes in the
                // separate out_path field below.
                if (s.auto_named) {
                    out_name[0] = 0;
                } else {
                    std::strncpy(out_name, s.name, out_name_sz - 1);
                    out_name[out_name_sz - 1] = 0;
                }
            }
            if (out_group && out_group_sz > 0) {
                std::strncpy(out_group, s.group, out_group_sz - 1);
                out_group[out_group_sz - 1] = 0;
            }
            if (out_path && out_path_sz > 0) {
                std::strncpy(out_path, s.path, out_path_sz - 1);
                out_path[out_path_sz - 1] = 0;
            }
            return true;
        }
        ++n;
    }
    return false;
}

// v1 back-compat — same as save_at_full but ignores composite components
// and path. Kept for any external consumer; mtr-asi's own ini path uses
// save_at_full so it sees the path field.
bool save_at(int i, uint32_t* out_state_key,
             float* out_ox, float* out_oy,
             float* out_sx, float* out_sy,
             bool* out_hidden,
             char* out_name,  size_t out_name_sz,
             char* out_group, size_t out_group_sz) {
    return save_at_full(i, out_state_key,
                        nullptr, nullptr, nullptr,
                        out_ox, out_oy, out_sx, out_sy,
                        out_hidden, out_name, out_name_sz,
                        out_group, out_group_sz,
                        nullptr, 0);
}

// Extended load: takes the composite-key components. Callers passing
// kUVWildcard / kScreenWildcard / kQuadWildcard (or omitting via the
// v1 load_apply wrapper) get a wildcard pattern slot, matching v1
// behaviour exactly.
void load_apply_full(uint32_t state_key,
                     uint16_t uv_bucket,
                     uint8_t  screen_context,
                     uint8_t  bbox_quadrant,
                     float ox, float oy,
                     float sx, float sy, bool hidden,
                     const char* name, const char* group) {
    if (state_key == 0) return;
    std::scoped_lock lk(g_mu);
    // For wildcard patterns we use the existing wildcard-or-create path;
    // for concrete patterns we always create a new slot (no merging),
    // since two identical concrete patterns shouldn't both exist post-
    // load anyway (user would have deduped pre-save).
    int idx;
    const bool is_wildcard =
        uv_bucket      == kUVWildcard
     && screen_context == kScreenWildcard
     && bbox_quadrant  == kQuadWildcard;
    if (is_wildcard) {
        idx = find_or_create_wildcard(state_key);
    } else {
        idx = alloc_slot();
        if (idx >= 0) {
            Slot& s = g_slots[idx];
            s.key.state_key      = state_key;
            s.key.uv_bucket      = uv_bucket;
            s.key.screen_context = screen_context;
            s.key.bbox_quadrant  = bbox_quadrant;
            index_add(state_key, idx);
        }
    }
    if (idx < 0) return;
    Slot& s = g_slots[idx];
    s.offset_x = ox;
    s.offset_y = oy;
    s.scale_x  = sx;
    s.scale_y  = sy;
    s.hidden   = hidden;
    if (name) {
        std::strncpy(s.name, name, kNameLen - 1);
        s.name[kNameLen - 1] = 0;
    }
    if (group) {
        std::strncpy(s.group, group, kGroupLen - 1);
        s.group[kGroupLen - 1] = 0;
    }
    s.edit_seq = g_edit_seq.fetch_add(1);
}

void load_apply(uint32_t state_key, float ox, float oy,
                float sx, float sy, bool hidden,
                const char* name, const char* group) {
    load_apply_full(state_key, kUVWildcard, kScreenWildcard, kQuadWildcard,
                    ox, oy, sx, sy, hidden, name, group);
}

// === Phase A diagnostic API =================================================

struct FrameDiagPublic {
    uint32_t total;
    uint32_t state_key_zero;
    uint32_t ext_pos_used;
    uint32_t ext_uvs_used;
    uint32_t flag_bit_0x1;
    uint32_t flag_bit_0x2;
    uint32_t flag_bit_0x4;
    uint32_t flag_bit_0x100;
    uint32_t flag_bit_0x400;
    uint32_t flag_bit_other;
    uint32_t degenerate_quad;
    uint32_t zero_alpha;
    uint32_t pickable_today;
};

FrameDiagPublic frame_diag() {
    FrameDiagPublic out{};
    out.total            = g_diag.total;
    out.state_key_zero   = g_diag.state_key_zero;
    out.ext_pos_used     = g_diag.ext_pos_used;
    out.ext_uvs_used     = g_diag.ext_uvs_used;
    out.flag_bit_0x1     = g_diag.flag_bit_0x1;
    out.flag_bit_0x2     = g_diag.flag_bit_0x2;
    out.flag_bit_0x4     = g_diag.flag_bit_0x4;
    out.flag_bit_0x100   = g_diag.flag_bit_0x100;
    out.flag_bit_0x400   = g_diag.flag_bit_0x400;
    out.flag_bit_other   = g_diag.flag_bit_other;
    out.degenerate_quad  = g_diag.degenerate_quad;
    out.zero_alpha       = g_diag.zero_alpha;
    out.pickable_today   = g_diag.pickable_today;
    return out;
}

void request_entry_csv_dump()       { g_csv_dump_requested.store(true); }
uint32_t last_csv_entry_count()     { return g_csv_last_entry_count.load(); }

} // namespace mtr::sprite_xform
