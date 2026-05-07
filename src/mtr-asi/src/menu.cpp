#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "mtr/version.h"
#include "mtr/sim_decouple.h"
#include "mtr/interp.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace mtr::log        { void info(const char* fmt, ...); }
namespace mtr::d3d9hook   { int last_pp_width(); int last_pp_height(); }
namespace mtr::screenshot {
    void        request();
    void        try_capture(IDirect3DDevice9* dev);
    const char* last_path();
    std::uint64_t last_path_tag();
}
namespace mtr::aspect {
    bool  available();
    float current();
    float original();
    bool  set(float value);
}
namespace mtr::fov {
    bool  has_override();
    float current();
    bool  set(float value);
}
namespace mtr::draw_dist {
    bool  has_override();
    float current();
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
namespace mtr::lod {
    float lod_scale();              void set_lod_scale(float v);
    float periphery_reject_angle(); void set_periphery_reject_angle(float v_rad);
    float periphery_reject_dist();  void set_periphery_reject_dist(float v);
    float periphery_accept_dist();  void set_periphery_accept_dist(float v);
    float focus_dist();             void set_focus_dist(float v);
    float high_dist();              void set_high_dist(float v);
    float medium_dist();            void set_medium_dist(float v);
    void disable_periphery_cull();
    void restore_periphery_cull_defaults();
    float periphery_reject_angle_deg();
    void  set_periphery_reject_angle_deg(float deg);
    float periphery_reject_dist_decoded();
    void  set_periphery_reject_dist_squared(float linear_dist);
}
namespace mtr::cvar_dump {
    size_t count();
    bool   dump_default();
}
namespace mtr::fps_limit {
    bool enabled();
    int  current();
    void set(int fps);
    int  monitor_refresh_hz();
}
// mtr::sim_decouple API comes from "mtr/sim_decouple.h" included above.
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

    // v2 (composite-identity) APIs — operate by stable slot index instead
    // of raw state_key, so multiple variants of the same state_key can be
    // controlled independently.
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
    // Group drag (broadcast a single slot's delta to its group peers).
    bool      drag_group();
    void      set_drag_group(bool v);
    bool      get_group_at_buf(int slot_idx, char* out, size_t out_size);
    void      apply_group_translate_delta(const char* group, int exclude_slot_idx,
                                          float delta_ox, float delta_oy);
    void      apply_group_scale_factor(const char* group, int exclude_slot_idx,
                                       float factor_sx, float factor_sy);
    // Auto-grouping by asset path.
    bool      auto_group_from_path();
    void      set_auto_group_from_path(bool v);
    int       auto_group_all_from_paths();
    // Phase A: per-frame distribution tally + one-shot full-list CSV dump.
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
}
namespace mtr::input_hook {
    int drain_wheel_delta();
    struct MouseQueueEvent {
        enum Kind : uint8_t { Pos = 0, Button = 1, Wheel = 2 };
        uint8_t kind;
        uint8_t button;
        bool    down;
        int16_t wheel_delta;
        int32_t screen_x;
        int32_t screen_y;
    };
    int drain_mouse_events(MouseQueueEvent* out, int max_out);
}
namespace mtr::dinput_hook {
    // Notify the dinput hook that mtr-asi UI is visible / hidden. While
    // visible we downgrade the mouse device's cooperative level from
    // DISCL_EXCLUSIVE to DISCL_NONEXCLUSIVE so the OS cursor unpins;
    // hidden restores the original level for gameplay mouse-look.
    void set_ui_visible(bool visible);
    // Called every frame while UI is visible — re-asserts the
    // cursor-freedom state if anything fought it back.
    void tick_force_unpin();
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
}
namespace mtr::console {
    void poll_hotkey();
    void draw();
    bool is_visible();
    void set_visible(bool v);
}
namespace mtr::freecam {
    bool  active();
    void  set_active(bool a);
    void  tick();
    void  get_pose(float pos[3], float* yaw, float* pitch);
    float move_speed();
    float look_speed();
    float mouse_sens();
    void  set_move_speed(float v);
    void  set_look_speed(float v);
    void  set_mouse_sens(float v);
    void  set_ui_visible(bool v);
    void  request_teleport_to_camera();
    void  accumulate_wheel(int delta);
}
namespace mtr::menu {

namespace {

std::atomic<bool> g_visible{false};
std::atomic<bool> g_imgui_ready{false};
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<IDirect3DDevice9*> g_device{nullptr};
ImGuiContext* g_ctx = nullptr;
WNDPROC g_orig_wndproc = nullptr;

// FPS overlay: tiny corner-pinned window that displays FPS + frame time
// independent of the main Insert menu. Toggle from Tools tab.
// Default ON — the overlay is non-interactive and the cost is a single
// always-visible widget; useful for any user verifying performance.
std::atomic<bool> g_fps_overlay{true};
std::atomic<int>  g_fps_overlay_corner{0};  // 0=TL, 1=TR, 2=BL, 3=BR

LRESULT CALLBACK subclass_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imgui_ready.load()) {
        ImGui::SetCurrentContext(g_ctx);
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
            // ImGui consumed; swallow if any mtr-asi window is up so game doesn't
            // see player typing console commands or clicking the menu.
            if (g_visible.load() || mtr::console::is_visible()) {
                switch (msg) {
                case WM_KEYDOWN: case WM_KEYUP:
                case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                case WM_CHAR:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_MOUSEMOVE:
                case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                    return 0;
                }
            }
        }
    }
    return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

void init_imgui(IDirect3DDevice9* dev) {
    if (g_imgui_ready.load()) return;

    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(dev->GetCreationParameters(&cp))) {
        mtr::log::info("menu: GetCreationParameters failed");
        return;
    }
    HWND hwnd = cp.hFocusWindow;
    if (!hwnd) {
        mtr::log::info("menu: hFocusWindow is null");
        return;
    }
    g_hwnd  = hwnd;
    g_device = dev;

    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        mtr::log::info("menu: ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext(g_ctx); g_ctx = nullptr;
        return;
    }
    if (!ImGui_ImplDX9_Init(dev)) {
        mtr::log::info("menu: ImGui_ImplDX9_Init failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_ctx); g_ctx = nullptr;
        return;
    }

    g_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(&subclass_wndproc)));

    g_imgui_ready = true;
    mtr::log::info("menu: ImGui ready (hwnd=%p dev=%p) -- press Insert to toggle", hwnd, dev);
}

// === Visual section helpers =================================================
// Each tab uses these for consistent styling: a coloured collapsing-header
// bar at the top of each section, indented body, a trailing separator gap.

bool section_begin(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.18f, 0.32f, 0.50f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.38f, 0.58f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.26f, 0.45f, 0.66f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    const bool open = ImGui::CollapsingHeader(title);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (open) {
        ImGui::Indent(8.0f);
        ImGui::Spacing();
    }
    return open;
}

void section_end(bool was_open) {
    if (was_open) {
        ImGui::Spacing();
        ImGui::Unindent(8.0f);
    }
}

// === Menu style convention ==================================================
//
// RULE: never paste paragraph-length technical text directly into the
// section body. Walls of TextWrapped clutter the menu and bury the
// controls users care about. ALWAYS use one of these helpers instead:
//
//   info_pill(text)
//       — emits a "(?)" hint that shows `text` on hover. Use it next to
//         a control or after a SameLine() to attach background context.
//
//   heading_with_info(title, details)
//       — emits a one-line section sub-heading + the (?) pill.
//         Call this AT MOST ONCE per section, immediately after
//         section_begin(...), to give the section a brief framing line
//         with the technical details tucked away.
//
//   primary_button / warning_button / danger_button
//       — colour-coded buttons. Reserve for the *primary* user-facing
//         action of a section; default ImGui blue stays for everything
//         else. Don't use these for engineering-diagnostic buttons.
//
// Anti-patterns to avoid:
//   - ImGui::TextWrapped("multi-paragraph engineering note...")    NO
//   - ImGui::TextDisabled("multi-line tip about how to use...")    NO
//   - Multiple full-width TextWrapped lines stacked at the top      NO
//
// If a section needs a >1-line label that must remain visible, write a
// tight title (heading_with_info) and put EVERYTHING ELSE in the (?)
// tooltip. Tooltips can be paragraphs; the visible UI cannot.
//
// Compact "(?)" hint pill — hover to see the technical text.
void info_pill(const char* tooltip_text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(tooltip_text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Section sub-heading helper: short title + (?) pill on the same line.
// `details` is the technical / engineering note shown on hover.
void heading_with_info(const char* title, const char* details) {
    ImGui::TextUnformatted(title);
    ImGui::SameLine();
    info_pill(details);
}

// Color-coded action buttons. Use sparingly — only on the *primary*
// fast-effect buttons users want to find at a glance. Default ImGui blue
// stays for everything else.
//
//   primary  — the recommended one-click action for this section (green).
//   warning  — destructive but reversible (yellow / amber).
//   danger   — destructive and not easily reversible (red).
bool primary_button(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.75f, 0.30f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}
bool warning_button(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.45f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.55f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.65f, 0.18f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}
bool danger_button(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.26f, 0.26f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

// === FPS overlay ============================================================
// Reads io.Framerate (ImGui's smoothed 1s EMA -- already updated every frame
// in NewFrame, so always available). Pins a no-decoration window to one of
// the four screen corners with a 8-px margin from the work-area edge.
//
// Decouple-aware: when the sim_decouple module is in THROTTLE mode (or the
// detailed-log toggle is on), the overlay expands to show structured RENDER /
// SIM / ALPHA lines + per-system status flags. Otherwise it stays minimal
// (one-line legacy display) so users who don't care about decouple don't
// see extra clutter.
void draw_fps_overlay() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const int corner = g_fps_overlay_corner.load();
    const float pad = 8.0f;
    ImVec2 pos, pivot;
    pos.x   = (corner & 1) ? (vp->WorkPos.x + vp->WorkSize.x - pad) : (vp->WorkPos.x + pad);
    pos.y   = (corner & 2) ? (vp->WorkPos.y + vp->WorkSize.y - pad) : (vp->WorkPos.y + pad);
    pivot.x = (corner & 1) ? 1.0f : 0.0f;
    pivot.y = (corner & 2) ? 1.0f : 0.0f;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.55f);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration   | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings| ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav          | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##mtr_fps_overlay", nullptr, flags)) {
        const float fps = ImGui::GetIO().Framerate;
        const float ms  = (fps > 0.0f) ? (1000.0f / fps) : 0.0f;
        const bool decouple_active = mtr::sim_decouple::is_throttling()
                                   || mtr::sim_decouple::detailed_log_enabled();

        if (!decouple_active) {
            ImGui::Text("%6.1f FPS  %5.2f ms", fps, ms);
        } else {
            const double r_hz  = mtr::sim_decouple::measured_render_hz();
            const double s_hz  = mtr::sim_decouple::measured_sim_hz();
            const double alpha = mtr::sim_decouple::measured_alpha();
            const int    tgt   = mtr::sim_decouple::target_hz();
            const float  r_ms  = (r_hz > 0.001) ? static_cast<float>(1000.0 / r_hz) : 0.0f;
            const float  s_ms  = (s_hz > 0.001) ? static_cast<float>(1000.0 / s_hz) : 0.0f;

            // RENDER line: independent EMA from on_render_frame, falls back
            // to ImGui's smoothed framerate when the EMA hasn't filled yet.
            const double r_show = (r_hz > 0.001) ? r_hz : static_cast<double>(fps);
            const float  rms_show = (r_show > 0.001) ? static_cast<float>(1000.0 / r_show) : ms;
            (void)r_ms; (void)s_ms;
            ImGui::Text("RENDER:  %6.1f Hz  %5.2f ms", r_show, rms_show);

            // SIM line: shows "—" until M1.2 wires the sim hook (s_hz reads 0).
            if (s_hz > 0.001) {
                ImGui::Text("SIM:     %6.1f Hz  %5.2f ms  (target %d)",
                            s_hz, static_cast<float>(1000.0 / s_hz), tgt);
            } else {
                ImGui::Text("SIM:        --- Hz   ---  ms  (target %d)", tgt);
            }

            // ALPHA: now wired to interp module (M2). Reads (now - curr.qpc) /
            // sim_step, clamped to [0, 1].
            const float interp_alpha = mtr::interp::current_alpha();
            const bool  cut          = mtr::interp::is_cut_detected();
            (void)alpha;
            ImGui::Text("ALPHA:   %5.3f%s", interp_alpha, cut ? "  [CUT]" : "");

            // Status line: which decouple modes are active. CAM_INTERP /
            // PLAYER_INTERP / NPC_INTERP placeholders read `off` until M3-M5.
            auto draw_flag = [](const char* label, bool on) {
                ImGui::SameLine();
                ImGui::TextColored(on ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f)
                                      : ImVec4(0.55f, 0.55f, 0.6f, 1.0f),
                                   on ? "%s:ON" : "%s:off", label);
            };
            ImGui::Text("DECOUPLE:");
            draw_flag("THR",  mtr::sim_decouple::is_throttling());
            draw_flag("CAM",  mtr::interp::view_interp_enabled());
            draw_flag("PLR",  mtr::interp::player_interp_enabled());
            draw_flag("NPC",  mtr::interp::npc_interp_enabled());
            draw_flag("AIM",  mtr::interp::aim_snap_active());
        }
    }
    ImGui::End();
}

