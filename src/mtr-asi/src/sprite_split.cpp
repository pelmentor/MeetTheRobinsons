// Sprite-batcher split-pass renderer (Phase 3 M3.3).
//
// Goal: per-entry classification — pillarbox menu sprites only, leave HUD
// (and cutscene letterbox) alone. Phase 2's auto_from_rules approach can't
// handle screens where HUD and menu coexist (pause overlay / tip popup) —
// the screen-stack top-of-stack name is identical (`WilburMainMenu` at
// depth 7) for ALL gameplay states, so screen-based gating can't tell them
// apart.
//
// Classifier (decided from M3.2 capture analysis — see
// research/findings/sprite-classification-findings.md):
//
//   entry is MENU iff all 4 vertices have x ∈ [0, 1] AND y ∈ [0, 1]
//
// MAIN_MENU and all menu screen captures kept positions strictly inside
// the unit square. GAMEPLAY_HUD, GAMEPLAY_TIP_WINDOW, GAMEPLAY_ESC_MENU,
// and CUTSCENE_MOM had positions extending outside (HUD edge anchors and
// letterbox bars).
//
// Implementation: two-pass render. We use the engine's own flag bit 0x40
// ("skip this entry") to gate which entries each pass renders. Pass 1
// renders HUD without pillarbox; pass 2 renders menu with pillarbox.
// Restore flags and clear the override after both passes.

#include <atomic>
#include <cstdint>

namespace mtr        { /* nothing */ }
namespace mtr::log   { void info(const char* fmt, ...); }
namespace mtr::aspect { float current(); }
namespace mtr::screen_push {
    bool current_top_name(char* out, size_t out_size);
}
namespace mtr::ui_aspect_rules {
    float resolve_aspect(const char* top_name);
}
namespace mtr::sprite_matrix {
    void set_pass_override_factor(float f);
}

namespace mtr::sprite_split {

namespace {

constexpr uintptr_t kSpriteListHeadVA      = 0x007271E8;
constexpr uintptr_t kRenderSpriteBatcherVA = 0x004E8D30;

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

using PFN_RenderSpriteBatcher = unsigned int (__cdecl*)();

std::atomic<bool>     g_enabled{false};
std::atomic<uint64_t> g_last_total{0};
std::atomic<uint64_t> g_last_menu_count{0};
std::atomic<uint64_t> g_last_hud_count{0};

// Snapshot buffers — sized to comfortably exceed observed peaks
// (~250 entries/frame in our captures). Static to avoid per-frame
// allocation. Single-threaded access (render thread only).
constexpr int kMaxEntries = 4096;
SpriteEntry*  g_entries[kMaxEntries];
uint32_t      g_saved_flags[kMaxEntries];
bool          g_is_menu[kMaxEntries];

// Vertex-bbox classifier: entry is "menu" if all 4 verts have x ∈ [0, 1]
// AND y ∈ [0, 1]. Inline positions are 4×XYZ floats at +0x28; if flag
// bit 0 is set, positions are at *ext_positions (same layout). Returns
// false on null ext pointer (defensive).
bool entry_is_menu(SpriteEntry* e) {
    const float* p = (e->flags & 0x1) ? e->ext_positions : e->inline_positions;
    if (!p) return false;
    for (int v = 0; v < 4; ++v) {
        const float x = p[v * 3 + 0];
        const float y = p[v * 3 + 1];
        if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return false;
    }
    return true;
}

float compute_pillarbox_factor() {
    char top[64] = {0};
    mtr::screen_push::current_top_name(top, sizeof(top));
    const float target = mtr::ui_aspect_rules::resolve_aspect(top);
    if (target <= 0.0f) return 1.0f;
    const float screen = mtr::aspect::current();
    if (screen <= 0.0f) return 1.0f;
    return target / screen;
}

} // namespace

bool enabled()              { return g_enabled.load(); }
void set_enabled(bool v) {
    g_enabled.store(v);
    mtr::log::info("sprite_split: enabled = %d", v ? 1 : 0);
}

uint64_t last_total()       { return g_last_total.load(); }
uint64_t last_menu_count()  { return g_last_menu_count.load(); }
uint64_t last_hud_count()   { return g_last_hud_count.load(); }

// Run the two-pass split rendering. Caller is `wrapper_render_sprite_batcher`
// in sprite_probe.cpp, which intercepts the single call site at 0x4D23BF.
// `orig` is the canonical render_sprite_batcher (sub_4E8D30).
//
// Returns whatever the second invocation of orig returns (the engine
// doesn't use this return value upstream).
unsigned int execute_split() {
    SpriteEntry* head = *reinterpret_cast<SpriteEntry**>(kSpriteListHeadVA);
    auto orig = reinterpret_cast<PFN_RenderSpriteBatcher>(kRenderSpriteBatcherVA);

    // Empty list — nothing to split. Just call orig.
    if (!head) return orig();

    // Walk + classify. Bound by kMaxEntries to keep static buffers safe.
    int n = 0;
    for (SpriteEntry* e = head; e && n < kMaxEntries; e = e->next, ++n) {
        g_entries[n]      = e;
        g_saved_flags[n]  = e->flags;
        g_is_menu[n]      = entry_is_menu(e);
    }

    uint64_t menu_n = 0, hud_n = 0;
    for (int i = 0; i < n; ++i) {
        if (g_is_menu[i]) ++menu_n; else ++hud_n;
    }
    g_last_total.store(static_cast<uint64_t>(n));
    g_last_menu_count.store(menu_n);
    g_last_hud_count.store(hud_n);

    const float pillarbox_f = compute_pillarbox_factor();

    // Pass 1: render HUD only, no pillarbox.
    //   Set bit 0x40 on menu entries → engine pass 1 sets bit 0x80 on
    //   them → pass 2 skips them. Clear bit 0x80 on HUD entries so they
    //   render normally.
    for (int i = 0; i < n; ++i) {
        const uint32_t f = g_saved_flags[i] & ~0x80u;
        g_entries[i]->flags = g_is_menu[i] ? (f | 0x40u) : f;
    }
    mtr::sprite_matrix::set_pass_override_factor(1.0f);  // explicit passthrough
    orig();

    // Pass 2: render menu only, with pillarbox.
    //   Same scheme inverted. Clear 0x80 on menu entries (they were
    //   marked consumed during pass 1's bit-0x40 path).
    for (int i = 0; i < n; ++i) {
        const uint32_t f = g_saved_flags[i] & ~0x80u;
        g_entries[i]->flags = g_is_menu[i] ? f : (f | 0x40u);
    }
    mtr::sprite_matrix::set_pass_override_factor(pillarbox_f);
    unsigned int rc = orig();

    // Restore. The engine may have modified bit 0x80 on entries during
    // either pass; we explicitly write back the original flags so any
    // downstream code (or next frame) sees a clean state.
    for (int i = 0; i < n; ++i) {
        g_entries[i]->flags = g_saved_flags[i];
    }
    mtr::sprite_matrix::set_pass_override_factor(0.0f);

    return rc;
}

} // namespace mtr::sprite_split
