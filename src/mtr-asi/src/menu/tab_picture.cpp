// Display tab: borderless fullscreen window mode + 3D world aspect ratio +
// HUD/menu pillarboxing rules + per-element sprite controls (sprite_xform
// pin/specialize/auto-group) + diagnostic probes.
//
// This is the largest tab in the menu — it owns every visual / 2D-aspect
// override the mod exposes. Future split: borderless + aspect + HUD aspect
// stay here as "Picture"; the [diagnostic] sprite probe / vis_test_probe /
// scene_vis_log / state_key dump sections move to a dedicated Debug tab.
// Until that happens, the diagnostic blocks live behind TreeNode collapses
// inside the existing sections so the visible UI stays compact.

#include "menu_internal.h"
#include "imgui.h"

#include <windows.h>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mtr::log { void info(const char* fmt, ...); }
namespace mtr::d3d9hook { int last_pp_width(); int last_pp_height(); }
namespace mtr::aspect {
    bool  available();
    float current();
    float original();
    bool  set(float value);
}
namespace mtr::scene {
    bool fog_disabled();         void set_fog_disabled(bool v);
    bool side_cull_disabled();   void set_side_cull_disabled(bool v);
}
namespace mtr::force_vis {
    bool active();
    void set(bool on);
}
namespace mtr::vis_test_probe {
    bool active();
    bool force_pass();
    void set_force_pass(bool v);
    uint64_t cum_total();
    uint64_t cum_pass();
    uint64_t last_frame_total();
    uint64_t last_frame_pass();
    uint64_t last_frame_site(size_t i);
    const char* site_tag(size_t i);
    size_t num_sites();
}
namespace mtr::scene_vis_log {
    uint64_t last_hides();
    uint64_t last_shows();
    uint64_t last_script_calls();
    uint64_t last_script_hides();
    uint64_t last_script_shows();
    uint64_t cum_hides();
    uint64_t cum_shows();
    uint64_t cum_script_calls();
    int sticky_scene_count();
    uint32_t sticky_scene_at(int idx);
}
namespace mtr::sprite_probe {
    bool     installed();
    bool     armed();
    int      frames_remaining();
    uint64_t total_captured();
    uint64_t last_frame_count();
    void     arm(int frame_budget);
    void     disarm();
    bool     csv_path(char* out, size_t out_size);
}
namespace mtr::sprite_split {
    bool     enabled();
    void     set_enabled(bool v);
    uint64_t last_total();
    uint64_t last_menu_count();
    uint64_t last_hud_count();
}
namespace mtr::sprite_xform {
    bool     enabled();
    void     set_enabled(bool v);
    uint64_t last_total_entries();
    int      snapshot_keys(uint32_t* out_keys, uint32_t* out_frame_counts,
                           uint64_t* out_total_counts, int max_out);
    struct Transform { float offset_x, offset_y, scale_x, scale_y; bool hidden; };
    Transform get_transform(uint32_t state_key);
    void      set_transform(uint32_t state_key, float ox, float oy,
                            float sx, float sy, bool hidden);
    void      set_highlight(uint32_t state_key, bool on);
    void      clear_all_highlights();
    void      reset_transform(uint32_t state_key);
    void      reset_all_transforms();
    void      forget_all_keys();
    void      get_name(uint32_t state_key, char* out, size_t out_size);
    void      set_name(uint32_t state_key, const char* name);
    void      get_group(uint32_t state_key, char* out, size_t out_size);
    void      set_group(uint32_t state_key, const char* group);

    struct SlotInfo {
        int      slot_idx;
        uint32_t state_key;
        uint16_t uv_bucket;
        uint8_t  screen_context;
        uint8_t  bbox_quadrant;
        uint16_t last_uv_bucket;
        uint8_t  last_screen_context;
        uint8_t  last_bbox_quadrant;
        bool     last_concrete_valid;
        bool     auto_named;
        uint32_t frame_count;
        uint64_t total_count;
        char     name[48];
        char     group[32];
        float    offset_x, offset_y, scale_x, scale_y;
        bool     hidden;
    };
    int       snapshot_slots(SlotInfo* out, int max_out);
    Transform get_transform_at(int slot_idx);
    void      set_transform_at(int slot_idx, float ox, float oy,
                               float sx, float sy, bool hidden);
    void      reset_transform_at(int slot_idx);
    void      set_highlight_at(int slot_idx, bool on);
    void      get_name_at (int slot_idx, char* out, size_t out_size);
    void      set_name_at (int slot_idx, const char* name);
    void      get_group_at(int slot_idx, char* out, size_t out_size);
    void      set_group_at(int slot_idx, const char* group);
    int       specialize_slot(int parent_slot_idx);
    void      remove_slot(int slot_idx);
    struct HighlightBox { float x0, y0, x1, y1; uint32_t state_key; };
    int       snapshot_highlight_boxes(HighlightBox* out, int max_out);
    bool      drag_group();
    void      set_drag_group(bool v);
    bool      get_group_at_buf(int slot_idx, char* out, size_t out_size);
    void      apply_group_translate_delta(const char* group, int exclude_slot_idx,
                                          float delta_ox, float delta_oy);
    void      apply_group_scale_factor(const char* group, int exclude_slot_idx,
                                       float factor_sx, float factor_sy);
    bool      auto_group_from_path();
    void      set_auto_group_from_path(bool v);
    int       auto_group_all_from_paths();
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
    FrameDiagPublic frame_diag();
    void     request_entry_csv_dump();
    uint32_t last_csv_entry_count();
}
namespace mtr::state_key_probe {
    int      dump_all_to_csv();
    bool     csv_path(char* out, size_t out_size);
    uint64_t last_dump_count();
}
namespace mtr::sprite_matrix {
    bool  enabled();
    void  set_enabled(bool v);
    bool  auto_from_rules();
    void  set_auto_from_rules(bool v);
    float mul_a_a1(); float mul_a_a2(); float mul_a_a3();
    float mul_b_a1(); float mul_b_a2(); float mul_b_a3();
    void  set_mul_a(float a1, float a2, float a3);
    void  set_mul_b(float a1, float a2, float a3);
    void  reset();
    float pos_offset_x();
    float pos_offset_y();
    float pass_override_factor();
    void  set_pos_offset_x(float v);
    void  set_pos_offset_y(float v);
    struct Factors { float Fx; float Fy; float dx; float dy; };
    Factors current_factors();
}
namespace mtr::sprite_picking {
    int  pick_engine(float ex, float ey);
    int  pick_engine_at(float ex, float ey, int layer_index);
    int  selected();
    void set_selected(int slot_idx);
    void clear_selection();
    bool pick_mode();
    void set_pick_mode(bool on);
    int  quad_count();
    void quad_at(int i, int* slot_idx_out, uint32_t* state_key_out,
                 float* xy8_out, int* render_order_out);
}
namespace mtr::screen_push {
    bool current_top_name(char* out, size_t out_size);
    int  stack_depth();
    bool stack_at(int idx, char* out, size_t out_size);
    int  registered_count();
    bool registered_at(int idx, char* out, size_t out_size);
}
namespace mtr::ui_aspect_rules {
    size_t max_rules();
    size_t rule_count();
    bool   get_rule(size_t idx, char* out_pattern, size_t pattern_size, float* out_aspect);
    void   set_rule(size_t idx, const char* pattern, float aspect);
    void   add_rule(const char* pattern, float aspect);
    void   remove_rule(size_t idx);
    float  resolve_aspect(const char* top_name);
    bool   resolve_match(const char* top_name, char* out_pattern, size_t pattern_size, float* out_aspect);
    void   install_defaults();
    void   clear_all();
    void   save_to_ini();
    bool   load_from_ini();
    void   request_save();
    void   flush_pending_save();
    int    sync_from_registry();
    bool   auto_sync_screens();
    void   set_auto_sync_screens(bool v);
}
namespace mtr::windowmode {
    bool enabled();
    void set_enabled(bool on);
    unsigned long long create_device_rewrites();
    unsigned long long reset_rewrites();
    unsigned long long change_display_settings_blocks();
    HWND last_styled_window();
    int  last_monitor_w();
    int  last_monitor_h();
}
namespace mtr::fps_limit {
    bool enabled();
    int  current();
    void set(int fps);
    int  monitor_refresh_hz();
}