// === Camera tab: most-used controls front and center ========================
void tab_camera() {
    // FreeCam toggle: prominent full-width button with state-coloured fill.
    {
        bool active = mtr::freecam::active();
        ImGui::PushStyleColor(ImGuiCol_Button,
            active ? ImVec4(0.18f, 0.55f, 0.20f, 1.0f) : ImVec4(0.28f, 0.28f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            active ? ImVec4(0.22f, 0.65f, 0.24f, 1.0f) : ImVec4(0.40f, 0.40f, 0.45f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            active ? ImVec4(0.26f, 0.75f, 0.28f, 1.0f) : ImVec4(0.50f, 0.50f, 0.55f, 1.0f));
        if (ImGui::Button(active ? "FreeCam: ON     (F3 toggles)"
                                 : "FreeCam: off    (F3 toggles)",
                          ImVec2(-FLT_MIN, 40.0f))) {
            mtr::freecam::set_active(!active);
        }
        ImGui::PopStyleColor(3);
    }

    if (bool open = section_begin("FreeCam state & controls")) {
        if (mtr::freecam::active()) {
            float pos[3]; float yaw, pitch;
            mtr::freecam::get_pose(pos, &yaw, &pitch);
            ImGui::Text("Pos    %8.2f  %8.2f  %8.2f", pos[0], pos[1], pos[2]);
            ImGui::Text("Yaw  %7.1f deg     Pitch %7.1f deg",
                        yaw * 57.29578f, pitch * 57.29578f);
            ImGui::Spacing();

            float ms = mtr::freecam::move_speed();
            if (ImGui::SliderFloat("Move speed##fcms", &ms, 1.0f, 500.0f, "%.1f u/s",
                                   ImGuiSliderFlags_Logarithmic)) {
                mtr::freecam::set_move_speed(ms);
            }
            float mouse = mtr::freecam::mouse_sens() * 1000.0f;
            if (ImGui::SliderFloat("Mouse sens##fcsens", &mouse, 0.5f, 15.0f, "%.2f mrad/px")) {
                mtr::freecam::set_mouse_sens(mouse / 1000.0f);
            }
        } else {
            ImGui::TextDisabled("FreeCam is off. Toggle above or press F3 in-game.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Mouse=look  Wheel=speed  WASD=move  Space/C=up/down  Shift=4x");
        ImGui::TextDisabled("Arrows=look (fallback)  MMB=teleport request (dumps target)");
        ImGui::TextDisabled("Open menu/console (Insert/F2) to free the cursor for UI.");
        section_end(open);
    }

    if (bool open = section_begin("FOV override")) {
        const bool fov_on = mtr::fov::has_override();
        float fov_cur = mtr::fov::current();
        static float fov_ui = 90.0f;
        if (fov_on) fov_ui = fov_cur;
        ImGui::Text("%s   %.1f deg", fov_on ? "ACTIVE" : "off (engine value)", fov_cur);
        ImGui::Spacing();
        if (ImGui::Button("60"))  mtr::fov::set(60.0f);  ImGui::SameLine();
        if (ImGui::Button("75"))  mtr::fov::set(75.0f);  ImGui::SameLine();
        if (ImGui::Button("90"))  mtr::fov::set(90.0f);  ImGui::SameLine();
        if (ImGui::Button("100")) mtr::fov::set(100.0f); ImGui::SameLine();
        if (ImGui::Button("110")) mtr::fov::set(110.0f); ImGui::SameLine();
        if (ImGui::Button("Off##fov")) mtr::fov::set(0.0f);
        ImGui::SliderFloat("##fov", &fov_ui, 30.0f, 150.0f, "%.1f deg");
        ImGui::SameLine();
        if (ImGui::Button("Apply##fov")) mtr::fov::set(fov_ui);
        section_end(open);
    }
}

// === Display tab: aspect ratio (main world + HUD) ===========================
void tab_display() {
    if (bool open = section_begin("World aspect (main 3D camera)")) {
        if (!mtr::aspect::available()) {
            ImGui::TextDisabled("constant not located in Wilbur.exe (scan failed)");
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

    if (bool open = section_begin("UI aspect (HUD / menus / mini-games)")) {
        heading_with_info("Per-screen pillarbox for 2D UI",
            "Per-screen pillarbox via the sprite-batcher path — handles every "
            "2D UI element (HUD, menus, mini-games, loading screens). The first "
            "rule whose pattern matches the current top-screen name (case-"
            "insensitive substring) wins; its target aspect is applied as a "
            "pillarbox factor (target / screen). Aspect = 0 opts a screen out "
            "(stretches with screen).\n\n"
            "Implementation: hooks transform_apply_scale_via_stack (0x562AA0) "
            "and transform_apply_translate_via_stack (0x562AE0), substituting "
            "args at the matrix-builder level so the per-frame sprite-batch "
            "matrix gets the pillarbox factor on sx/tx.");
        ImGui::Spacing();

        // ---- Quick-setup: one-click recommended path -----------------------
        // Idempotent — install_defaults only seeds when the rule table is
        // empty, so re-clicking after the user added their own rules is safe.
        if (primary_button("Enable per-screen 4:3 pillarbox (recommended)",
                           ImVec2(-FLT_MIN, 32.0f))) {
            mtr::ui_aspect_rules::install_defaults();
            mtr::sprite_matrix::set_enabled(true);
            mtr::sprite_matrix::set_auto_from_rules(true);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::Spacing();

        // ---- Master toggles ------------------------------------------------
        bool en = mtr::sprite_matrix::enabled();
        if (ImGui::Checkbox("Enabled (master)##sprite_master", &en)) {
            mtr::sprite_matrix::set_enabled(en);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::SameLine();
        bool auto_mode = mtr::sprite_matrix::auto_from_rules();
        if (ImGui::Checkbox("Auto from rules (per-screen)##sprite_auto", &auto_mode)) {
            mtr::sprite_matrix::set_auto_from_rules(auto_mode);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::Spacing();

        // ---- Live position offset (drag to nudge UI) -----------------------
        // Applied to xform_b's a1/a2 (translate) AFTER the pillarbox factor.
        // Active whenever sprite_matrix is enabled (or pass-override is set).
        // Lets the user fine-tune UI placement without recompressing.
        // DragFloat = drag to change, ctrl+click to type, shift = 10×, alt = 0.1×.
        float ox = mtr::sprite_matrix::pos_offset_x();
        float oy = mtr::sprite_matrix::pos_offset_y();
        ImGui::TextUnformatted("Position offset (live — affects translate matrix):");
        ImGui::SameLine();
        info_pill(
            "Drag the field to change the value live. Ctrl+click to type a "
            "number directly. Hold Shift while dragging for a 10× larger step, "
            "or Alt for a 0.1× finer step. Range -5..+5 (clamped on input).");
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
        if (ImGui::TreeNode("[experimental] Phase 3 split-pass##split_tree")) {
            ImGui::SameLine();
            info_pill(
                "Per-entry vertex-bbox classifier: entries whose 4 verts are "
                "all in [0,1]^2 are pillarboxed; others (HUD/letterbox) are "
                "left untouched. Brittle for menu frames with verts slightly "
                "outside [0,1]^2 (they classify as HUD and break visually). "
                "Use the position offset above for live placement instead.");
            if (ImGui::Checkbox("Enable split-pass##split", &split_on)) {
                mtr::sprite_split::set_enabled(split_on);
            }
            if (split_on) {
                const uint64_t st  = mtr::sprite_split::last_total();
                const uint64_t smn = mtr::sprite_split::last_menu_count();
                const uint64_t shd = mtr::sprite_split::last_hud_count();
                ImGui::TextDisabled("  last frame: %llu entries (menu=%llu, hud=%llu)",
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
                "Granular per-element transforms. Each row is one slot; most "
                "slots are wildcards [*] that match every entry sharing the "
                "asset (state_key). When labelling \"menu text\" also moves "
                "HUD text, click \"Specialize\" — that splits off a child slot "
                "pinned to the exact UV / screen / quadrant variant the row "
                "last matched, so you can dial in different transforms for "
                "menu vs HUD even though they share an atlas.\n\n"
                "Variants are tagged [uv=XXXX s=YY q=Z]. Wildcard parents "
                "are tagged [*]. Specialised variants persist to ini.\n\n"
                "Slider controls: drag to change, Ctrl+click to type a number "
                "directly, Shift+drag = 10× step, Alt+drag = 0.1× finer step.");

            bool xon = mtr::sprite_xform::enabled();
            if (ImGui::Checkbox("Enabled##xform_en", &xon)) {
                mtr::sprite_xform::set_enabled(xon);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(must be on for transforms / hide to apply)");
            ImGui::TextDisabled("Total entries this frame: %llu",
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
            // Phase D research aid — dumps every tracked slot's state_key
            // target memory to CSV so we can identify the texture-object
            // layout offline. See state_key_probe.cpp for what's in the CSV.
            if (ImGui::SmallButton("Dump probe CSV##sk_probe")) {
                int n = mtr::state_key_probe::dump_all_to_csv();
                mtr::log::info("state_key probe: %d slots dumped", n);
            }
            ImGui::SameLine();
            info_pill(
                "[Research aid for Phase D auto-naming.] Dumps every tracked "
                "slot's state_key target memory (256 bytes) to "
                "Game/mtr-asi-state-key-probe.csv. Used to find the texture-"
                "object layout — once we know the m_pcName offset, sprite_xform "
                "can auto-populate names from asset paths instead of needing "
                "manual labelling. Send the CSV back for offline analysis.");
            ImGui::Spacing();

            // ---- Phase A diagnostics --------------------------------------
            // Per-frame distribution tally and one-shot full-frame CSV
            // dump of every entry the sprite-batcher renders. Built to
            // settle empirically what differentiates pickable from
            // non-pickable sprites — the picking gaps the user reports
            // can't be guessed correctly without seeing the data.
            if (ImGui::TreeNode("Diagnostic (entry distribution + CSV dump)##xform_diag")) {
                ImGui::SameLine();
                info_pill(
                    "Phase A diagnostics. Counters update every frame and "
                    "show what the sprite-batcher actually sees this frame: "
                    "how many entries have state_key=0 (currently invisible "
                    "to picking), how many use ext_positions (off-engine "
                    "geometry), and the flag-bit distribution.\n\n"
                    "\"Dump entries CSV\" captures one frame of every entry's "
                    "raw state to Game/mtr-asi-entries.csv. Run it once on a "
                    "screen where picking misbehaves (e.g. with HUD visible) "
                    "and send the CSV back so the right fix can be designed "
                    "from data rather than guesses.");

                const auto d = mtr::sprite_xform::frame_diag();
                ImGui::Text("Entries this frame: %u", d.total);
                ImGui::Text("  state_key == 0   : %u  (invisible to picking)",
                            d.state_key_zero);
                ImGui::Text("  ext_positions    : %u  (engine reads ext, not inline)",
                            d.ext_pos_used);
                ImGui::Text("  ext_uvs          : %u", d.ext_uvs_used);
                ImGui::Text("  flag bit 0x001   : %u", d.flag_bit_0x1);
                ImGui::Text("  flag bit 0x002   : %u", d.flag_bit_0x2);
                ImGui::Text("  flag bit 0x004   : %u", d.flag_bit_0x4);
                ImGui::Text("  flag bit 0x100   : %u  (alpha-mod-honoring)",
                            d.flag_bit_0x100);
                ImGui::Text("  flag bit 0x400   : %u  (alpha-mod-honoring alt)",
                            d.flag_bit_0x400);
                ImGui::Text("  other flag bits  : %u", d.flag_bit_other);
                ImGui::Text("  degenerate quads : %u", d.degenerate_quad);
                ImGui::Text("  alpha_mod == 0   : %u", d.zero_alpha);
                ImGui::Text("  pickable today   : %u  / %u",
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
            if (ImGui::Checkbox("Pin list##xform_pin", &pin_toggle)) {
                s_pinned = pin_toggle;
                if (s_pinned) recapture_pin();
            }
            ImGui::SameLine();
            info_pill(
                "Freezes the displayed list at the moment of toggling so rows "
                "stop reordering while you label them. Useful for animated UI "
                "(the F1 help robot, fading HUD pieces). Edits to name / group "
                "/ transforms still apply live to the underlying slot — only "
                "the list-of-rows is frozen.");

            ImGui::SameLine();
            bool pick_on = mtr::sprite_picking::pick_mode();
            if (ImGui::Checkbox("Pick mode##xform_pick", &pick_on)) {
                mtr::sprite_picking::set_pick_mode(pick_on);
                if (!pick_on) mtr::sprite_picking::clear_selection();
            }
            ImGui::SameLine();
            info_pill(
                "Click a sprite directly in the game to select its slot. The "
                "selected slot's row scrolls into view in the list and gets a "
                "cyan outline on screen. Click on overlapping sprites to cycle "
                "between layers (topmost wins on the first click).\n\n"
                "Pick mode only listens for clicks OUTSIDE the menu window. "
                "Clicks inside the menu drive the menu as usual. Press Esc to "
                "deselect.");

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
                "When on, dragging the gizmo on a sprite that belongs to a "
                "group also moves / scales every other sprite in the same "
                "group. Right-click reset still affects the entire group. "
                "Slots without a group are unaffected — only the picked "
                "slot moves. Use \"Auto-group from paths\" below to bulk-"
                "create groups from asset directory names.");

            // Auto-group toggle + one-shot retroactive button.
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
                "Auto-derive a slot's group from its auto-named asset path's "
                "parent directory (e.g. ui/menu/main_menu/wilbur_smile → "
                "main_menu). Toggle = apply to NEW slots as they appear. "
                "\"Apply now\" = walk every existing slot once and fill in "
                "empty groups. Manual groups are never overwritten.");

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

            ImGui::Text("Top %d slots%s, grouped by user-assigned group:",
                        n, s_pinned ? " (pinned)" : " this frame");
            ImGui::Spacing();

            // Group rows by `group` field. Walk once to bucket. Empty
            // group → "Ungrouped" bucket. Stable ordering within each
            // bucket (by frame_count desc, since the source array is sorted).
            constexpr int kMaxGroups   = 16;
            constexpr int kIdxsPerGrp  = kMaxRows;
            char group_names[kMaxGroups][32] = {{0}};
            int  group_idxs [kMaxGroups][kIdxsPerGrp];
            int  group_sizes[kMaxGroups]  = {0};
            int  num_groups = 0;
            for (int i = 0; i < n; ++i) {
                const char* gn = slots[i].group[0] ? slots[i].group : "Ungrouped";
                int gi = -1;
                for (int j = 0; j < num_groups; ++j) {
                    if (std::strcmp(group_names[j], gn) == 0) { gi = j; break; }
                }
                if (gi < 0) {
                    if (num_groups >= kMaxGroups) continue;
                    gi = num_groups++;
                    std::strncpy(group_names[gi], gn, 31);
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
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::InputTextWithHint("##xname", "name (e.g. wilbur smile)",
                                              info.name, sizeof(info.name))) {
                    mtr::sprite_xform::set_name_at(info.slot_idx, info.name);
                }
                if (name_is_auto) ImGui::PopStyleColor();
                if (ImGui::IsItemDeactivatedAfterEdit()) save_now = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f);
                if (ImGui::InputTextWithHint("##xgrp", "group (e.g. HUD)",
                                              info.group, sizeof(info.group))) {
                    mtr::sprite_xform::set_group_at(info.slot_idx, info.group);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) save_now = true;
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
                if (is_selected) {
                    // Selection wash: cyan-tinted full-width band + cyan
                    // left bar, overrides the stripe and state_key-color
                    // bar so the picked row is unmistakable.
                    dl->AddRectFilled(
                        ImVec2(x_left, row_top.y - 2.0f),
                        ImVec2(x_right, row_bot.y - 2.0f),
                        IM_COL32(80, 200, 230, 60));
                    dl->AddRectFilled(
                        ImVec2(x_left + 2.0f, row_top.y - 1.0f),
                        ImVec2(x_left + 5.0f, row_bot.y - 3.0f),
                        IM_COL32(80, 220, 250, 255));
                } else {
                    if (stripe) {
                        dl->AddRectFilled(
                            ImVec2(x_left, row_top.y - 2.0f),
                            ImVec2(x_right, row_bot.y - 2.0f),
                            IM_COL32(255, 255, 255, 10));
                    }
                    const uint32_t h = info.state_key * 0x9E3779B1u;
                    const ImU32 bar = IM_COL32(
                        static_cast<uint8_t>((h >>  8) | 0xC0),
                        static_cast<uint8_t>((h >> 16) | 0xC0),
                        static_cast<uint8_t>((h >>  0) | 0xC0),
                        220);
                    dl->AddRectFilled(
                        ImVec2(x_left + 2.0f, row_top.y - 1.0f),
                        ImVec2(x_left + 5.0f, row_bot.y - 3.0f),
                        bar);
                }
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
                // Stable PushID via group-name hash so the CollapsingHeader's
                // open/closed state survives across frames even when the
                // group's index in `group_names` shifts (slot creation /
                // eviction / re-bucketing).
                ImGui::PushID(group_id_hash(group_names[g]));
                char hdr[64];
                std::snprintf(hdr, sizeof(hdr), "%s  (%d slot%s)",
                    group_names[g], group_sizes[g],
                    group_sizes[g] == 1 ? "" : "s");
                const bool default_open = (std::strcmp(group_names[g], "Ungrouped") != 0);
                ImGuiTreeNodeFlags flg = ImGuiTreeNodeFlags_Framed;
                if (default_open) flg |= ImGuiTreeNodeFlags_DefaultOpen;
                if (ImGui::CollapsingHeader(hdr, flg)) {
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

                    // Bulk-add offset: tracks last-frame snapshot, applies
                    // delta on change. The static buffers reset whenever
                    // the user enters values or clicks a button.
                    static char    s_bulk_group_owner[32] = {0};
                    static float   s_bulk_dx = 0.0f;
                    static float   s_bulk_dy = 0.0f;
                    static float   s_bulk_dx_prev = 0.0f;
                    static float   s_bulk_dy_prev = 0.0f;
                    const bool same_owner = std::strcmp(s_bulk_group_owner, group_names[g]) == 0;
                    if (!same_owner) {
                        // Different group active — reset deltas so nothing
                        // accidentally applies. (One bulk-edit context at a time.)
                        s_bulk_dx = s_bulk_dy = s_bulk_dx_prev = s_bulk_dy_prev = 0.0f;
                    }
                    ImGui::TextDisabled("Add offset:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(85.0f);
                    bool dx_changed = ImGui::DragFloat("dx##bulk_dx",
                        &s_bulk_dx, 0.001f, -5.0f, 5.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(85.0f);
                    bool dy_changed = ImGui::DragFloat("dy##bulk_dy",
                        &s_bulk_dy, 0.001f, -5.0f, 5.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Apply##bulk_apply")) {
                        for (int k = 0; k < group_sizes[g]; ++k) {
                            const int row = group_idxs[g][k];
                            auto& info = slots[row];
                            const float new_ox = info.offset_x + s_bulk_dx;
                            const float new_oy = info.offset_y + s_bulk_dy;
                            mtr::sprite_xform::set_transform_at(info.slot_idx,
                                new_ox, new_oy, info.scale_x, info.scale_y, info.hidden);
                            info.offset_x = new_ox;
                            info.offset_y = new_oy;
                        }
                        s_bulk_dx = s_bulk_dy = 0.0f;
                        std::strncpy(s_bulk_group_owner, group_names[g],
                                     sizeof(s_bulk_group_owner) - 1);
                        s_bulk_group_owner[sizeof(s_bulk_group_owner) - 1] = 0;
                        mtr::ui_aspect_rules::request_save();
                    }
                    (void)dx_changed; (void)dy_changed;
                    (void)s_bulk_dx_prev; (void)s_bulk_dy_prev;

                    ImGui::Separator();
                    for (int k = 0; k < group_sizes[g]; ++k) {
                        render_one_slot(group_idxs[g][k], k);
                    }
                    ImGui::Unindent(8.0f);
                }
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::TextUnformatted("Tip: ");
            ImGui::SameLine();
            info_pill(
                "Hold \"Hilite\" to force-visible the matching sprites "
                "(alpha=255). Toggle \"Hide\" to make them invisible — useful "
                "for binary-searching to find which slot is which sprite.\n\n"
                "If one slot moves multiple unrelated UI elements (e.g. menu "
                "text + HUD text share a font atlas), click \"Specialize\" "
                "while the offending sprite is on screen — that creates a "
                "new slot pinned to the exact variant currently rendering, "
                "and the wildcard parent stops affecting it.\n\n"
                "Slot capacity is 64 (wildcard + variants combined). Edited "
                "slots are pinned; only unedited wildcards get auto-evicted "
                "when capacity fills.\n\n"
                "Persistence: state_keys are heap pointers, so cross-session "
                "matches are best-effort. Specialised variants are persisted "
                "by their full composite key — same caveat applies.");
            ImGui::TreePop();
        }
        ImGui::Spacing();

        // ---- Live status panel: top screen + matched rule + factor ---------
        char top[64] = {0};
        const bool have_top = mtr::screen_push::current_top_name(top, sizeof(top));
        const int  depth    = mtr::screen_push::stack_depth();
        const float screen_aspect = mtr::aspect::current();

        ImGui::Text("Top screen (depth=%d):", depth);
        ImGui::SameLine();
        if (have_top) {
            ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "%s", top);
        } else {
            ImGui::TextDisabled("(empty)");
        }

        char matched_pat[48] = {0};
        float matched_aspect = 0.0f;
        const bool matched = mtr::ui_aspect_rules::resolve_match(
            top, matched_pat, sizeof(matched_pat), &matched_aspect);
        if (matched) {
            const float factor = (screen_aspect > 0.0f && matched_aspect > 0.0f)
                                  ? matched_aspect / screen_aspect : 1.0f;
            ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f),
                "matched rule: \"%s\"  target=%.4f  screen=%.4f  factor=%.4f",
                matched_pat, matched_aspect, screen_aspect, factor);
        } else if (have_top) {
            ImGui::TextDisabled("no rule matched (pass-through)");
        } else {
            ImGui::TextDisabled("no top screen — pass-through");
        }

        // Full stack listing (collapsed by default — usually 0-4 entries).
        if (depth > 0 && ImGui::TreeNode("Screen stack (top last)")) {
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

        // ---- Rules table ---------------------------------------------------
        const size_t n   = mtr::ui_aspect_rules::rule_count();
        const size_t cap = mtr::ui_aspect_rules::max_rules();
        ImGui::Text("Per-screen rules (%zu / %zu):", n, cap);
        ImGui::Spacing();

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

    if (bool open = section_begin("[advanced] sprite-batcher manual override")) {
        heading_with_info("Manual fallback for the sprite-batcher matrix",
            "PRIMARY controls live in the \"UI aspect\" section above (per-"
            "screen rules + auto mode). This section is for direct slider "
            "tuning when you want a uniform override regardless of which "
            "screen is up.\n\n"
            "Static RE of the targets:\n"
            "  - transform_apply_scale_via_stack(2.0, -2.0, 1.0): "
            "scale(sx, sy, sz) matrix multiplied onto stack.\n"
            "  - transform_apply_translate_via_stack(-2.0, -2.0, 0.0): "
            "translate(tx, ty, tz) matrix multiplied onto stack.\n"
            "Combined: vertex (x,y,z) -> (sx*x + tx, sy*y + ty, sz*z + tz).\n\n"
            "NOTE: When \"Auto from rules\" is on (above), it overrides "
            "matrix-A.sx and matrix-B.tx with the per-screen factor; the "
            "sliders below have no effect on those args while auto is on.");
        ImGui::Spacing();

        // Master enable mirrors the toggle in the section above — keep both
        // editable so the user can flip from either entry point.
        bool en = mtr::sprite_matrix::enabled();
        if (ImGui::Checkbox("Enabled (master)##sprite_master2", &en)) {
            mtr::sprite_matrix::set_enabled(en);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset all to 1.0 + disable")) {
            mtr::sprite_matrix::reset();
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::Spacing();

        // Pillarbox quick-set: write factor into BOTH matrix-A.sx and matrix-
        // B.tx so the narrower X clip-space extent stays centered. Useful when
        // you want a uniform pillarbox without the per-screen rule machinery.
        const float screen_aspect = mtr::aspect::current();
        const float factor_4_3   = (4.0f  / 3.0f)  / screen_aspect;
        const float factor_16_10 = (16.0f / 10.0f) / screen_aspect;
        ImGui::TextDisabled("Pillarbox factor (target / screen):");
        ImGui::TextDisabled("  4:3 in current screen:   %.4f", factor_4_3);
        ImGui::TextDisabled("  16:10 in current screen: %.4f", factor_16_10);
        if (ImGui::Button("Pillarbox 4:3 (uniform)##manual43")) {
            mtr::sprite_matrix::set_mul_a(factor_4_3, 1.0f, 1.0f);
            mtr::sprite_matrix::set_mul_b(factor_4_3, 1.0f, 1.0f);
            mtr::sprite_matrix::set_enabled(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Pillarbox 16:10 (uniform)##manual1610")) {
            mtr::sprite_matrix::set_mul_a(factor_16_10, 1.0f, 1.0f);
            mtr::sprite_matrix::set_mul_b(factor_16_10, 1.0f, 1.0f);
            mtr::sprite_matrix::set_enabled(true);
        }
        ImGui::Spacing();

        if (ImGui::TreeNode("Per-component sliders")) {
            // Scale matrix sliders (matrix A: scale = (sx, sy, sz)).
            float aa1 = mtr::sprite_matrix::mul_a_a1();
            float aa2 = mtr::sprite_matrix::mul_a_a2();
            float aa3 = mtr::sprite_matrix::mul_a_a3();
            ImGui::TextDisabled("scale matrix (orig sx=2.0 sy=-2.0 sz=1.0):");
            bool a_changed = false;
            a_changed |= ImGui::SliderFloat("mul sx##spritea1", &aa1, 0.25f, 4.0f, "%.4f");
            a_changed |= ImGui::SliderFloat("mul sy##spritea2", &aa2, 0.25f, 4.0f, "%.4f");
            a_changed |= ImGui::SliderFloat("mul sz##spritea3", &aa3, 0.25f, 4.0f, "%.4f");
            if (a_changed) mtr::sprite_matrix::set_mul_a(aa1, aa2, aa3);
            ImGui::Text("=> scale(%.3f, %.3f, %.3f)", 2.0f * aa1, -2.0f * aa2, 1.0f * aa3);
            ImGui::Spacing();

            // Translate matrix sliders (matrix B: translate = (tx, ty, tz)).
            float ba1 = mtr::sprite_matrix::mul_b_a1();
            float ba2 = mtr::sprite_matrix::mul_b_a2();
            float ba3 = mtr::sprite_matrix::mul_b_a3();
            ImGui::TextDisabled("translate matrix (orig tx=-2.0 ty=-2.0 tz=0.0):");
            bool b_changed = false;
            b_changed |= ImGui::SliderFloat("mul tx##spriteb1", &ba1, 0.25f, 4.0f, "%.4f");
            b_changed |= ImGui::SliderFloat("mul ty##spriteb2", &ba2, 0.25f, 4.0f, "%.4f");
            b_changed |= ImGui::SliderFloat("mul tz##spriteb3", &ba3, 0.25f, 4.0f, "%.4f");
            if (b_changed) mtr::sprite_matrix::set_mul_b(ba1, ba2, ba3);
            ImGui::Text("=> translate(%.3f, %.3f, %.3f)", -2.0f * ba1, -2.0f * ba2, 0.0f * ba3);
            ImGui::TreePop();
        }
        section_end(open);
    }

    if (bool open = section_begin("[Phase3 M3.2] sprite-batcher list probe")) {
        heading_with_info("Capture per-entry data to CSV for offline analysis",
            "Captures per-entry data from the sprite-batcher list "
            "(g_sprite_list_head @ 0x7271E8) before render_sprite_batcher "
            "consumes it each frame. Goal: identify a menu-vs-HUD signal "
            "(state_key, sort_key, position) so Phase 3 can split the "
            "render pass per-classification.\n\n"
            "Workflow: open the game state you want to characterize "
            "(main menu / in-game with HUD / pause overlay / loading / "
            "mini-game), click \"Arm 60 frames\", let it capture, then "
            "open the CSV in Excel/sqlite and pivot on (top_screen, "
            "state_key) to see which keys cluster per state.\n\n"
            "Off by default — zero overhead when disarmed.");
        ImGui::Spacing();

        if (!mtr::sprite_probe::installed()) {
            ImGui::TextDisabled("probe not installed (hook failed at startup)");
            section_end(open);
        } else {
            const bool armed = mtr::sprite_probe::armed();
            ImGui::Text("Status: ");
            ImGui::SameLine();
            if (armed) {
                ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                    "ARMED  (%d frames remaining, %llu entries captured so far)",
                    mtr::sprite_probe::frames_remaining(),
                    static_cast<unsigned long long>(mtr::sprite_probe::total_captured()));
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "off");
            }
            ImGui::Text("Last frame: %llu entries",
                static_cast<unsigned long long>(mtr::sprite_probe::last_frame_count()));
            ImGui::Spacing();

            ImGui::BeginDisabled(armed);
            if (ImGui::Button("Arm 30 frames##sp_arm30"))   mtr::sprite_probe::arm(30);
            ImGui::SameLine();
            if (ImGui::Button("Arm 60 frames##sp_arm60"))   mtr::sprite_probe::arm(60);
            ImGui::SameLine();
            if (ImGui::Button("Arm 180 frames##sp_arm180")) mtr::sprite_probe::arm(180);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!armed);
            if (ImGui::Button("Disarm##sp_disarm")) mtr::sprite_probe::disarm();
            ImGui::EndDisabled();
            ImGui::Spacing();

            char path[MAX_PATH] = {0};
            if (mtr::sprite_probe::csv_path(path, sizeof(path))) {
                ImGui::TextDisabled("CSV: %s", path);
            }
            section_end(open);
        }
    }
}

// === World tab: draw distance ===============================================
void tab_world() {
    if (bool open = section_begin("Draw distance (camera far plane)")) {
        heading_with_info("Camera-relative far cull plane",
            "Engine caches the frustum buffer at outer_cam+0xD4 once at scene "
            "init and never rebuilds it; we write directly to its far plane "
            "(offset +16..31) and far corners (+352..399) every frame inside "
            "hk_PerCameraApply. Pure data-level override on the engine's own "
            "buffer — no code patches.");
        ImGui::Spacing();

        const bool dd_on = mtr::draw_dist::has_override();
        const float dd_cur = mtr::draw_dist::current();
        static float dd_ui = 5000.0f;
        if (dd_on) dd_ui = dd_cur;

        ImGui::Text("Status:");
        ImGui::SameLine();
        ImGui::TextColored(dd_on ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f)
                                 : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           dd_on ? "ACTIVE  %.0f" : "off (engine far = 1500)", dd_cur);
        ImGui::Spacing();

        if (ImGui::Button("3000##dd"))   mtr::draw_dist::set(3000.0f);  ImGui::SameLine();
        if (ImGui::Button("5000##dd"))   mtr::draw_dist::set(5000.0f);  ImGui::SameLine();
        if (ImGui::Button("10000##dd"))  mtr::draw_dist::set(10000.0f); ImGui::SameLine();
        if (ImGui::Button("50000##dd"))  mtr::draw_dist::set(50000.0f); ImGui::SameLine();
        if (ImGui::Button("Off##dd"))    mtr::draw_dist::set(0.0f);
        ImGui::SliderFloat("##dd", &dd_ui, 100.0f, 100000.0f, "%.0f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        if (ImGui::Button("Apply##dd")) mtr::draw_dist::set(dd_ui);
        section_end(open);
    }

    if (bool open = section_begin("Periphery culling (corner-cull suspect)")) {
        heading_with_info("MeshLOD.PeripheryRejectAngle / Dist",
            "RE'd encoding:\n"
            " - Angle storage = cos²(half_cone_deg). Default 0.39 = ~51° cone.\n"
            "   To DISABLE write 0.0 (= cos²(90°) = full hemisphere).\n"
            " - Dist storage may be linear OR dist² (common \"real\" setter\n"
            "   sub_429D20 squares input). Default raw 1500. Disable with 1e12.\n"
            "Direct writes to engine cvar globals 0x745B58 / 0x745B54.");
        ImGui::Spacing();

        const float ang_raw = mtr::lod::periphery_reject_angle();
        const float ang_deg = mtr::lod::periphery_reject_angle_deg();
        const float dist_raw = mtr::lod::periphery_reject_dist();
        const float dist_decoded = mtr::lod::periphery_reject_dist_decoded();
        const float acc_d = mtr::lod::periphery_accept_dist();

        ImGui::Text("Angle raw: %.4f   decoded: %.1f deg", ang_raw, ang_deg);
        ImGui::Text("Dist  raw: %.1f   sqrt:    %.1f      Accept(raw): %.1f",
                    dist_raw, dist_decoded, acc_d);
        ImGui::Spacing();

        static float ang_deg_ui = 51.0f;
        if (ImGui::SliderFloat("Half-cone (deg)##pra", &ang_deg_ui, 0.0f, 90.0f, "%.1f")) {
            mtr::lod::set_periphery_reject_angle_deg(ang_deg_ui);
        }
        static float dist_lin_ui = 1500.0f;
        if (ImGui::SliderFloat("Reject dist (linear, written squared)##prd",
                               &dist_lin_ui, 1.0f, 1000000.0f, "%.0f",
                               ImGuiSliderFlags_Logarithmic)) {
            mtr::lod::set_periphery_reject_dist_squared(dist_lin_ui);
        }
        ImGui::Spacing();
        if (ImGui::Button("Disable periphery (angle=90°, dist=1e6)")) {
            mtr::lod::disable_periphery_cull();
            ang_deg_ui  = 90.0f;
            dist_lin_ui = 1.0e6f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Restore defaults##pr")) {
            mtr::lod::restore_periphery_cull_defaults();
            ang_deg_ui  = 51.0f;
            dist_lin_ui = 38.7f;
        }
        section_end(open);
    }

    if (bool open = section_begin("[experimental] Force-pass vis_test (4 call sites)")) {
        heading_with_info("Force the per-object visibility test to always pass",
            "Patches all 4 known call sites of sub_4E0B90 (per-object visibility "
            "thunk) at once: sub_4BC340 (scene-tree list update — upstream), "
            "sub_4C3790 (main render loop), orphan @ 0x4CBAC7, sub_4E6A20 (reflection). "
            "Replaces each `call sub_4E0B90` with `mov al,1; nop*3`. Tests whether "
            "vis_test is the cull gate at any of those sites.");
        ImGui::Spacing();
        bool fv = mtr::force_vis::active();
        if (ImGui::Checkbox("Force-pass vis_test (all 4 sites)", &fv)) {
            mtr::force_vis::set(fv);
        }
        ImGui::SameLine();
        ImGui::TextColored(fv ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           fv ? "  ACTIVE" : "  off");
        section_end(open);
    }

    if (bool open = section_begin("[diagnostic] vis_test probe (IAT-slot patch)")) {
        heading_with_info("Per-callsite vis_test pass/fail counters + central force-pass",
            "IAT-slot patch at 0xF92F34 (the SecuROM thunk's indirect target). "
            "Routes ALL calls to sub_4E0B90 through a wrapper that counts pass/fail "
            "per call site. Toggle force-pass to bypass the test centrally. Use this "
            "to characterize the real cull behavior independently of the call-site "
            "rewrites above. Counters update each frame (snapshotted in EndScene).");
        ImGui::Spacing();

        const bool armed = mtr::vis_test_probe::active();
        ImGui::Text("Probe: ");
        ImGui::SameLine();
        ImGui::TextColored(armed ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                           armed ? "ARMED" : "not armed (waiting for SecuROM unpack)");

        bool fp = mtr::vis_test_probe::force_pass();
        if (ImGui::Checkbox("Force-pass via wrapper", &fp)) {
            mtr::vis_test_probe::set_force_pass(fp);
        }
        ImGui::SameLine();
        ImGui::TextColored(fp ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           fp ? " ACTIVE" : " off");
        ImGui::Spacing();

        const uint64_t total_f = mtr::vis_test_probe::last_frame_total();
        const uint64_t pass_f  = mtr::vis_test_probe::last_frame_pass();
        const uint64_t total_c = mtr::vis_test_probe::cum_total();
        const uint64_t pass_c  = mtr::vis_test_probe::cum_pass();
        const float pct_f = (total_f > 0) ? (100.0f * static_cast<float>(pass_f) / static_cast<float>(total_f)) : 0.0f;
        const float pct_c = (total_c > 0) ? (100.0f * static_cast<float>(pass_c) / static_cast<float>(total_c)) : 0.0f;

        ImGui::Text("Last frame: %llu calls, %llu pass (%.1f%%)",
                    static_cast<unsigned long long>(total_f),
                    static_cast<unsigned long long>(pass_f), pct_f);
        ImGui::Text("Cumulative: %llu calls, %llu pass (%.1f%%)",
                    static_cast<unsigned long long>(total_c),
                    static_cast<unsigned long long>(pass_c), pct_c);
        ImGui::Spacing();

        ImGui::TextDisabled("Per-call-site (last frame):");
        for (size_t i = 0; i < mtr::vis_test_probe::num_sites(); ++i) {
            const uint64_t c = mtr::vis_test_probe::last_frame_site(i);
            ImGui::Text("  %s: %llu", mtr::vis_test_probe::site_tag(i),
                        static_cast<unsigned long long>(c));
        }
        section_end(open);
    }

    if (bool open = section_begin("[diagnostic] scene visibility tracker")) {
        heading_with_info("Live counters for (scene+104) bit 0 writers",
            "Hooks scene_set_visible (sub_4AABC0) and script_set_instance_hidden "
            "(sub_5E3DC0) — the two writers of (scene+104) bit 0 that scripts can "
            "drive. If panning to corners makes hides/sec spike, the corner-cull "
            "is going through one of these paths. Stable near-zero counts mean the "
            "cull is happening through some other mechanism (and (scene+104) bit 0 "
            "is confirmed not the corner-cull driver).");
        ImGui::Spacing();

        const uint64_t lh  = mtr::scene_vis_log::last_hides();
        const uint64_t ls  = mtr::scene_vis_log::last_shows();
        const uint64_t lsc = mtr::scene_vis_log::last_script_calls();
        const uint64_t lsh = mtr::scene_vis_log::last_script_hides();
        const uint64_t lss = mtr::scene_vis_log::last_script_shows();
        const uint64_t ch  = mtr::scene_vis_log::cum_hides();
        const uint64_t cs  = mtr::scene_vis_log::cum_shows();
        const uint64_t csc = mtr::scene_vis_log::cum_script_calls();

        ImGui::Text("Last frame:  %llu hides, %llu shows  (scene_set_visible)",
                    static_cast<unsigned long long>(lh),
                    static_cast<unsigned long long>(ls));
        ImGui::Text("Last frame:  %llu script calls (%llu hide-result, %llu show-result)",
                    static_cast<unsigned long long>(lsc),
                    static_cast<unsigned long long>(lsh),
                    static_cast<unsigned long long>(lss));
        ImGui::Text("Cumulative:  %llu hides, %llu shows, %llu script calls",
                    static_cast<unsigned long long>(ch),
                    static_cast<unsigned long long>(cs),
                    static_cast<unsigned long long>(csc));
        ImGui::Spacing();

        const int n_sticky = mtr::scene_vis_log::sticky_scene_count();
        ImGui::TextDisabled("Distinct scenes hidden this frame: %d", n_sticky);
        for (int i = 0; i < n_sticky; ++i) {
            uint32_t addr = mtr::scene_vis_log::sticky_scene_at(i);
            if (addr) ImGui::Text("  scene[%d] = 0x%08X", i, addr);
        }
        section_end(open);
    }

    if (bool open = section_begin("LOD distances (MeshLOD)")) {
        heading_with_info("Mesh-detail distance thresholds",
            "Distance thresholds at which the engine drops mesh detail. Engine "
            "defaults: Focus=100, High=250, Medium=500. Increase to push detail "
            "farther; decrease for perf.");
        ImGui::Spacing();

        float focus  = mtr::lod::focus_dist();
        float high   = mtr::lod::high_dist();
        float medium = mtr::lod::medium_dist();
        ImGui::Text("Focus  %.1f   High  %.1f   Medium  %.1f", focus, high, medium);
        if (ImGui::SliderFloat("Focus##fd",  &focus,  10.0f,  5000.0f, "%.1f"))  mtr::lod::set_focus_dist(focus);
        if (ImGui::SliderFloat("High##hd",   &high,   10.0f,  5000.0f, "%.1f"))  mtr::lod::set_high_dist(high);
        if (ImGui::SliderFloat("Medium##md", &medium, 10.0f, 10000.0f, "%.1f"))  mtr::lod::set_medium_dist(medium);
        section_end(open);
    }

    if (bool open = section_begin("Global LOD scale (ActorLOD)")) {
        heading_with_info("Global multiplier on actor LOD distance",
            "ActorLOD.LODScale — global multiplier applied to actor LOD distance "
            "decisions. > 1.0 keeps higher detail farther; < 1.0 drops detail "
            "earlier. Engine default 1.0.");
        ImGui::Spacing();

        float ls = mtr::lod::lod_scale();
        ImGui::Text("LODScale = %.3f", ls);
        if (ImGui::SliderFloat("LODScale##ls", &ls, 0.1f, 10.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) {
            mtr::lod::set_lod_scale(ls);
        }
        ImGui::SameLine();
        if (ImGui::Button("1.0##ls_reset")) mtr::lod::set_lod_scale(1.0f);
        section_end(open);
    }

    if (bool open = section_begin("Fog")) {
        heading_with_info("Toggle engine fog off",
            "Engine fog (D3DRS_FOGENABLE + scene fogEnabled cvar at 0x745279). "
            "Disable writes 0 to the cvar BYTE every frame and forces D3DRS_FOGENABLE "
            "to 0 in the SetRenderState hook.");
        ImGui::Spacing();
        bool fd = mtr::scene::fog_disabled();
        if (ImGui::Checkbox("Disable fog", &fd)) mtr::scene::set_fog_disabled(fd);
        ImGui::SameLine();
        ImGui::TextColored(fd ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           fd ? "  ACTIVE" : "  off");
        section_end(open);
    }
}

// === Tools tab: console + screenshot + status ===============================
void tab_tools() {
    if (bool open = section_begin("Console")) {
        bool con_vis = mtr::console::is_visible();
        if (ImGui::Checkbox("Show console (F2 also toggles)", &con_vis)) {
            mtr::console::set_visible(con_vis);
        }
        ImGui::SameLine();
        info_pill(
            "Engine REPL via console_printf. Output mirrored to mtr-asi.log. "
            "Try: `context view`, `list`, `set Verbosity debug`.");
        section_end(open);
    }

    if (bool open = section_begin("Screenshot")) {
        if (ImGui::Button("Capture now (F12)", ImVec2(160.0f, 0.0f))) {
            mtr::screenshot::request();
        }
        ImGui::SameLine();
        if (mtr::screenshot::last_path_tag() > 0) {
            ImGui::TextWrapped("Last: %s", mtr::screenshot::last_path());
        } else {
            ImGui::TextDisabled("F12 captures the current view (incl. this menu).");
        }
        section_end(open);
    }

    if (bool open = section_begin("Cvar dump")) {
        heading_with_info("Dump every registered cvar to a text file",
            "Hooks the engine's eight typed-variant registration functions to "
            "record every (group, name, address) the game registers. Writes "
            "mtr_cvars.txt next to Wilbur.exe — grep it for lod / cull / draw "
            "/ fade / occlud / sector to find optimization knobs.");
        ImGui::Spacing();
        ImGui::Text("Registered so far: %zu", mtr::cvar_dump::count());
        ImGui::Spacing();
        if (ImGui::Button("Dump cvars to mtr_cvars.txt", ImVec2(220.0f, 0.0f))) {
            bool ok = mtr::cvar_dump::dump_default();
            mtr::log::info("cvar_dump: dump button -> %s", ok ? "ok" : "FAILED");
        }
        section_end(open);
    }

    if (bool open = section_begin("FPS limiter")) {
        heading_with_info("Cap frame rate via EndScene spin-wait",
            "Caps frame rate by spinning at the end of each EndScene call. "
            "Game logic is QPC/dt-based (game_get_time_ms feeds 30+ engine "
            "functions), so capping should not change game speed — but "
            "validate empirically: at 30 vs 144 FPS, jump height / walk "
            "speed / animation timing must match.");
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
                           en ? "  ACTIVE" : "  off");

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

    if (bool open = section_begin("Performance overlay")) {
        bool en = g_fps_overlay.load();
        if (ImGui::Checkbox("Show FPS overlay", &en)) {
            g_fps_overlay.store(en);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(visible while game is running, even when this menu is closed)");

        int corner = g_fps_overlay_corner.load();
        const char* items[] = { "Top-left", "Top-right", "Bottom-left", "Bottom-right" };
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Corner##fps", &corner, items, IM_ARRAYSIZE(items))) {
            g_fps_overlay_corner.store(corner);
        }
        section_end(open);
    }

    if (bool open = section_begin("Decouple sim from render")) {
        heading_with_info("Run physics/anim/AI at a fixed rate while rendering at any rate",
            "Engine integrates physics, anim and PathCam at a hardcoded 0.003 "
            "step per render frame — so 240 Hz render makes everything 4x "
            "fast. Decouple gates those ticks behind a real-time clock so "
            "they only run target_hz times per second; render keeps running "
            "as fast as the limiter allows. Stack the M3 view interp + M4 "
            "Wilbur interp + M5 NPC interp on top for visible 240 Hz "
            "fluidity without breaking sim speed.");
        ImGui::Spacing();

        // M6.4 preset profiles. One-click apply that sets the underlying
        // toggles. Users can then tweak individually — presets are
        // convenience, not modes.
        ImGui::TextDisabled("Presets:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Quality##decouple_preset")) {
            mtr::sim_decouple::set_mode(mtr::sim_decouple::Mode::THROTTLE);
            mtr::sim_decouple::set_target_hz(60);
            mtr::interp::set_view_interp_enabled(true);
            mtr::interp::set_player_interp_enabled(true);
            mtr::interp::set_npc_interp_enabled(true);
            mtr::sim_decouple::set_auto_disable_in_minigame(true);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Performance##decouple_preset")) {
            mtr::sim_decouple::set_mode(mtr::sim_decouple::Mode::THROTTLE);
            mtr::sim_decouple::set_target_hz(60);
            mtr::interp::set_view_interp_enabled(false);
            mtr::interp::set_player_interp_enabled(false);
            mtr::interp::set_npc_interp_enabled(false);
            mtr::sim_decouple::set_auto_disable_in_minigame(true);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Compatibility##decouple_preset")) {
            mtr::sim_decouple::set_mode(mtr::sim_decouple::Mode::OFF);
            mtr::interp::set_view_interp_enabled(false);
            mtr::interp::set_player_interp_enabled(false);
            mtr::interp::set_npc_interp_enabled(false);
        }
        ImGui::SameLine();
        info_pill(
            "Quality = throttle 60 Hz + all interp (camera+wilbur+NPCs) on. "
            "Performance = throttle only, no interp (low-cost VRR). "
            "Compatibility = decouple fully off (baseline). Presets are "
            "convenience; tweak any toggle below afterward.");
        ImGui::Spacing();

        using SDM = mtr::sim_decouple::Mode;
        const SDM cur_mode = mtr::sim_decouple::mode();
        bool throttle_on = (cur_mode == SDM::THROTTLE);
        if (ImGui::Checkbox("Throttle sim ticks (M1)", &throttle_on)) {
            mtr::sim_decouple::set_mode(throttle_on ? SDM::THROTTLE : SDM::OFF);
        }
        ImGui::SameLine();
        info_pill(
            "When ON, all 12 throttle hooks (sim_aggregator + pathcam + "
            "overlay tween + UV scroll + alt-pump integrators) are gated "
            "to the call rate below. When OFF, behaves exactly like "
            "baseline (no decimation). Note: only call rate=60 preserves "
            "authored game speed — see slider tooltip.");

        ImGui::Spacing();
        int tgt = mtr::sim_decouple::target_hz();
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::SliderInt("Sim call rate", &tgt, 15, 240, "%d Hz")) {
            mtr::sim_decouple::set_target_hz(tgt);
        }
        ImGui::SameLine();
        info_pill(
            "Number of sim_aggregator calls per real second. "
            "60 = match the engine's authored cadence (CORRECT speed). "
            "Other values change GAME SPEED, not fidelity: 30 = slow-mo, "
            "120 = 2x fast-forward. The engine's physics step is fixed at "
            "0.003 per call, not per real second — until we add an "
            "accumulator pattern, only 60 preserves speed.");
        ImGui::SameLine();
        if (ImGui::SmallButton("60 (correct)##sim_hz")) { mtr::sim_decouple::set_target_hz(60); }
        if (tgt != 60) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "  ! Game runs %s at this rate (only 60 = correct speed)",
                tgt < 60 ? "in slow-motion" : "fast-forward");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Live measurements:");
        const double r_hz  = mtr::sim_decouple::measured_render_hz();
        const double s_hz  = mtr::sim_decouple::measured_sim_hz();
        ImGui::Text("  RENDER: %6.1f Hz", r_hz);
        if (s_hz > 0.001) {
            ImGui::Text("  SIM:    %6.1f Hz", s_hz);
        } else {
            ImGui::TextDisabled("  SIM:       --- Hz   (sim hook not yet firing)");
        }
        const float a   = mtr::interp::current_alpha();
        const bool  cut = mtr::interp::is_cut_detected();
        ImGui::Text("  ALPHA:    %5.3f%s", a, cut ? "  (CUT this frame)" : "");
        ImGui::TextDisabled("  Snapshots: %llu  cuts: %llu  (interp ready: %s)",
                            static_cast<unsigned long long>(mtr::interp::snapshots_taken()),
                            static_cast<unsigned long long>(mtr::interp::cuts_detected()),
                            mtr::interp::has_two_snapshots() ? "yes" : "no");

        ImGui::TextDisabled("  Skipped:  sim=%llu  pathcam=%llu  overlay=%llu  uv=%llu",
                            static_cast<unsigned long long>(mtr::sim_decouple::sim_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::pathcam_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::overlay_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::uv_skipped()));
        ImGui::TextDisabled("  Alt-pump:  wave=%llu  chain=%llu  managed=%llu (M1.7)",
                            static_cast<unsigned long long>(mtr::sim_decouple::wave_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::chain_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::managed_skipped()));
        ImGui::TextDisabled("  M1.8:  timer=%llu  post_render=%llu  alt_subsys=%llu  alt_audio=%llu",
                            static_cast<unsigned long long>(mtr::sim_decouple::timer_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::post_render_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::alt_subsys_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::alt_audio_skipped()));
        ImGui::TextDisabled("  Frame-dt corrections: %llu (M1.6)",
                            static_cast<unsigned long long>(mtr::sim_decouple::dt_corrections_applied()));

        ImGui::Spacing();
        bool vi = mtr::interp::view_interp_enabled();
        if (ImGui::Checkbox("Enable view interp (M3.1) — slerp+lerp per render frame", &vi)) {
            mtr::interp::set_view_interp_enabled(vi);
        }
        ImGui::SameLine();
        info_pill(
            "When on AND throttle is engaged AND no cut is detected, the "
            "render frame's view + world matrices are smoothly interpolated "
            "between the last two sim ticks via slerp(rotation) + "
            "lerp(translation). Net effect: 240 Hz fluidity for camera "
            "while sim runs at the chosen target Hz. Inherent ~1 sim "
            "window of latency vs uncapped render — D4 lerp-only design.");
        ImGui::TextDisabled("  Frames written: %llu",
                            static_cast<unsigned long long>(mtr::interp::view_interp_writes()));

        ImGui::Spacing();
        bool pi = mtr::interp::player_interp_enabled();
        if (ImGui::Checkbox("Enable player transform interp (M4) — Wilbur pos+rot", &pi)) {
            mtr::interp::set_player_interp_enabled(pi);
        }
        ImGui::SameLine();
        info_pill(
            "When on AND throttle is engaged AND no player teleport on this "
            "sim window, Wilbur's position (entity+0x58) and rotation 3x3 "
            "(entity+0x70) are interpolated between the last two sim ticks "
            "via lerp+slerp each render frame. Save-write-restore fence "
            "around the player entity keeps the next sim's reads clean.");
        ImGui::TextDisabled("  Frames written: %llu  teleports: %llu",
                            static_cast<unsigned long long>(mtr::interp::player_interp_writes()),
                            static_cast<unsigned long long>(mtr::interp::player_teleports_detected()));

        ImGui::Spacing();
        bool ni = mtr::interp::npc_interp_enabled();
        if (ImGui::Checkbox("Enable NPC transform interp (M5) — every visible entity", &ni)) {
            mtr::interp::set_npc_interp_enabled(ni);
        }
        ImGui::SameLine();
        info_pill(
            "Walks the engine's per-frame transform list (dword_724DE4) and "
            "applies the same lerp+slerp+fence pattern as M4 to every entity "
            "in it (capped at 64 slots). Skips the player (M4 covers it) and "
            "any node the engine flagged \"skip transform\". Stale slots age "
            "out after 6 sim ticks of absence. Per-entity write protected "
            "by SEH against entities the engine freed mid-frame.");
        ImGui::TextDisabled("  Slots active: %llu  writes: %llu  teleports: %llu",
                            static_cast<unsigned long long>(mtr::interp::npc_active_slots()),
                            static_cast<unsigned long long>(mtr::interp::npc_interp_writes()),
                            static_cast<unsigned long long>(mtr::interp::npc_teleports_detected()));

        float t_tp = mtr::interp::player_teleport_threshold();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Teleport threshold", &t_tp, 1.0f, 100.0f, "%.1f units")) {
            mtr::interp::set_player_teleport_threshold(t_tp);
        }
        ImGui::SameLine();
        info_pill(
            "Per-sim-tick player |translation delta| above this triggers a "
            "snap (no interp this window). Defaults to 10 units — covers "
            "respawn-after-death and scripted teleports without false-"
            "positives during sprint/slide.");

        ImGui::Spacing();
        bool as = mtr::interp::aim_snap_enabled();
        if (ImGui::Checkbox("Aim-snap (M3.2) — hold a key to clamp alpha=1.0", &as)) {
            mtr::interp::set_aim_snap_enabled(as);
        }
        ImGui::SameLine();
        info_pill(
            "While the bound key is held, all interp falls back to "
            "freshest-sim (alpha=1.0). Eliminates the ~1 sim-window "
            "latency during aim. Default: VK_RBUTTON (right mouse). "
            "Substitutes for the engine-side aim-mode flag, which lives "
            "in SecuROM-thunked script-callback territory.");
        int vk = mtr::interp::aim_snap_vk();
        const char* vk_name =
            (vk == 0x01) ? "LMB" : (vk == 0x02) ? "RMB" : (vk == 0x04) ? "MMB" :
            (vk == 0x10) ? "Shift" : (vk == 0x11) ? "Ctrl" : (vk == 0x12) ? "Alt" :
            (vk == 0x46) ? "F" : (vk == 0x20) ? "Space" : "(custom)";
        ImGui::TextDisabled("  Bound key: 0x%02X %s   active now: %s", vk, vk_name,
                            mtr::interp::aim_snap_active() ? "YES" : "no");
        if (ImGui::SmallButton("RMB##aimkey"))    mtr::interp::set_aim_snap_vk(0x02);
        ImGui::SameLine();
        if (ImGui::SmallButton("LMB##aimkey"))    mtr::interp::set_aim_snap_vk(0x01);
        ImGui::SameLine();
        if (ImGui::SmallButton("MMB##aimkey"))    mtr::interp::set_aim_snap_vk(0x04);
        ImGui::SameLine();
        if (ImGui::SmallButton("Shift##aimkey"))  mtr::interp::set_aim_snap_vk(0x10);
        ImGui::SameLine();
        if (ImGui::SmallButton("F##aimkey"))      mtr::interp::set_aim_snap_vk(0x46);

        ImGui::Spacing();
        ImGui::TextDisabled("Cut-detect thresholds (skip interp on big sim deltas):");
        float t_thr = mtr::interp::cut_translation_threshold();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Translation##cut", &t_thr, 0.5f, 50.0f, "%.2f units")) {
            mtr::interp::set_cut_translation_threshold(t_thr);
        }
        ImGui::SameLine();
        info_pill(
            "Per-sim-tick |translation| above this triggers a cut. Defaults "
            "to 5 world units. Lower = stricter (more cuts caught). Higher = "
            "looser (fewer false positives during fast travel).");
        float r_thr = mtr::interp::cut_rotation_threshold_deg();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Rotation##cut",  &r_thr, 5.0f, 180.0f, "%.0f deg")) {
            mtr::interp::set_cut_rotation_threshold_deg(r_thr);
        }
        ImGui::SameLine();
        info_pill(
            "Per-sim-tick view-axis rotation above this triggers a cut. "
            "Defaults to 30 deg. Counts angle between consecutive sim ticks' "
            "forward axes; 30 deg matches a 0.5s 60-deg/s spin without "
            "tripping.");

        ImGui::Spacing();
        bool autodis = mtr::sim_decouple::auto_disable_in_minigame();
        if (ImGui::Checkbox("Auto-disable in mini-games", &autodis)) {
            mtr::sim_decouple::set_auto_disable_in_minigame(autodis);
        }
        ImGui::SameLine();
        info_pill(
            "When ON (default), throttle force-OFFs while a mini-game screen "
            "is on the stack — DigDug / MiniHamster / ChargeBall — so their "
            "alternate sim paths run at native speed. Detection is screen-"
            "name based (no vtable reads). Disable only if the auto-detect "
            "false-positives or you want to push past the supported scope.");

        const auto pm = mtr::sim_decouple::current_player_mode();
        const bool   mg = mtr::sim_decouple::minigame_detected();
        const char*  pml = mtr::sim_decouple::player_mode_label(pm);
        ImGui::Text("  Player mode: %s%s", pml,
                    (mg && autodis) ? "  (throttle vetoed)" : "");

        ImGui::Spacing();
        bool log_on = mtr::sim_decouple::detailed_log_enabled();
        if (ImGui::Checkbox("Detailed log (mtr-asi-decouple.log)", &log_on)) {
            mtr::sim_decouple::set_detailed_log_enabled(log_on);
        }
        ImGui::SameLine();
        info_pill(
            "Writes per-tick + per-snapshot diagnostics to "
            "Game/mtr-asi-decouple.log. Off by default — overhead is one "
            "atomic load when off. Useful for triaging hidden 0.003 sites "
            "and validating throttle behaviour at high render rates.");

        section_end(open);
    }

    if (bool open = section_begin("Status")) {
        ImGui::Text("Device:      %p", g_device.load());
        ImGui::Text("Window:      %p", g_hwnd.load());
        ImGui::Text("Backbuffer:  %d x %d",
                    mtr::d3d9hook::last_pp_width(), mtr::d3d9hook::last_pp_height());
        if (HWND h = g_hwnd.load()) {
            RECT r{};
            if (GetClientRect(h, &r)) {
                ImGui::Text("Client area: %ld x %ld", r.right - r.left, r.bottom - r.top);
            }
        }
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Hotkeys: Insert (menu)  F2 (console)  F3 (FreeCam)  F12 (screenshot)");
        section_end(open);
    }
}

void draw_menu() {
    // Default to ~37% width × ~95% height of the viewport so the menu fits
    // most native resolutions without scrolling, matches the user's
    // preferred mockup size, and scales sensibly across 1080p / 1440p / 4K.
    // Position pinned to a small inset from the top-left.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 def_size(vp->WorkSize.x * 0.37f, vp->WorkSize.y * 0.95f);
    ImGui::SetNextWindowSize(def_size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 24.0f, vp->WorkPos.y + 24.0f),
                            ImGuiCond_FirstUseEver);

    if (ImGui::Begin("mtr-asi v" MTR_ASI_VERSION, nullptr, ImGuiWindowFlags_NoCollapse)) {
        // Always-visible status strip so the user can see active overrides at
        // a glance, regardless of which tab is open. Coloured so on/off are
        // easy to scan.
        auto draw_flag = [](const char* label, bool on) {
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::TextColored(on ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f)
                                  : ImVec4(0.55f, 0.55f, 0.6f, 1.0f),
                               on ? "ON " : "off");
        };
        draw_flag("FreeCam:",   mtr::freecam::active());     ImGui::SameLine();
        ImGui::TextDisabled("|");                            ImGui::SameLine();
        draw_flag("FOV:",       mtr::fov::has_override());   ImGui::SameLine();
        ImGui::TextDisabled("|");                            ImGui::SameLine();
        draw_flag("DrawDist:",  mtr::draw_dist::has_override());
        ImGui::Separator();

        if (ImGui::BeginTabBar("##mtr_tabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Camera"))  { tab_camera();  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Display")) { tab_display(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("World"))   { tab_world();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Tools"))   { tab_tools();   ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace

void poll_input_to_imgui() {
    // Game uses DirectInput8 in exclusive-foreground mode -> WM_KEY/WM_*BUTTON
    // are eaten before reaching our WndProc subclass. Bypass with GetAsyncKeyState +
    // GetCursorPos polling, feeding events directly to ImGui's IO each frame.
    ImGuiIO& io = ImGui::GetIO();

    HWND hwnd = g_hwnd.load();
    POINT pt;
    if (hwnd && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
        io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
    }

    static bool prev_lmb = false, prev_rmb = false, prev_mmb = false;
    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool mmb = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    if (lmb != prev_lmb) { io.AddMouseButtonEvent(0, lmb); prev_lmb = lmb; }
    if (rmb != prev_rmb) { io.AddMouseButtonEvent(1, rmb); prev_rmb = rmb; }
    if (mmb != prev_mmb) { io.AddMouseButtonEvent(2, mmb); prev_mmb = mmb; }

    // Toggle key (Insert) — direct polling, edge-triggered.
    static bool prev_ins = false;
    bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (ins && !prev_ins) {
        g_visible = !g_visible.load();
        mtr::log::info("menu: visible=%d (Insert)", g_visible.load() ? 1 : 0);
    }
    prev_ins = ins;

    // FreeCam toggle (F3) — edge-triggered.
    static bool prev_f3 = false;
    bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    if (f3 && !prev_f3) {
        mtr::freecam::set_active(!mtr::freecam::active());
    }
    prev_f3 = f3;

    // MMB while freecam active = teleport request. Edge-triggered to avoid
    // repeating while held. Reuse the same `mmb` value polled above for ImGui.
    static bool prev_mmb_freecam = false;
    if (mmb && !prev_mmb_freecam && mtr::freecam::active()) {
        mtr::freecam::request_teleport_to_camera();
    }
    prev_mmb_freecam = mmb;

    // Tell freecam whether ImGui is currently grabbing the cursor; freecam's
    // mouse-look only runs when no UI is open.
    const bool ui_open = g_visible.load() || mtr::console::is_visible();
    mtr::freecam::set_ui_visible(ui_open);


    // Screenshot key (F12) — edge-triggered.
    static bool prev_f12 = false;
    bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12 && !prev_f12) {
        mtr::screenshot::request();
    }
    prev_f12 = f12;

    // Wheel — read accumulated raw state via GetAsyncKeyState equivalent
    // (kept simple; wheel events come via WM_MOUSEWHEEL in WndProc subclass).
}

void on_end_scene(IDirect3DDevice9* dev) {
    if (!dev) return;
    if (!g_imgui_ready.load()) {
        init_imgui(dev);
        if (!g_imgui_ready.load()) return;
    }

    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();

    poll_input_to_imgui();
    mtr::console::poll_hotkey();
    mtr::freecam::tick();

    // ImGui draws its own cursor when an INTERACTIVE mtr-asi window is up.
    // FPS overlay is non-interactive (NoInputs), so it doesn't force the
    // cursor on -- but it does need ImGui::Render() to run, hence the
    // separate `any_draw` flag below.
    const bool any_interactive = g_visible.load() || mtr::console::is_visible();
    const bool any_draw        = any_interactive || g_fps_overlay.load();
    io.MouseDrawCursor = any_interactive;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Drain the LL-hook mouse queue captured while the UI was visible.
    // DirectInput-exclusive on the mouse eats WM_LBUTTONDOWN /
    // WM_MOUSEMOVE / WM_MOUSEWHEEL during gameplay before they reach
    // any WndProc — including ImGui's subclass — which is why pick mode
    // and the wheel-driven scrollbar both worked in the main menu (no
    // exclusive grab) but failed during gameplay HUD.
    //
    // The LL hook now captures every mouse event when an mtr-asi UI is
    // visible and returns 1 to swallow, so the queue is the SINGLE
    // source of truth (no double-fire vs the WndProc subclass; the
    // subclass simply never sees these events while UI is visible).
    // Here on the render thread we replay them through io.AddXxxEvent.
    {
        mtr::input_hook::MouseQueueEvent evs[256];
        int n = mtr::input_hook::drain_mouse_events(evs, 256);
        if (n > 0) {
            HWND hwnd = g_hwnd.load();
            ImGuiIO& io2 = ImGui::GetIO();
            for (int i = 0; i < n; ++i) {
                const auto& e = evs[i];
                // Convert screen-space → client-space pixel coords. ImGui's
                // mouse-pos space matches the window client area, which is
                // also what DisplaySize reflects.
                POINT p{e.screen_x, e.screen_y};
                if (hwnd) ScreenToClient(hwnd, &p);
                if (e.kind == mtr::input_hook::MouseQueueEvent::Pos) {
                    io2.AddMousePosEvent(static_cast<float>(p.x),
                                         static_cast<float>(p.y));
                } else if (e.kind == mtr::input_hook::MouseQueueEvent::Button) {
                    // ImGui needs the position to be current at the time
                    // of the click — emit a pos event first if the queue
                    // didn't include a separate WM_MOUSEMOVE for this
                    // exact tick (button events in MSLLHOOKSTRUCT carry
                    // their own pt, so we always trust it).
                    io2.AddMousePosEvent(static_cast<float>(p.x),
                                         static_cast<float>(p.y));
                    io2.AddMouseButtonEvent(static_cast<int>(e.button),
                                            e.down);
                } else if (e.kind == mtr::input_hook::MouseQueueEvent::Wheel) {
                    io2.AddMousePosEvent(static_cast<float>(p.x),
                                         static_cast<float>(p.y));
                    io2.AddMouseWheelEvent(0.0f,
                        static_cast<float>(e.wheel_delta) / 120.0f);
                }
            }
        }
    }
    // Back-compat path for the legacy wheel-only drain (drains the
    // separate accumulator that the previous freecam-only wheel hook
    // wrote to). Stays here so any pre-Phase-B wheel events still
    // reach ImGui — the new path doesn't write into this accumulator.
    if (const int wheel_120 = mtr::input_hook::drain_wheel_delta(); wheel_120 != 0) {
        ImGui::GetIO().AddMouseWheelEvent(0.0f, static_cast<float>(wheel_120) / 120.0f);
    }

    // Cooperative-level swap on UI visibility transitions. The dinput hook
    // downgrades the mouse device from DISCL_EXCLUSIVE to DISCL_NONEXCLUSIVE
    // while UI is visible; the OS cursor unpins immediately and
    // GetCursorPos / WM_MOUSEMOVE / the win32 backend's input plumbing all
    // start tracking the live cursor again. Hidden = restore original.
    static bool s_ui_prev = false;
    if (any_interactive != s_ui_prev) {
        mtr::dinput_hook::set_ui_visible(any_interactive);
        s_ui_prev = any_interactive;
    }
    // Per-frame re-assert while UI is visible — the game (or its driver
    // layer) may try to fight the override back; re-applying every frame
    // makes sure the cursor stays free for the duration of the menu.
    if (any_interactive) {
        mtr::dinput_hook::tick_force_unpin();
    }

    ImGui::NewFrame();

    if (g_visible.load()) {
        draw_menu();
    }
    // Coalesce ini writes: every UI site that mutates state calls
    // request_save() (cheap), and we do the actual write at most every
    // kSaveDebounceMs (~250ms). End-of-drag stutter eliminated.
    mtr::ui_aspect_rules::flush_pending_save();

    // Engine↔screen overlay + spatial mouse handling. Single owner of
    // every screen-space interaction (click-to-pick, gizmo drag, on-screen
    // visual indicators). All consumers share one engine→screen remap
    // sourced from sprite_matrix::current_factors() — the canonical
    // resolver that mirrors hk_MatrixSetXform{A,B}'s activation order —
    // so overlays never drift from the actual rendered geometry.
    //
    // Pipeline (in order — earlier consumers can short-circuit later
    // ones via the click_consumed flag):
    //   1. Compute factors, remap helpers, inverse remap.
    //   2. Hilite overlay (per-row hold-to-highlight, no input).
    //   3. Selection outline + translate gizmo handle (visual + click
    //      target). Drag state machine processes mouse here. If gizmo
    //      claimed the click, sets click_consumed=true.
    //   4. Click-to-pick: if pick_mode is on AND click wasn't claimed
    //      AND click was outside any ImGui window, set selection.
    {
        const auto F    = mtr::sprite_matrix::current_factors();
        const ImVec2 sz = ImGui::GetIO().DisplaySize;
        ImDrawList* dl  = ImGui::GetForegroundDrawList();
        const bool sz_ok = (sz.x > 0.0f && sz.y > 0.0f);
        const bool F_ok  = (fabsf(F.Fx) > 1e-6f && fabsf(F.Fy) > 1e-6f);

        auto remap = [&](float x, float y) -> ImVec2 {
            const float ssx = (x - 0.5f) * F.Fx + 0.5f + F.dx * 0.5f;
            const float ssy = (y - 0.5f) * F.Fy + 0.5f - F.dy * 0.5f;
            return ImVec2(ssx * sz.x, ssy * sz.y);
        };
        // Inverse remap: pixels → engine [0,1]. Used by pick to find
        // engine coords under the cursor, and by the translate gizmo to
        // turn a pixel-delta into the engine-delta we write to offset_x/y.
        auto pixel_to_engine = [&](float px, float py, float* ex, float* ey) {
            *ex = ((px / sz.x) - 0.5f - F.dx * 0.5f) / F.Fx + 0.5f;
            *ey = ((py / sz.y) - 0.5f + F.dy * 0.5f) / F.Fy + 0.5f;
        };

        // ---- Hilite overlay (held button on a per-element row) ----
        mtr::sprite_xform::HighlightBox boxes[64];
        const int hn = mtr::sprite_xform::snapshot_highlight_boxes(boxes, 64);
        for (int i = 0; i < hn; ++i) {
            const auto& b = boxes[i];
            const ImVec2 a = remap(b.x0, b.y0);
            const ImVec2 c = remap(b.x1, b.y1);
            const uint32_t h = b.state_key * 0x9E3779B1u;
            const uint8_t r  = static_cast<uint8_t>((h >>  8) | 0xC0);
            const uint8_t g  = static_cast<uint8_t>((h >> 16) | 0xC0);
            const uint8_t b8 = static_cast<uint8_t>((h >>  0) | 0xC0);
            dl->AddRectFilled(a, c, IM_COL32(r, g, b8, 70));
            dl->AddRect      (a, c, IM_COL32(r, g, b8, 255), 0.0f, 0, 2.5f);
        }

        // ---- Selection outline + translate / scale gizmos ----------
        // Persistent drag state. We don't use ImGui's InvisibleButton
        // for the handles because they float in screen space outside
        // any ImGui window; a manual state machine keyed off IsMouseDown
        // / IsMouseClicked is simpler and gives us full control over
        // click-consumption.
        //
        // handle = -1 (translate, center circle), 0..3 (scale corners,
        // matching pts[] index of the captured quad). Drag captures the
        // anchor centroid + handle pixel position so subsequent frames
        // measure relative to a fixed reference; if the engine re-emits
        // the entry at a slightly shifted position, the gizmo stays
        // tied to where the user grabbed it.
        struct GizmoDrag {
            bool  active = false;
            int   slot_idx = -1;
            int   handle = -1;
            float anchor_mouse_x = 0, anchor_mouse_y = 0;
            float anchor_offset_x = 0, anchor_offset_y = 0;
            float anchor_scale_x = 1, anchor_scale_y = 1;
            float anchor_center_px = 0, anchor_center_py = 0;
            float anchor_handle_px = 0, anchor_handle_py = 0;
        };
        static GizmoDrag g_drag;

        bool click_consumed = false;

        int sel = mtr::sprite_picking::selected();  // mutable: auto-Specialize swaps it
        if (sel >= 0 && sz_ok && F_ok) {
            // Find the latest quad for the selected slot (animated UI may
            // re-emit the same slot multiple times per frame; pick the
            // last one — topmost in render order).
            const int qn = mtr::sprite_picking::quad_count();
            int      best_quad = -1;
            int      best_order = -1;
            float    best_xy[8] = {0};
            for (int i = 0; i < qn; ++i) {
                int slot_idx = -1, order = 0;
                uint32_t state_key = 0;
                float xy[8] = {0};
                mtr::sprite_picking::quad_at(i, &slot_idx, &state_key, xy, &order);
                if (slot_idx != sel) continue;
                if (order > best_order) {
                    best_order = order;
                    best_quad  = i;
                    for (int k = 0; k < 8; ++k) best_xy[k] = xy[k];
                }
            }

            if (best_quad >= 0) {
                ImVec2 pts[4] = {
                    remap(best_xy[0], best_xy[1]),
                    remap(best_xy[2], best_xy[3]),
                    remap(best_xy[4], best_xy[5]),
                    remap(best_xy[6], best_xy[7]),
                };
                dl->AddConvexPolyFilled(pts, 4, IM_COL32(80, 220, 250, 50));
                dl->AddPolyline(pts, 4, IM_COL32(80, 220, 250, 255),
                                ImDrawFlags_Closed, 3.0f);

                ImVec2 ctr{0,0};
                for (int k = 0; k < 4; ++k) { ctr.x += pts[k].x; ctr.y += pts[k].y; }
                ctr.x *= 0.25f; ctr.y *= 0.25f;

                const ImVec2 mp = ImGui::GetIO().MousePos;
                const float translate_pick_r = 14.0f;
                const float corner_pick_half = 9.0f;

                // Hit-testing: priority is translate-center over corners,
                // since the centroid sometimes falls near a degenerate
                // corner (1-pixel sprites). Corners get tested only when
                // the cursor isn't already on the center.
                int   hover_handle = -2;     // -2 = none, -1 = center, 0..3 = corner
                {
                    const float dxh = mp.x - ctr.x, dyh = mp.y - ctr.y;
                    if (dxh*dxh + dyh*dyh <= translate_pick_r * translate_pick_r) {
                        hover_handle = -1;
                    } else {
                        for (int k = 0; k < 4; ++k) {
                            if (fabsf(mp.x - pts[k].x) <= corner_pick_half
                             && fabsf(mp.y - pts[k].y) <= corner_pick_half) {
                                hover_handle = k;
                                break;
                            }
                        }
                    }
                }

                // ---- Drag start ----
                if (!g_drag.active && hover_handle != -2
                    && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && !io.WantCaptureMouse) {
                    // Auto-Specialize: if the user is about to edit a
                    // wildcard slot, fork off a concrete variant pinned
                    // to the picked entry's exact UV/screen/quadrant
                    // first. This is the no-crutch path for "edit only
                    // THIS sprite, not every variant sharing the asset" —
                    // editing the wildcard would silently mutate sibling
                    // variants. specialize_slot is a no-op (returns the
                    // same idx) when the slot is already concrete.
                    int target = mtr::sprite_xform::specialize_slot(sel);
                    if (target < 0) target = sel;  // no last_concrete; edit in place
                    if (target != sel) {
                        sel = target;
                        mtr::sprite_picking::set_selected(target);
                        mtr::ui_aspect_rules::request_save();
                    }
                    g_drag.active   = true;
                    g_drag.slot_idx = sel;
                    g_drag.handle   = hover_handle;
                    g_drag.anchor_mouse_x = mp.x;
                    g_drag.anchor_mouse_y = mp.y;
                    auto t = mtr::sprite_xform::get_transform_at(sel);
                    g_drag.anchor_offset_x = t.offset_x;
                    g_drag.anchor_offset_y = t.offset_y;
                    g_drag.anchor_scale_x  = t.scale_x;
                    g_drag.anchor_scale_y  = t.scale_y;
                    g_drag.anchor_center_px = ctr.x;
                    g_drag.anchor_center_py = ctr.y;
                    if (hover_handle >= 0 && hover_handle < 4) {
                        g_drag.anchor_handle_px = pts[hover_handle].x;
                        g_drag.anchor_handle_py = pts[hover_handle].y;
                    }
                    click_consumed = true;
                }

                // ---- Drag update ----
                if (g_drag.active && g_drag.slot_idx == sel
                    && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    auto t = mtr::sprite_xform::get_transform_at(sel);

                    // Snapshot pre-update values so we can compute the
                    // delta to broadcast to group peers when "Drag
                    // group" is on. We compute the delta from the
                    // selected slot's previous frame value, not from
                    // the anchor — ensures group peers get exactly the
                    // same incremental movement, even when the picked
                    // slot's transform is clamped.
                    const float prev_ox = t.offset_x;
                    const float prev_oy = t.offset_y;
                    const float prev_sx = t.scale_x;
                    const float prev_sy = t.scale_y;

                    if (g_drag.handle == -1) {
                        // Translate: pixel delta → engine delta on offset.
                        const float pdx = mp.x - g_drag.anchor_mouse_x;
                        const float pdy = mp.y - g_drag.anchor_mouse_y;
                        const float edx = pdx / (F.Fx * sz.x);
                        const float edy = pdy / (F.Fy * sz.y);
                        t.offset_x = g_drag.anchor_offset_x + edx;
                        t.offset_y = g_drag.anchor_offset_y + edy;
                    } else {
                        // Scale: ratio of (current corner→center) over
                        // (anchor corner→center) per axis. Ctrl snaps to
                        // 0.05 increments. Shift uniformizes (use
                        // larger absolute axis ratio for both).
                        const float ax = g_drag.anchor_handle_px - g_drag.anchor_center_px;
                        const float ay = g_drag.anchor_handle_py - g_drag.anchor_center_py;
                        const float cx = mp.x - g_drag.anchor_center_px;
                        const float cy = mp.y - g_drag.anchor_center_py;
                        // Avoid divide-by-zero: corners with a 0-axis
                        // contribution can't influence that axis (e.g. a
                        // perfectly-thin sprite). Fall back to identity
                        // ratio there.
                        float ratio_x = (fabsf(ax) > 1e-3f) ? (cx / ax) : 1.0f;
                        float ratio_y = (fabsf(ay) > 1e-3f) ? (cy / ay) : 1.0f;
                        const ImGuiIO& io2 = ImGui::GetIO();
                        if (io2.KeyShift) {
                            // Uniform: pick whichever axis moved further
                            // from 1.0 in absolute terms. Sign retained.
                            const float ax_dev = fabsf(ratio_x - 1.0f);
                            const float ay_dev = fabsf(ratio_y - 1.0f);
                            if (ax_dev >= ay_dev) ratio_y = ratio_x;
                            else                  ratio_x = ratio_y;
                        }
                        float ns_x = g_drag.anchor_scale_x * ratio_x;
                        float ns_y = g_drag.anchor_scale_y * ratio_y;
                        if (io2.KeyCtrl) {
                            auto snap = [](float v) -> float {
                                // floorf(v + 0.5) is the same as round-half-away-
                                // from-zero for non-negative; we already clamp
                                // ns_x/y > 0.05 below, so this is safe.
                                return floorf(v * 20.0f + 0.5f) / 20.0f;
                            };
                            ns_x = snap(ns_x);
                            ns_y = snap(ns_y);
                        }
                        // Clamp positive — flipping past 0 produces a
                        // degenerate / inverted sprite, which is almost
                        // never the user's intent. The DragFloat slider
                        // already enforces 0.05..5; mirror that here.
                        if (ns_x < 0.05f) ns_x = 0.05f; if (ns_x > 5.0f) ns_x = 5.0f;
                        if (ns_y < 0.05f) ns_y = 0.05f; if (ns_y > 5.0f) ns_y = 5.0f;
                        t.scale_x = ns_x;
                        t.scale_y = ns_y;
                    }
                    mtr::sprite_xform::set_transform_at(sel,
                        t.offset_x, t.offset_y,
                        t.scale_x,  t.scale_y, t.hidden);

                    // Broadcast incremental delta to group peers. Using
                    // (new - prev) for offsets and (new / prev) for
                    // scale ensures peers integrate the same per-frame
                    // motion as the picked slot, with no compounding.
                    if (mtr::sprite_xform::drag_group()) {
                        char grp[32] = {0};
                        if (mtr::sprite_xform::get_group_at_buf(
                                sel, grp, sizeof(grp))) {
                            if (g_drag.handle == -1) {
                                const float dox = t.offset_x - prev_ox;
                                const float doy = t.offset_y - prev_oy;
                                mtr::sprite_xform::apply_group_translate_delta(
                                    grp, sel, dox, doy);
                            } else {
                                const float fsx = (prev_sx > 1e-6f) ? (t.scale_x / prev_sx) : 1.0f;
                                const float fsy = (prev_sy > 1e-6f) ? (t.scale_y / prev_sy) : 1.0f;
                                mtr::sprite_xform::apply_group_scale_factor(
                                    grp, sel, fsx, fsy);
                            }
                        }
                    }
                }

                // ---- Drag end ----
                if (g_drag.active
                    && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    g_drag.active = false;
                    g_drag.slot_idx = -1;
                    g_drag.handle = -1;
                    mtr::ui_aspect_rules::request_save();
                }

                // ---- Right-click on a handle resets the relevant axis ----
                if (!g_drag.active && hover_handle != -2
                    && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                    && !io.WantCaptureMouse) {
                    // Same auto-Specialize logic as left-click drag start —
                    // resetting on a wildcard slot would un-edit every
                    // variant sharing the asset, which is the textbook
                    // hidden side-effect we're trying to avoid. Fork the
                    // variant first, then reset the axis only on it.
                    int target = mtr::sprite_xform::specialize_slot(sel);
                    if (target < 0) target = sel;
                    if (target != sel) {
                        sel = target;
                        mtr::sprite_picking::set_selected(target);
                    }
                    auto t = mtr::sprite_xform::get_transform_at(sel);
                    const float prev_ox = t.offset_x, prev_oy = t.offset_y;
                    const float prev_sx = t.scale_x,  prev_sy = t.scale_y;
                    if (hover_handle == -1) {
                        t.offset_x = 0.0f;
                        t.offset_y = 0.0f;
                    } else {
                        t.scale_x = 1.0f;
                        t.scale_y = 1.0f;
                    }
                    mtr::sprite_xform::set_transform_at(sel,
                        t.offset_x, t.offset_y,
                        t.scale_x,  t.scale_y, t.hidden);
                    if (mtr::sprite_xform::drag_group()) {
                        char grp[32] = {0};
                        if (mtr::sprite_xform::get_group_at_buf(
                                sel, grp, sizeof(grp))) {
                            if (hover_handle == -1) {
                                mtr::sprite_xform::apply_group_translate_delta(
                                    grp, sel,
                                    t.offset_x - prev_ox,
                                    t.offset_y - prev_oy);
                            } else {
                                const float fsx = (prev_sx > 1e-6f) ? (t.scale_x / prev_sx) : 1.0f;
                                const float fsy = (prev_sy > 1e-6f) ? (t.scale_y / prev_sy) : 1.0f;
                                mtr::sprite_xform::apply_group_scale_factor(
                                    grp, sel, fsx, fsy);
                            }
                        }
                    }
                    mtr::ui_aspect_rules::request_save();
                    click_consumed = true;
                }

                // ---- Visuals ----
                const float trans_r = 9.0f;
                const bool  trans_hot = (hover_handle == -1)
                                     || (g_drag.active && g_drag.handle == -1);
                const ImU32 trans_col = trans_hot ? IM_COL32(120, 240, 255, 255)
                                                  : IM_COL32(80,  220, 250, 255);
                dl->AddCircleFilled(ctr, trans_r, trans_col);
                dl->AddCircle      (ctr, trans_r, IM_COL32(0, 0, 0, 220), 0, 2.0f);
                dl->AddLine(ImVec2(ctr.x - trans_r + 2.0f, ctr.y),
                            ImVec2(ctr.x + trans_r - 2.0f, ctr.y),
                            IM_COL32(0, 0, 0, 220), 1.5f);
                dl->AddLine(ImVec2(ctr.x, ctr.y - trans_r + 2.0f),
                            ImVec2(ctr.x, ctr.y + trans_r - 2.0f),
                            IM_COL32(0, 0, 0, 220), 1.5f);

                // Corner handles: small filled squares with a darker
                // border. Match the selection cyan; brighter when hot.
                const float cs = 6.0f;
                for (int k = 0; k < 4; ++k) {
                    const bool hot = (hover_handle == k)
                                  || (g_drag.active && g_drag.handle == k);
                    const ImU32 col = hot ? IM_COL32(120, 240, 255, 255)
                                          : IM_COL32(80,  220, 250, 255);
                    dl->AddRectFilled(
                        ImVec2(pts[k].x - cs, pts[k].y - cs),
                        ImVec2(pts[k].x + cs, pts[k].y + cs),
                        col);
                    dl->AddRect(
                        ImVec2(pts[k].x - cs, pts[k].y - cs),
                        ImVec2(pts[k].x + cs, pts[k].y + cs),
                        IM_COL32(0, 0, 0, 220), 0.0f, 0, 2.0f);
                }

                // ---- Live drag tooltip with delta values --------------
                if (g_drag.active && g_drag.slot_idx == sel) {
                    auto t = mtr::sprite_xform::get_transform_at(sel);
                    char tip[160];
                    if (g_drag.handle == -1) {
                        std::snprintf(tip, sizeof(tip),
                            "translate  ox %.4f  oy %.4f\n"
                            "(\xCE\x94 %+.4f, %+.4f)",
                            t.offset_x, t.offset_y,
                            t.offset_x - g_drag.anchor_offset_x,
                            t.offset_y - g_drag.anchor_offset_y);
                    } else {
                        std::snprintf(tip, sizeof(tip),
                            "scale  sx %.3f  sy %.3f\n"
                            "shift = uniform / ctrl = snap 0.05",
                            t.scale_x, t.scale_y);
                    }
                    dl->AddRectFilled(
                        ImVec2(mp.x + 14.0f, mp.y + 14.0f),
                        ImVec2(mp.x + 14.0f + 230.0f, mp.y + 14.0f + 36.0f),
                        IM_COL32(20, 20, 20, 220), 4.0f);
                    dl->AddText(ImVec2(mp.x + 20.0f, mp.y + 18.0f),
                                IM_COL32(220, 240, 250, 255), tip);
                }
            }
        } else if (g_drag.active) {
            // Selection cleared mid-drag (e.g. Esc) — drop the drag.
            g_drag.active = false;
            g_drag.slot_idx = -1;
            g_drag.handle = -1;
        }

        // ---- Click-to-pick (only if no gizmo claimed the click) -----
        // Repeated clicks at the same screen position cycle through the
        // stacked layers under the cursor — atlas UI typically draws
        // text on top of button frames on top of backgrounds, so the
        // first click always returns the topmost glyph. Click again at
        // the same spot within the cycle window to drop one layer.
        // Move the cursor by more than a few pixels OR wait past the
        // window → cycle resets to topmost.
        static ImVec2 s_pick_anchor{-1e6f, -1e6f};
        static double s_pick_anchor_time = -1.0;
        static int    s_pick_layer = 0;
        if (!click_consumed
            && mtr::sprite_picking::pick_mode()
            && sz_ok && F_ok
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !io.WantCaptureMouse) {
            const ImVec2 mp = io.MousePos;
            const double now = ImGui::GetTime();
            const float ddx = mp.x - s_pick_anchor.x;
            const float ddy = mp.y - s_pick_anchor.y;
            const bool same_spot =
                (ddx * ddx + ddy * ddy) < 25.0f       // 5 px radius
             && (now - s_pick_anchor_time) < 0.8;     // 0.8 s window
            if (same_spot) ++s_pick_layer;
            else           s_pick_layer = 0;
            s_pick_anchor      = mp;
            s_pick_anchor_time = now;

            float ex = 0, ey = 0;
            pixel_to_engine(mp.x, mp.y, &ex, &ey);
            int slot = mtr::sprite_picking::pick_engine_at(ex, ey, s_pick_layer);
            if (slot < 0 && s_pick_layer > 0) {
                // Past the bottom layer — wrap to topmost.
                s_pick_layer = 0;
                slot = mtr::sprite_picking::pick_engine_at(ex, ey, 0);
            }
            if (slot >= 0) {
                mtr::sprite_picking::set_selected(slot);
            }
        }

        // ---- Pick-mode visual hint: faint border around viewport so
        // the user knows pick mode is armed even when no sprite is
        // hovered. Doesn't render when a drag is in progress (avoids
        // visual noise during the actual interaction).
        if (mtr::sprite_picking::pick_mode() && !g_drag.active && sz_ok) {
            dl->AddRect(ImVec2(0, 0), ImVec2(sz.x, sz.y),
                        IM_COL32(80, 220, 250, 60), 0.0f, 0, 2.0f);
        }
    }

    mtr::console::draw();
    if (g_fps_overlay.load()) {
        draw_fps_overlay();
    }

    ImGui::EndFrame();
    if (any_draw) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    // Capture AFTER menu draws so the screenshot reflects what the user sees
    // (game frame + menu overlay if visible). F12 sets the request via poll;
    // we consume it here so the same frame's render is what gets saved.
    mtr::screenshot::try_capture(dev);
}

void on_reset_pre(IDirect3DDevice9* /*dev*/) {
    if (g_imgui_ready.load()) {
        ImGui::SetCurrentContext(g_ctx);
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
}

void on_reset_post(IDirect3DDevice9* dev) {
    if (g_imgui_ready.load()) {
        ImGui::SetCurrentContext(g_ctx);
        ImGui_ImplDX9_CreateDeviceObjects();
        g_device = dev;
    }
}

bool is_visible() { return g_visible.load(); }

void shutdown() {
    // Force-flush any pending ini save before we tear down so a debounce
    // window in flight at process detach doesn't drop the last edit.
    mtr::ui_aspect_rules::save_to_ini();
    if (g_imgui_ready.load()) {
        if (g_orig_wndproc && g_hwnd.load()) {
            SetWindowLongPtrA(g_hwnd.load(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
        }
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        if (g_ctx) ImGui::DestroyContext(g_ctx);
        g_imgui_ready = false;
    }
}

} // namespace mtr::menu