namespace mtr::menu::detail {

// === Picture tab: borderless / aspect / HUD aspect / FPS limit / overlay ====
void tab_picture() {
    if (bool open = section_begin("Borderless fullscreen window")) {
        heading_with_info("Native borderless window mode",
            "Replicates dxwrapper's FullscreenWindowMode without dxwrapper. "
            "Phase B of the DXVK migration (mtr-asi handles this natively "
            "so dxwrapper can be retired entirely).\n\n"
            "When ON:\n"
            "  - The D3D device is created in WINDOWED mode (no exclusive "
            "fullscreen mode change of the desktop).\n"
            "  - The game window is restyled to borderless and resized to "
            "fill the monitor at (0,0).\n"
            "  - ChangeDisplaySettings* calls are no-ops, so the game can't "
            "yank the desktop into a 4:3 mode.\n\n"
            "When OFF the game runs as if no wrapper was present: exclusive "
            "fullscreen + display-mode change, which matches the engine's "
            "default but loses HDR / multi-monitor / quick-tab benefits.\n\n"
            "Default ON. Persisted to mtr-asi-ui.ini under [windowmode].");

        bool wm_on = mtr::windowmode::enabled();
        if (ImGui::Checkbox("Borderless windowed at monitor size##wm_en", &wm_on)) {
            mtr::windowmode::set_enabled(wm_on);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(takes effect on next CreateDevice / Reset)");

        // Diagnostics — verifies the rewrite is firing as expected.
        const HWND wnd  = mtr::windowmode::last_styled_window();
        const int  mw   = mtr::windowmode::last_monitor_w();
        const int  mh   = mtr::windowmode::last_monitor_h();
        const unsigned long long n_create = mtr::windowmode::create_device_rewrites();
        const unsigned long long n_reset  = mtr::windowmode::reset_rewrites();
        const unsigned long long n_block  = mtr::windowmode::change_display_settings_blocks();
        ImGui::Spacing();
        ImGui::TextDisabled("Last styled hwnd : %p", wnd);
        ImGui::TextDisabled("Monitor size     : %d x %d", mw, mh);
        ImGui::TextDisabled("CreateDevice rewrites      : %llu", n_create);
        ImGui::TextDisabled("Reset rewrites             : %llu", n_reset);
        ImGui::TextDisabled("ChangeDisplaySettings blocks: %llu", n_block);
        section_end(open);
    }

    if (bool open = section_begin("3D world aspect ratio")) {
        if (!mtr::aspect::available()) {
            ImGui::TextDisabled("Aspect constant not found in the game executable.");
        } else {
            const float cur = mtr::aspect::current();
            const float orig = mtr::aspect::original();
            const float bb_w = static_cast<float>(mtr::d3d9hook::last_pp_width());
            const float bb_h = static_cast<float>(mtr::d3d9hook::last_pp_height());
            const float monitor_aspect = (bb_h > 0.0f) ? (bb_w / bb_h) : 0.0f;
            ImGui::Text("Current  %.4f", cur);
            ImGui::Text("Original %.4f     Monitor %.4f", orig, monitor_aspect);
            ImGui::Spacing();

            if (ImGui::Button("4:3"))   mtr::aspect::set(4.0f / 3.0f);   ImGui::SameLine();
            if (ImGui::Button("16:10")) mtr::aspect::set(16.0f / 10.0f); ImGui::SameLine();
            if (ImGui::Button("16:9"))  mtr::aspect::set(16.0f / 9.0f);  ImGui::SameLine();
            if (ImGui::Button("21:9"))  mtr::aspect::set(21.0f / 9.0f);  ImGui::SameLine();
            if (monitor_aspect > 0.0f && ImGui::Button("Monitor")) mtr::aspect::set(monitor_aspect);
            ImGui::SameLine();
            if (ImGui::Button("Restore")) mtr::aspect::set(orig);

            static float custom = 16.0f / 9.0f;
            ImGui::SliderFloat("##aspect", &custom, 0.5f, 5.5f, "%.4f");
            ImGui::SameLine();
            if (ImGui::Button("Apply##aspect")) mtr::aspect::set(custom);
        }
        section_end(open);
    }

    if (bool open = section_begin("HUD and menu aspect ratio")) {
        heading_with_info("Per-screen pillarbox for 2D UI",
            "Pillarboxes 2D UI elements (HUD, menus, mini-games, loading "
            "screens) so they display at their authored aspect on a "
            "widescreen monitor instead of stretching. The first matching "
            "rule by screen-name pattern wins. Aspect = 0 means \"don't "
            "pillarbox this screen\".");
        ImGui::Spacing();

        if (primary_button("Enable 4:3 pillarbox for HUD and menus (recommended)",
                           ImVec2(-FLT_MIN, 32.0f))) {
            mtr::ui_aspect_rules::install_defaults();
            mtr::sprite_matrix::set_enabled(true);
            mtr::sprite_matrix::set_auto_from_rules(true);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::Spacing();

        bool en = mtr::sprite_matrix::enabled();
        if (ImGui::Checkbox("On / off##sprite_master", &en)) {
            mtr::sprite_matrix::set_enabled(en);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::SameLine();
        bool auto_mode = mtr::sprite_matrix::auto_from_rules();
        if (ImGui::Checkbox("Use per-screen rules##sprite_auto", &auto_mode)) {
            mtr::sprite_matrix::set_auto_from_rules(auto_mode);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::Spacing();

        // Live position offset (drag to nudge UI). Applied after the
        // pillarbox factor. Lets the user fine-tune UI placement without
        // recompressing.
        float ox = mtr::sprite_matrix::pos_offset_x();
        float oy = mtr::sprite_matrix::pos_offset_y();
        ImGui::TextUnformatted("Nudge UI position:");
        ImGui::SameLine();
        info_pill(
            "Drag the slider to shift the entire UI. Ctrl+click to type a "
            "value. Shift = larger step, Alt = finer step. Range -5..+5.");
        constexpr ImGuiSliderFlags kGlobalDragFlags = ImGuiSliderFlags_AlwaysClamp;
        if (ImGui::DragFloat("X offset##posoffx", &ox, 0.001f, -5.0f, 5.0f, "%.4f", kGlobalDragFlags)) {
            mtr::sprite_matrix::set_pos_offset_x(ox);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) mtr::ui_aspect_rules::request_save();
        if (ImGui::DragFloat("Y offset##posoffy", &oy, 0.001f, -5.0f, 5.0f, "%.4f", kGlobalDragFlags)) {
            mtr::sprite_matrix::set_pos_offset_y(oy);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) mtr::ui_aspect_rules::request_save();
        if (ImGui::Button("Center (reset offsets)##pos_reset")) {
            mtr::sprite_matrix::set_pos_offset_x(0.0f);
            mtr::sprite_matrix::set_pos_offset_y(0.0f);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::Spacing();

        // ---- [experimental] split-pass toggle ------------------------------
        // Per-entry vertex-bbox classification (Phase 3 M3.3). Brittle on
        // menu frames that extend slightly past [0,1]² — they get
        // misclassified as HUD and the split looks wrong. Off by default;
        // kept as research scaffolding.
        bool split_on = mtr::sprite_split::enabled();
        if (ImGui::TreeNode("[experimental] Split menu vs HUD pillarboxing##split_tree")) {
            ImGui::SameLine();
            info_pill(
                "Tries to pillarbox only menu sprites and leave HUD elements "
                "alone, by inspecting each sprite's screen bounds. Brittle - "
                "menu frames that extend slightly past the screen get "
                "classified as HUD. Use the nudge sliders above instead for "
                "fine-tuning.");
            if (ImGui::Checkbox("On / off##split", &split_on)) {
                mtr::sprite_split::set_enabled(split_on);
            }
            if (split_on) {
                const uint64_t st  = mtr::sprite_split::last_total();
                const uint64_t smn = mtr::sprite_split::last_menu_count();
                const uint64_t shd = mtr::sprite_split::last_hud_count();
                ImGui::TextDisabled("  last frame: %llu sprites (menu=%llu, HUD=%llu)",
                    static_cast<unsigned long long>(st),
                    static_cast<unsigned long long>(smn),
                    static_cast<unsigned long long>(shd));
            }
            ImGui::TreePop();
        }
        ImGui::Spacing();

        // ---- Per-element control (sprite_xform) ---------------------------
        // Granular per-state_key transforms: walks the sprite list each
        // frame, lets the user pick a key from the live "currently
        // rendering" list, and modify its inline vertex positions
        // (offset / scale / hide) in place.
        //
        // state_key is a session-stable per-asset identifier (heap pointer
        // or texture handle) — same value across frames within a session
        // for the same sprite. So the user picks "the wilbur smile"
        // visually (via highlight or by hiding others), then dials in a
        // transform that follows it across frames.
        //
        // Live-only — state_keys change across game sessions, so the
        // transforms don't persist to ini (and would be misapplied if
        // they did).
        if (ImGui::TreeNode("Per-element control (live)##xform_tree")) {
            ImGui::SameLine();
            info_pill(
                "Move, scale or hide individual UI elements. Each row is one "
                "tracked element; most rows are wildcards that match every "
                "sprite sharing the same asset. If renaming \"menu text\" "
                "also affects HUD text (because they share an atlas), click "
                "\"Specialize\" to split off a child entry pinned to one "
                "specific occurrence — then you can move them independently. "
                "Specialized entries persist; wildcard transforms don't.\n\n"
                "Sliders: drag to change, Ctrl+click to type, Shift = larger "
                "step, Alt = finer step.");

            bool xon = mtr::sprite_xform::enabled();
            if (ImGui::Checkbox("On / off##xform_en", &xon)) {
                mtr::sprite_xform::set_enabled(xon);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(needed for moves and hides to apply)");
            ImGui::TextDisabled("Total UI elements this frame: %llu",
                static_cast<unsigned long long>(mtr::sprite_xform::last_total_entries()));
            ImGui::Spacing();

            if (warning_button("Reset all transforms##xform_reset_all")) {
                mtr::sprite_xform::reset_all_transforms();
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            if (danger_button("Forget all keys##xform_forget")) {
                mtr::sprite_xform::forget_all_keys();
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("[diagnostic] Dump asset memory to CSV##sk_probe")) {
                int n = mtr::state_key_probe::dump_all_to_csv();
                mtr::log::info("state_key probe: %d slots dumped", n);
            }
            ImGui::SameLine();
            info_pill(
                "[Research aid.] Dumps each tracked element's underlying "
                "asset memory to Game/mtr-asi-state-key-probe.csv for "
                "offline analysis. Used to map asset names so elements can "
                "be auto-labelled instead of manually named.");
            ImGui::Spacing();

            if (ImGui::TreeNode("[diagnostic] Sprite stats and CSV dump##xform_diag")) {
                ImGui::SameLine();
                info_pill(
                    "Per-frame counters of what the sprite renderer sees: "
                    "how many entries are unpickable, how many have external "
                    "geometry, the flag-bit distribution, etc.\n\n"
                    "\"Dump entries CSV\" captures one full frame to "
                    "Game/mtr-asi-entries.csv for offline review.");

                const auto d = mtr::sprite_xform::frame_diag();
                ImGui::Text("Entries this frame: %u", d.total);
                ImGui::Text("  unpickable (no key) : %u", d.state_key_zero);
                ImGui::Text("  external geometry   : %u", d.ext_pos_used);
                ImGui::Text("  external UVs        : %u", d.ext_uvs_used);
                ImGui::Text("  flag bit 0x001      : %u", d.flag_bit_0x1);
                ImGui::Text("  flag bit 0x002      : %u", d.flag_bit_0x2);
                ImGui::Text("  flag bit 0x004      : %u", d.flag_bit_0x4);
                ImGui::Text("  alpha-honoring      : %u", d.flag_bit_0x100);
                ImGui::Text("  alpha-honoring alt  : %u", d.flag_bit_0x400);
                ImGui::Text("  other flag bits     : %u", d.flag_bit_other);
                ImGui::Text("  degenerate quads    : %u", d.degenerate_quad);
                ImGui::Text("  fully transparent   : %u", d.zero_alpha);
                ImGui::Text("  pickable            : %u / %u",
                            d.pickable_today, d.total);
                ImGui::Spacing();
                if (ImGui::Button("Dump entries CSV##xform_dump_entries")) {
                    mtr::sprite_xform::request_entry_csv_dump();
                }
                const uint32_t n_csv = mtr::sprite_xform::last_csv_entry_count();
                if (n_csv > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(
                        "last dump: %u entries -> Game/mtr-asi-entries.csv",
                        n_csv);
                }
                ImGui::TreePop();
            }
            ImGui::Spacing();

            // ---- Pin / capture + snapshot of all live slots -------------------
            // The displayed list is one row per *slot* (not per state_key).
            // Most state_keys have one slot — a wildcard pattern that
            // matches all entries with that asset (= v1 behaviour). When
            // the user clicks "Specialize" on a row, a NEW slot is created
            // with the row's last-concrete composite key (uv_bucket +
            // screen_context + bbox_quadrant), so the same state_key now
            // has two rows: the wildcard parent and the specialised child.
            // Different transforms can be dialled into each.
            //
            // Pin mode freezes the displayed list at the moment of toggle,
            // defeating the "dancing list" caused by animated UI. Edits
            // still flow through to the live slots immediately; only the
            // ROW LIST is frozen.
            constexpr int kMaxRows = 64;
            using SlotInfo = mtr::sprite_xform::SlotInfo;
            static bool     s_pinned = false;
            static SlotInfo s_pinned_slots[kMaxRows]{};
            static int      s_pinned_n = 0;

            auto recapture_pin = [&]() {
                s_pinned_n = mtr::sprite_xform::snapshot_slots(s_pinned_slots, kMaxRows);
            };

            bool pin_toggle = s_pinned;
            if (ImGui::Checkbox("Freeze list##xform_pin", &pin_toggle)) {
                s_pinned = pin_toggle;
                if (s_pinned) recapture_pin();
            }
            ImGui::SameLine();
            info_pill(
                "Freezes the row order so the list stops reshuffling while "
                "you're labelling things. Useful for animated UI. Edits to "
                "name / group / transforms still apply live - only the row "
                "order is frozen.");

            ImGui::SameLine();
            bool pick_on = mtr::sprite_picking::pick_mode();
            if (ImGui::Checkbox("Click to pick##xform_pick", &pick_on)) {
                mtr::sprite_picking::set_pick_mode(pick_on);
                if (!pick_on) mtr::sprite_picking::clear_selection();
            }
            ImGui::SameLine();
            info_pill(
                "Click a UI element in the game to select its row in the "
                "list. The selected element gets a cyan outline. Click again "
                "on overlapping elements to cycle through layers. Esc to "
                "deselect. Clicks inside the menu still drive the menu.");

            if (mtr::sprite_picking::selected() >= 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Deselect##xform_desel")) {
                    mtr::sprite_picking::clear_selection();
                }
            }

            // Group-drag toggle: when on, gizmo deltas apply to every
            // slot sharing the selected slot's group, not just the
            // picked one. Lets the user grab one element and shift the
            // whole HUD or menu en bloc.
            ImGui::SameLine();
            bool dg = mtr::sprite_xform::drag_group();
            if (ImGui::Checkbox("Drag group##xform_dg", &dg)) {
                mtr::sprite_xform::set_drag_group(dg);
            }
            ImGui::SameLine();
            info_pill(
                "When on, dragging the move/scale handle on one element in "
                "a group also moves every other element in that group. Right-"
                "click reset affects the whole group too. Elements not in "
                "any group are unaffected. Use Auto-group below to bulk-"
                "create groups from the asset directory names.");

            ImGui::SameLine();
            bool ag = mtr::sprite_xform::auto_group_from_path();
            if (ImGui::Checkbox("Auto-group##xform_ag", &ag)) {
                mtr::sprite_xform::set_auto_group_from_path(ag);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply now##xform_ag_now")) {
                int n_changed = mtr::sprite_xform::auto_group_all_from_paths();
                mtr::log::info("auto-group: assigned %d slots from path", n_changed);
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            info_pill(
                "Auto-derives an element's group from its asset path's "
                "parent directory (e.g. ui/menu/main_menu/wilbur_smile → "
                "main_menu). Toggle = apply to new elements. Apply now = "
                "fill in empty groups for all existing elements. Manual "
                "groups are never overwritten.");

            if (s_pinned) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Recapture frame##xform_recap")) {
                    recapture_pin();
                }
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "(PINNED)");
            }

            // (Click-to-pick + gizmo mouse handling lives in the overlay
            // block at the bottom of draw_frame — one place that owns all
            // the spatial mouse state, so handles can't double-fire with
            // picks.)

            // Esc deselects, but only when there's an active selection — let
            // ImGui keep Esc for menu navigation otherwise.
            if (mtr::sprite_picking::selected() >= 0
                && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                mtr::sprite_picking::clear_selection();
            }

            ImGui::Spacing();

            // Snapshot: pinned (frozen array) or live (re-fetched every frame).
            SlotInfo live_slots[kMaxRows]{};
            int        n;
            SlotInfo*  slots;
            if (s_pinned) {
                n     = s_pinned_n;
                slots = s_pinned_slots;
            } else {
                n     = mtr::sprite_xform::snapshot_slots(live_slots, kMaxRows);
                slots = live_slots;
            }

            ImGui::Text("Top %d elements%s, organized by group:",
                        n, s_pinned ? " (frozen)" : " this frame");
            ImGui::Spacing();

            // Group rows by `group` field. Walk once to bucket. Empty
            // group → "Ungrouped" bucket. Stable ordering within each
            // bucket (by frame_count desc, since the source array is sorted).
            //
            // 2026-05-09: kMaxGroups was 16 — too small with Auto-group
            // ON, where each asset directory becomes its own group (easily
            // 20-50+ groups across the UI). Slots in the 17th-and-beyond
            // group were silently `continue`'d, making the list look
            // frozen. Bumped to 64; on overflow we still show a fallback
            // bucket "Other (overflow)" so slots are never invisible.
            constexpr int kMaxGroups   = 64;
            constexpr int kIdxsPerGrp  = kMaxRows;
            char group_names[kMaxGroups][32] = {{0}};
            int  group_idxs [kMaxGroups][kIdxsPerGrp];
            int  group_sizes[kMaxGroups]  = {0};
            int  num_groups   = 0;
            int  overflow_gi  = -1;   // lazily allocated last bucket
            for (int i = 0; i < n; ++i) {
                const char* gn = slots[i].group[0] ? slots[i].group : "Ungrouped";
                int gi = -1;
                for (int j = 0; j < num_groups; ++j) {
                    if (std::strcmp(group_names[j], gn) == 0) { gi = j; break; }
                }
                if (gi < 0) {
                    if (num_groups < kMaxGroups - 1) {
                        gi = num_groups++;
                        std::strncpy(group_names[gi], gn, 31);
                    } else {
                        // Last slot (index kMaxGroups-1) is reserved for
                        // overflow — every slot whose group can't fit in
                        // the table lands here so it remains visible.
                        if (overflow_gi < 0) {
                            overflow_gi = num_groups++;
                            std::strncpy(group_names[overflow_gi],
                                         "Other (overflow)", 31);
                        }
                        gi = overflow_gi;
                    }
                }
                if (group_sizes[gi] < kIdxsPerGrp) {
                    group_idxs[gi][group_sizes[gi]++] = i;
                }
            }

            // Render one row per slot. Display the variant components
            // (uv / screen / quad) when the slot has a non-wildcard pattern,
            // and a "Specialize" button when the slot is wildcard but has
            // a recent concrete key — that lets the user split off a
            // variant they want to control separately.
            // Stable ID hashes — slot_idx and group-array index both
            // shift across frames as slots are created/evicted. Hashing
            // the slot's COMPOSITE KEY (asset + variant) and the GROUP
            // NAME keeps ImGui's per-widget state (e.g. CollapsingHeader
            // open/closed, focus) anchored across those reshuffles.
            auto group_id_hash = [](const char* name) -> int {
                uint32_t h = 5381;
                for (const char* p = name; *p; ++p)
                    h = (h * 33u) ^ static_cast<uint8_t>(*p);
                return static_cast<int>(h | 1u);
            };
            auto slot_id_hash = [](const SlotInfo& info) -> int {
                uint32_t h = info.state_key;
                h ^= (static_cast<uint32_t>(info.uv_bucket)      << 16);
                h ^= (static_cast<uint32_t>(info.screen_context) <<  8);
                h ^=  static_cast<uint32_t>(info.bbox_quadrant);
                h *= 0x9E3779B1u;
                return static_cast<int>(h | 1u);
            };

            // Track which slot is selected, and remember the previous value
            // so we only auto-scroll on the frame the selection CHANGES.
            // Otherwise the list would yank the user's manual scrolling
            // every frame while a selection sits there.
            const int sel_slot = mtr::sprite_picking::selected();
            static int s_last_sel_slot = -1;
            const bool sel_changed = (sel_slot != s_last_sel_slot);
            s_last_sel_slot = sel_slot;

            auto render_one_slot = [&](int i, int row_index_in_group) {
                SlotInfo& info = slots[i];
                ImGui::PushID(slot_id_hash(info));
                bool changed  = false;
                bool save_now = false;
                const bool is_selected = (info.slot_idx == sel_slot);
                if (is_selected && sel_changed) {
                    ImGui::SetScrollHereY(0.5f);
                }

                // Two-channel split so we can paint a row background +
                // colored left-margin bar BEHIND the row content. Without
                // this, anything we draw after the content lands on top.
                ImGui::Spacing();
                const ImVec2 row_top = ImGui::GetCursorScreenPos();
                ImDrawList* dl       = ImGui::GetWindowDrawList();
                const bool stripe    = (row_index_in_group & 1) != 0;
                dl->ChannelsSplit(2);
                dl->ChannelsSetCurrent(1);  // foreground for content

                // Row 1: state_key + variant tag + name editor + group editor + counts.
                ImGui::Text("0x%08X", info.state_key);
                ImGui::SameLine();
                const bool is_wildcard =
                    info.uv_bucket      == 0xFFFF
                 && info.screen_context == 0xFF
                 && info.bbox_quadrant  == 0xFF;
                if (is_wildcard) {
                    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.65f, 1.0f), "[*]");
                } else {
                    char vt[48];
                    std::snprintf(vt, sizeof(vt), "[uv=%04X s=%02X q=%u]",
                        info.uv_bucket, info.screen_context, info.bbox_quadrant);
                    ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f), "%s", vt);
                }
                ImGui::SameLine();
                // Auto-named slots get a distinct text color so the user
                // can tell which names were auto-derived from asset paths
                // vs typed manually. Typing into the field clears the
                // auto_named flag (sprite_xform::set_name_at does that).
                const bool name_is_auto = info.auto_named && info.name[0] != 0;
                if (name_is_auto) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImVec4(0.55f, 0.85f, 0.55f, 1.0f));  // soft green
                }
                // Deferred-commit InputText pattern. Editing writes to a
                // per-slot draft buffer; nothing propagates back to the
                // sprite_xform store (and therefore nothing changes the
                // slot's group bucket / row sort key) until the user
                // releases focus. This keeps the row anchored where the
                // user clicked into it — without this, every keystroke
                // could change `info.group`, snapshot_slots would re-bucket
                // the slot under a different parent CollapsingHeader, the
                // InputText's parent ImGui-ID would change, focus dies.
                struct DraftEntry { int slot_idx; char name[48]; char group[32]; };
                static DraftEntry s_draft = { -1, {0}, {0} };

                const bool name_active_before =
                    (s_draft.slot_idx == info.slot_idx);
                char name_buf[48];
                std::strncpy(name_buf,
                    name_active_before ? s_draft.name : info.name, 47);
                name_buf[47] = 0;

                ImGui::SetNextItemWidth(160.0f);
                bool name_changed = ImGui::InputTextWithHint(
                    "##xname", "name (e.g. wilbur smile)",
                    name_buf, sizeof(name_buf));
                bool name_active = ImGui::IsItemActive();
                if (name_active) {
                    s_draft.slot_idx = info.slot_idx;
                    std::strncpy(s_draft.name, name_buf, 47);
                    s_draft.name[47] = 0;
                    // First-keystroke seed of group buffer — keeps it
                    // matching whatever was on disk if the user tabs.
                    if (!name_active_before) {
                        std::strncpy(s_draft.group, info.group, 31);
                        s_draft.group[31] = 0;
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    mtr::sprite_xform::set_name_at(info.slot_idx, name_buf);
                    save_now = true;
                    s_draft.slot_idx = -1;
                }
                if (name_is_auto) ImGui::PopStyleColor();

                ImGui::SameLine();
                char group_buf[32];
                std::strncpy(group_buf,
                    name_active_before ? s_draft.group : info.group, 31);
                group_buf[31] = 0;

                ImGui::SetNextItemWidth(110.0f);
                bool group_changed = ImGui::InputTextWithHint(
                    "##xgrp", "group (e.g. HUD)",
                    group_buf, sizeof(group_buf));
                bool group_active = ImGui::IsItemActive();
                if (group_active) {
                    s_draft.slot_idx = info.slot_idx;
                    std::strncpy(s_draft.group, group_buf, 31);
                    s_draft.group[31] = 0;
                    if (!name_active_before) {
                        std::strncpy(s_draft.name, info.name, 47);
                        s_draft.name[47] = 0;
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    mtr::sprite_xform::set_group_at(info.slot_idx, group_buf);
                    save_now = true;
                    s_draft.slot_idx = -1;
                }
                (void)name_changed; (void)group_changed; // edits commit on focus loss

                ImGui::SameLine();
                ImGui::TextDisabled(" %u/f cum %llu", info.frame_count,
                    static_cast<unsigned long long>(info.total_count));

                // Row 2: action buttons + transform sliders.
                if (ImGui::SmallButton("Hilite##xh")) { /* hold-only */ }
                if (ImGui::IsItemActive())
                    mtr::sprite_xform::set_highlight_at(info.slot_idx, true);
                else
                    mtr::sprite_xform::set_highlight_at(info.slot_idx, false);
                ImGui::SameLine();
                bool hidden = info.hidden;
                if (ImGui::Checkbox("Hide##xhd", &hidden)) {
                    info.hidden = hidden;
                    changed = save_now = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##xrs")) {
                    mtr::sprite_xform::reset_transform_at(info.slot_idx);
                    mtr::ui_aspect_rules::request_save();
                    dl->ChannelsMerge();  // close split before early-return
                    ImGui::PopID();
                    return;
                }

                // Specialize button — only meaningful on wildcard slots
                // that have actually matched a concrete entry recently.
                // Creates a new slot with the row's last-concrete components,
                // inheriting the wildcard's transform/name/group.
                if (is_wildcard && info.last_concrete_valid) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Specialize##xsp")) {
                        mtr::sprite_xform::specialize_slot(info.slot_idx);
                        mtr::ui_aspect_rules::request_save();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Pin to variant uv=%04X s=%02X q=%u",
                            info.last_uv_bucket, info.last_screen_context,
                            info.last_bbox_quadrant);
                    }
                } else if (!is_wildcard) {
                    ImGui::SameLine();
                    if (danger_button("Delete##xdel", ImVec2(0, 0))) {
                        mtr::sprite_xform::remove_slot(info.slot_idx);
                        mtr::ui_aspect_rules::request_save();
                        dl->ChannelsMerge();  // close split before early-return
                        ImGui::PopID();
                        return;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove this variant slot. The wildcard "
                                          "slot for the same state_key stays.");
                    }
                }

                constexpr ImGuiSliderFlags kDragFlags = ImGuiSliderFlags_AlwaysClamp;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(95.0f);
                if (ImGui::DragFloat("ox##xox", &info.offset_x, 0.001f, -5.0f, 5.0f, "%.4f", kDragFlags)) changed = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) save_now = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(95.0f);
                if (ImGui::DragFloat("oy##xoy", &info.offset_y, 0.001f, -5.0f, 5.0f, "%.4f", kDragFlags)) changed = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) save_now = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(75.0f);
                if (ImGui::DragFloat("sx##xsx", &info.scale_x, 0.002f, 0.05f, 5.0f, "%.4f", kDragFlags)) changed = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) save_now = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(75.0f);
                if (ImGui::DragFloat("sy##xsy", &info.scale_y, 0.002f, 0.05f, 5.0f, "%.4f", kDragFlags)) changed = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) save_now = true;

                if (changed) {
                    mtr::sprite_xform::set_transform_at(info.slot_idx,
                        info.offset_x, info.offset_y,
                        info.scale_x,  info.scale_y, info.hidden);
                }
                if (save_now) {
                    mtr::ui_aspect_rules::request_save();
                }
                ImGui::Spacing();

                // Paint row background + colored left-margin bar on
                // the BACKGROUND channel so it sits behind the content
                // we just drew. Bar color is hashed from state_key (same
                // hash the Hilite overlay uses), so when the user holds
                // Hilite, the on-screen rectangle and the row bar match.
                const ImVec2 row_bot = ImGui::GetCursorScreenPos();
                const ImVec2 win_pos = ImGui::GetWindowPos();
                const ImVec2 win_sz  = ImGui::GetWindowSize();
                const float  scrollbar_w = ImGui::GetStyle().ScrollbarSize;
                const float  x_left  = win_pos.x;
                const float  x_right = win_pos.x + win_sz.x - scrollbar_w;
                dl->ChannelsSetCurrent(0);  // background
                // Per-row tint hashed on the slot's full identity (state_key
                // + uv + screen + quad). Specialized variants of the same
                // texture get distinct tints — without this, three rows
                // pinned to GlowBox_Line would all look identical.
                const uint32_t h = static_cast<uint32_t>(slot_id_hash(info)) * 0x9E3779B1u;
                const uint8_t  cr = static_cast<uint8_t>((h >>  8) | 0x80);
                const uint8_t  cg = static_cast<uint8_t>((h >> 16) | 0x80);
                const uint8_t  cb = static_cast<uint8_t>((h >>  0) | 0x80);
                if (is_selected) {
                    // Selected row: strong cyan wash overrides the tint so
                    // the picked one is unmistakable.
                    dl->AddRectFilled(
                        ImVec2(x_left, row_top.y - 2.0f),
                        ImVec2(x_right, row_bot.y - 2.0f),
                        IM_COL32(80, 200, 230, 90));
                    dl->AddRectFilled(
                        ImVec2(x_left + 1.0f, row_top.y - 1.0f),
                        ImVec2(x_left + 6.0f, row_bot.y - 3.0f),
                        IM_COL32(80, 220, 250, 255));
                } else {
                    // Full-row wash at 40 alpha (visible but not glaring),
                    // plus optional zebra stripe lifted to 22 alpha so it
                    // separates same-state_key adjacent rows too.
                    dl->AddRectFilled(
                        ImVec2(x_left, row_top.y - 2.0f),
                        ImVec2(x_right, row_bot.y - 2.0f),
                        IM_COL32(cr, cg, cb, 40));
                    if (stripe) {
                        dl->AddRectFilled(
                            ImVec2(x_left, row_top.y - 2.0f),
                            ImVec2(x_right, row_bot.y - 2.0f),
                            IM_COL32(255, 255, 255, 22));
                    }
                    // Solid 6-px left bar in fully saturated tint as
                    // secondary marker (matches the Hilite overlay color).
                    dl->AddRectFilled(
                        ImVec2(x_left + 1.0f, row_top.y - 1.0f),
                        ImVec2(x_left + 6.0f, row_bot.y - 3.0f),
                        IM_COL32(cr, cg, cb, 240));
                }
                // Solid horizontal rule between rows so adjacent rows
                // don't visually blur. The previous design relied on
                // ImGui::Spacing alone, which left no clear divider.
                dl->AddLine(
                    ImVec2(x_left, row_bot.y - 1.0f),
                    ImVec2(x_right, row_bot.y - 1.0f),
                    IM_COL32(255, 255, 255, 28), 1.0f);
                dl->ChannelsMerge();
                ImGui::PopID();
            };

            // Named groups first (alphabetical), then "Ungrouped".
            int ungrouped_gi = -1;
            int order[kMaxGroups];
            int order_n = 0;
            for (int g = 0; g < num_groups; ++g) {
                if (std::strcmp(group_names[g], "Ungrouped") == 0) {
                    ungrouped_gi = g;
                } else {
                    order[order_n++] = g;
                }
            }
            // Simple alpha sort.
            for (int i = 1; i < order_n; ++i) {
                int j = i;
                while (j > 0 && std::strcmp(group_names[order[j]],
                                            group_names[order[j-1]]) < 0) {
                    int t = order[j]; order[j] = order[j-1]; order[j-1] = t;
                    --j;
                }
            }
            if (ungrouped_gi >= 0) order[order_n++] = ungrouped_gi;

            for (int oi = 0; oi < order_n; ++oi) {
                const int g = order[oi];
                ImGui::PushID(group_id_hash(group_names[g]));

                // Per-group color derived from group-name hash. Used for:
                //   (a) tinting the CollapsingHeader frame so the user can
                //       see at a glance which group is which,
                //   (b) drawing a tall vertical rail down the left side of
                //       the group's body so members visually "belong" to
                //       this group.
                // "Ungrouped" gets a neutral grey so it doesn't compete
                // visually with named groups.
                const bool is_ungrouped =
                    (std::strcmp(group_names[g], "Ungrouped") == 0);
                uint8_t grp_r = 110, grp_g = 110, grp_b = 110;
                if (!is_ungrouped) {
                    const uint32_t gh =
                        static_cast<uint32_t>(group_id_hash(group_names[g])) * 0x9E3779B1u;
                    grp_r = static_cast<uint8_t>((gh >>  8) | 0x80);
                    grp_g = static_cast<uint8_t>((gh >> 16) | 0x80);
                    grp_b = static_cast<uint8_t>((gh >>  0) | 0x80);
                }

                ImGui::PushStyleColor(ImGuiCol_Header,
                    IM_COL32(grp_r, grp_g, grp_b, 90));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                    IM_COL32(grp_r, grp_g, grp_b, 150));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                    IM_COL32(grp_r, grp_g, grp_b, 210));

                char hdr[80];
                std::snprintf(hdr, sizeof(hdr), "%s  (%d slot%s)##xfgrp",
                    group_names[g], group_sizes[g],
                    group_sizes[g] == 1 ? "" : "s");
                // Default-open every group, including "Ungrouped". Ungrouped
                // is where the user does the labeling work; auto-collapsing
                // on every Specialize click was hostile.
                ImGuiTreeNodeFlags flg = ImGuiTreeNodeFlags_Framed
                                       | ImGuiTreeNodeFlags_DefaultOpen;
                bool group_open = ImGui::CollapsingHeader(hdr, flg);
                ImGui::PopStyleColor(3);
                if (group_open) {
                    // Capture the body's top so we can draw a vertical rail
                    // down its full height after the children render.
                    const ImVec2 body_top = ImGui::GetCursorScreenPos();
                    ImDrawList* gdl = ImGui::GetWindowDrawList();
                    // Defer the rail draw until we know body_bot.
                    // CollapsingHeader's body — we'll PopID at the matching
                    // bottom. row_index_in_group is incremented per slot so
                    // alternating rows get the zebra-stripe tint.
                    ImGui::Indent(8.0f);

                    // ---- Per-group bulk operations -----------------------
                    // Compact action row above the per-slot list. Operations
                    // iterate over the group's slots and apply the same
                    // change to each. Use case: hide all HUD sprites with
                    // one click while editing a different element.
                    ImGui::TextDisabled("Bulk:");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Hide all##bulk_hide")) {
                        for (int k = 0; k < group_sizes[g]; ++k) {
                            const int row = group_idxs[g][k];
                            const auto& info = slots[row];
                            mtr::sprite_xform::set_transform_at(info.slot_idx,
                                info.offset_x, info.offset_y,
                                info.scale_x,  info.scale_y, true);
                        }
                        mtr::ui_aspect_rules::request_save();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Show all##bulk_show")) {
                        for (int k = 0; k < group_sizes[g]; ++k) {
                            const int row = group_idxs[g][k];
                            const auto& info = slots[row];
                            mtr::sprite_xform::set_transform_at(info.slot_idx,
                                info.offset_x, info.offset_y,
                                info.scale_x,  info.scale_y, false);
                        }
                        mtr::ui_aspect_rules::request_save();
                    }
                    ImGui::SameLine();
                    if (warning_button("Reset all##bulk_reset")) {
                        for (int k = 0; k < group_sizes[g]; ++k) {
                            const int row = group_idxs[g][k];
                            mtr::sprite_xform::reset_transform_at(slots[row].slot_idx);
                        }
                        mtr::ui_aspect_rules::request_save();
                    }
                    ImGui::SameLine();
                    info_pill(
                        "Bulk operations apply to every slot in this group. "
                        "Use 'Hide all' to hide a category of UI temporarily, "
                        "'Reset all' to clear transforms group-wide.");

                    // LIVE bulk-offset drag — drag-with-snapshot pattern.
                    // While the user holds the dx/dy slider:
                    //   - on first activation, snapshot every member slot's
                    //     current offset_x/offset_y into a frozen origins[]
                    //   - on every drag frame, each slot's offset becomes
                    //     origin + slider_value (relative, no accumulation)
                    //   - on release, save_dirty is flagged once
                    // Removes the "Apply" button — drag is live, like every
                    // other slider in the panel. Storage is per-group via
                    // ImGui's per-PushID slot.
                    constexpr int kMaxBulkOrigins = 64;
                    struct BulkDragOrigin { int slot_idx; float ox; float oy; };
                    static BulkDragOrigin s_bulk_origins[kMaxBulkOrigins];
                    static int     s_bulk_origin_count    = 0;
                    static int     s_bulk_origin_group_id = 0;  // group_id_hash
                    const int      this_group_id_hash     = group_id_hash(group_names[g]);

                    ImGuiStorage* state = ImGui::GetStateStorage();
                    const ImGuiID dx_id = ImGui::GetID("bulk_dx_v");
                    const ImGuiID dy_id = ImGui::GetID("bulk_dy_v");
                    float bulk_dx = state->GetFloat(dx_id, 0.0f);
                    float bulk_dy = state->GetFloat(dy_id, 0.0f);

                    auto snapshot_origins = [&]() {
                        s_bulk_origin_count    = 0;
                        s_bulk_origin_group_id = this_group_id_hash;
                        for (int k = 0; k < group_sizes[g]
                             && s_bulk_origin_count < kMaxBulkOrigins; ++k) {
                            const int row = group_idxs[g][k];
                            const auto& info = slots[row];
                            s_bulk_origins[s_bulk_origin_count++] = {
                                info.slot_idx, info.offset_x, info.offset_y };
                        }
                    };
                    auto apply_drag = [&]() {
                        if (s_bulk_origin_group_id != this_group_id_hash) return;
                        for (int k = 0; k < s_bulk_origin_count; ++k) {
                            const auto& o = s_bulk_origins[k];
                            mtr::sprite_xform::set_transform_at(o.slot_idx,
                                o.ox + bulk_dx, o.oy + bulk_dy,
                                /*sx*/ 1.0f, /*sy*/ 1.0f, /*hidden=preserved-by-not-touching*/ false);
                            // We have to look up the slot's current sx/sy/hidden
                            // because set_transform_at writes ALL fields. Find
                            // the slot in the local snapshot.
                            for (int kk = 0; kk < group_sizes[g]; ++kk) {
                                const int row = group_idxs[g][kk];
                                auto& info = slots[row];
                                if (info.slot_idx == o.slot_idx) {
                                    mtr::sprite_xform::set_transform_at(o.slot_idx,
                                        o.ox + bulk_dx, o.oy + bulk_dy,
                                        info.scale_x, info.scale_y, info.hidden);
                                    info.offset_x = o.ox + bulk_dx;
                                    info.offset_y = o.oy + bulk_dy;
                                    break;
                                }
                            }
                        }
                    };

                    ImGui::TextDisabled("Move group (live):");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(85.0f);
                    bool dx_changed = ImGui::DragFloat("dx##bulk_dx",
                        &bulk_dx, 0.001f, -5.0f, 5.0f, "%.4f",
                        ImGuiSliderFlags_AlwaysClamp);
                    if (ImGui::IsItemActivated()) snapshot_origins();
                    if (dx_changed) {
                        state->SetFloat(dx_id, bulk_dx);
                        apply_drag();
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        // Bake the drag delta into each slot's origin and
                        // zero the slider — next drag starts from the new
                        // position. Save once here.
                        state->SetFloat(dx_id, 0.0f);
                        state->SetFloat(dy_id, 0.0f);
                        s_bulk_origin_group_id = 0;  // invalidate
                        mtr::ui_aspect_rules::request_save();
                    }

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(85.0f);
                    bool dy_changed = ImGui::DragFloat("dy##bulk_dy",
                        &bulk_dy, 0.001f, -5.0f, 5.0f, "%.4f",
                        ImGuiSliderFlags_AlwaysClamp);
                    if (ImGui::IsItemActivated()) snapshot_origins();
                    if (dy_changed) {
                        state->SetFloat(dy_id, bulk_dy);
                        apply_drag();
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        state->SetFloat(dx_id, 0.0f);
                        state->SetFloat(dy_id, 0.0f);
                        s_bulk_origin_group_id = 0;
                        mtr::ui_aspect_rules::request_save();
                    }

                    ImGui::Separator();
                    for (int k = 0; k < group_sizes[g]; ++k) {
                        render_one_slot(group_idxs[g][k], k);
                    }
                    ImGui::Unindent(8.0f);

                    // Vertical rail down the left side of the group's body —
                    // visually wraps the members so they "belong" to this
                    // group. Color matches the header tint.
                    const ImVec2 body_bot = ImGui::GetCursorScreenPos();
                    const ImVec2 win_pos2  = ImGui::GetWindowPos();
                    const float  rail_x    = win_pos2.x + 4.0f;
                    gdl->AddRectFilled(
                        ImVec2(rail_x,        body_top.y),
                        ImVec2(rail_x + 4.0f, body_bot.y - 2.0f),
                        IM_COL32(grp_r, grp_g, grp_b, 220));
                }
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::TextUnformatted("Tip: ");
            ImGui::SameLine();
            info_pill(
                "Hold Hilite to force-show matching elements (alpha=255). "
                "Toggle Hide to make them invisible - useful for figuring "
                "out which row corresponds to which on-screen element.\n\n"
                "If one row moves several unrelated elements (because they "
                "share an atlas), click Specialize while the offending "
                "element is on screen - that creates a new row pinned to "
                "the exact variant currently rendering.\n\n"
                "Capacity is 64 elements at a time. Edited rows are "
                "pinned; unedited wildcards get evicted when full. "
                "Cross-session matches are best-effort because asset "
                "identifiers can shift between game runs.");
            ImGui::TreePop();
        }
        ImGui::Spacing();

        // ---- Live status panel: top screen + matched rule + factor ---------
        char top[64] = {0};
        const bool have_top = mtr::screen_push::current_top_name(top, sizeof(top));
        const int  depth    = mtr::screen_push::stack_depth();
        const float screen_aspect = mtr::aspect::current();

        ImGui::Text("Current screen (stack depth %d):", depth);
        ImGui::SameLine();
        if (have_top) {
            ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "%s", top);
        } else {
            ImGui::TextDisabled("(none)");
        }

        char matched_pat[48] = {0};
        float matched_aspect = 0.0f;
        const bool matched = mtr::ui_aspect_rules::resolve_match(
            top, matched_pat, sizeof(matched_pat), &matched_aspect);
        if (matched) {
            const float factor = (screen_aspect > 0.0f && matched_aspect > 0.0f)
                                  ? matched_aspect / screen_aspect : 1.0f;
            ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f),
                "Rule active: \"%s\"  target=%.4f  screen=%.4f  factor=%.4f",
                matched_pat, matched_aspect, screen_aspect, factor);
        } else if (have_top) {
            ImGui::TextDisabled("No rule matches this screen - showing as-is.");
        } else {
            ImGui::TextDisabled("No active screen.");
        }

        if (depth > 0 && ImGui::TreeNode("Screen stack (top is last)")) {
            for (int i = 0; i < depth; ++i) {
                char buf[64] = {0};
                if (mtr::screen_push::stack_at(i, buf, sizeof(buf))) {
                    ImGui::Text("  [%d] %s", i, buf);
                }
            }
            ImGui::TreePop();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Auto-sync the rules table with the engine's screen registry on
        // every menu draw. Idempotent — only adds names that aren't
        // already a rule pattern. The checkbox below lets a power user
        // disable it (curated tiny ruleset). Default ON so screens like
        // WilburNewLoadSave appear in the list the moment the user
        // navigates there, no extra clicks.
        if (mtr::ui_aspect_rules::auto_sync_screens()) {
            int added = mtr::ui_aspect_rules::sync_from_registry();
            if (added > 0) {
                mtr::ui_aspect_rules::request_save();
            }
        }

        bool auto_sync = mtr::ui_aspect_rules::auto_sync_screens();
        if (ImGui::Checkbox("Auto-track engine screens##rules_autosync", &auto_sync)) {
            mtr::ui_aspect_rules::set_auto_sync_screens(auto_sync);
            mtr::ui_aspect_rules::request_save();
        }
        info_pill("Auto-add every screen the engine registers (at boot) "
            "and every screen pushed at runtime. Off = curated list "
            "(remove rules and they stay removed).");
        ImGui::Spacing();

        const size_t n   = mtr::ui_aspect_rules::rule_count();
        const size_t cap = mtr::ui_aspect_rules::max_rules();
        ImGui::Text("Rules (%zu / %zu):", n, cap);
        ImGui::Spacing();

        // Wrap the per-rule list in a fixed-height scrollable child so a
        // bulk-add ("All N known screens") of 50+ rules doesn't blow the
        // menu off the bottom of the screen and hide the action buttons
        // ("+ Add rule", "Restore defaults", etc.) below. Height ~280px =
        // ~10 rule rows visible at once; scrolls within itself when full.
        ImGui::BeginChild("##rules_scroll",
                          ImVec2(0.0f, 280.0f),
                          true /*border*/,
                          ImGuiWindowFlags_HorizontalScrollbar);

        for (size_t i = 0; i < n; ++i) {
            char pat[48] = {0};
            float a = 0.0f;
            mtr::ui_aspect_rules::get_rule(i, pat, sizeof(pat), &a);
            // Highlight the row that's currently winning the match.
            const bool is_match = matched && std::strcmp(pat, matched_pat) == 0;
            ImGui::PushID(static_cast<int>(i));
            if (is_match) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.18f, 0.40f, 0.18f, 1.0f));
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("##pat", pat, sizeof(pat))) {
                mtr::ui_aspect_rules::set_rule(i, pat, a);
            }
            // Persist text edits when the user moves focus away (single write
            // per edit, not per-keystroke).
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::SliderFloat("##aspect", &a, 0.0f, 5.5f, "%.4f")) {
                mtr::ui_aspect_rules::set_rule(i, pat, a);
            }
            // Persist slider edits when the user releases — avoids 60Hz disk IO.
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            if (ImGui::Button("4:3"))  {
                a = 4.0f/3.0f;
                mtr::ui_aspect_rules::set_rule(i, pat, a);
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            if (ImGui::Button("16:9")) {
                a = 16.0f/9.0f;
                mtr::ui_aspect_rules::set_rule(i, pat, a);
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            if (ImGui::Button("Off"))  {
                a = 0.0f;
                mtr::ui_aspect_rules::set_rule(i, pat, a);
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            if (ImGui::Button("X##rm")) {
                mtr::ui_aspect_rules::remove_rule(i);
                mtr::ui_aspect_rules::request_save();
                if (is_match) ImGui::PopStyleColor();
                ImGui::PopID();
                break; // mutated, restart next frame
            }
            if (is_match) ImGui::PopStyleColor();
            ImGui::PopID();
        }

        ImGui::EndChild();
        ImGui::Spacing();
        if (n < cap) {
            if (ImGui::Button("+ Add rule")) {
                mtr::ui_aspect_rules::add_rule("", 0.0f);
                mtr::ui_aspect_rules::request_save();
            }
            ImGui::SameLine();
            // "+ Add for current" — uses the live top-screen name as the
            // pattern, defaulted to 4:3 (the typical pillarbox target). Only
            // active when we actually have a top screen to copy.
            if (have_top) {
                if (ImGui::Button("+ Add for current screen")) {
                    mtr::ui_aspect_rules::add_rule(top, 4.0f / 3.0f);
                    mtr::ui_aspect_rules::request_save();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Button("+ Add for current screen");
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled("Max %zu rules reached.", cap);
            ImGui::SameLine();
        }
        // "+ Add ALL known screens" — bulk-populate from the engine's screen
        // registry (every name registered at boot via sub_60E9F0 into the
        // 0x744A80 registry; ~57 names in retail). Skips names that already
        // have an exact-match rule, so re-clicking doesn't dupe. Default
        // aspect 4:3 to match the rest of the UI's pillarbox convention.
        const int reg_n = mtr::screen_push::registered_count();
        if (reg_n > 0) {
            char btn_label[64];
            std::snprintf(btn_label, sizeof(btn_label),
                          "+ Add ALL %d known screens##rules_addall", reg_n);
            if (ImGui::Button(btn_label)) {
                int added = 0;
                for (int i = 0; i < reg_n; ++i) {
                    char rname[64] = {0};
                    if (!mtr::screen_push::registered_at(i, rname, sizeof(rname))) continue;
                    if (rname[0] == 0) continue;
                    // Skip if a rule with this exact pattern already exists.
                    bool dup = false;
                    const size_t rules_now = mtr::ui_aspect_rules::rule_count();
                    for (size_t r = 0; r < rules_now; ++r) {
                        char existing[64] = {0};
                        float a = 0.0f;
                        mtr::ui_aspect_rules::get_rule(r, existing, sizeof(existing), &a);
                        if (std::strcmp(existing, rname) == 0) { dup = true; break; }
                    }
                    if (dup) continue;
                    if (mtr::ui_aspect_rules::rule_count() >= mtr::ui_aspect_rules::max_rules()) break;
                    mtr::ui_aspect_rules::add_rule(rname, 4.0f / 3.0f);
                    ++added;
                }
                if (added > 0) mtr::ui_aspect_rules::request_save();
                mtr::log::info("ui_aspect_rules: bulk-add added %d rules from"
                               " engine screen registry (%d candidates)",
                               added, reg_n);
            }
            ImGui::SameLine();
            info_pill("Adds one rule per screen registered at engine boot. "
                "Skips screens already in the list. Set the cap "
                "kMaxRules higher in ui_aspect_rules.cpp if you hit it.");
            ImGui::SameLine();
        }
        if (ImGui::Button("Restore defaults##rules_restore")) {
            mtr::ui_aspect_rules::clear_all();
            mtr::ui_aspect_rules::install_defaults();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear all##rules_clear")) {
            mtr::ui_aspect_rules::clear_all();
            mtr::ui_aspect_rules::request_save();
        }
        section_end(open);
    }

    if (bool open = section_begin("FPS limiter")) {
        heading_with_info("Cap frame rate to a target",
            "Caps the game's frame rate. Useful to match your monitor's "
            "refresh rate or to limit fan noise / GPU power. Game speed "
            "should remain constant at any cap (the game uses real-time "
            "clocks).");
        ImGui::Spacing();

        bool en = mtr::fps_limit::enabled();
        if (ImGui::Checkbox("Limit FPS", &en)) {
            // Toggle keeps last-set target, or defaults to monitor refresh
            // rate the first time the user enables it.
            if (en) {
                int t = mtr::fps_limit::current();
                if (t <= 0) t = mtr::fps_limit::monitor_refresh_hz();
                mtr::fps_limit::set(t);
            } else {
                mtr::fps_limit::set(0);
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(en ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           en ? "  Active" : "  Off");

        ImGui::Spacing();
        int target = mtr::fps_limit::current();
        if (target <= 0) target = mtr::fps_limit::monitor_refresh_hz();
        constexpr int presets[]   = { 30, 60, 120, 144, 240 };
        const char* preset_lbl[]  = { "30 FPS", "60 FPS", "120 FPS",
                                      "144 FPS", "240 FPS", "Custom" };
        int combo_idx = 5;
        for (int i = 0; i < 5; ++i) if (target == presets[i]) { combo_idx = i; break; }
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("##fps_preset", &combo_idx, preset_lbl, 6)) {
            if (combo_idx < 5) {
                mtr::fps_limit::set(presets[combo_idx]);
                target = presets[combo_idx];
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Match monitor##fps")) {
            mtr::fps_limit::set(mtr::fps_limit::monitor_refresh_hz());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(monitor: %d Hz)", mtr::fps_limit::monitor_refresh_hz());

        if (combo_idx == 5) {
            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::SliderInt("Custom##fps_custom", &target, 15, 500, "%d FPS",
                                 ImGuiSliderFlags_Logarithmic)) {
                mtr::fps_limit::set(target);
            }
        }
        section_end(open);
    }

    if (bool open = section_begin("FPS overlay")) {
        bool en = fps_overlay_enabled();
        if (ImGui::Checkbox("Show FPS overlay", &en)) {
            set_fps_overlay_enabled(en);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(visible while game is running, even when this menu is closed)");

        int corner = fps_overlay_corner();
        const char* items[] = { "Top-left", "Top-right", "Bottom-left", "Bottom-right" };
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Corner##fps", &corner, items, IM_ARRAYSIZE(items))) {
            set_fps_overlay_corner(corner);
        }
        section_end(open);
    }
}

} // namespace mtr::menu::detail
