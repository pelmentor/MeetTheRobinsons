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
#include "mtr/dt_correctness.h"
#include "mtr/test_harness.h"
#include "menu/menu_internal.h"

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
    // Virtual cursor — the menu drives ImGui's MousePos from here while
    // UI is visible. Updated from DI mouse deltas in the GetDeviceState
    // hook — works regardless of whether the OS cursor is pinned by DI
    // exclusive, raw input, or any other Windows quirk.
    void seed_virt_cursor(int x, int y);
    void set_virt_clamp(int w, int h);
    int  virt_cursor_x();
    int  virt_cursor_y();
    bool virt_button(int b);
    int  virt_drain_wheel();
    void set_virt_scale_pct(int percent);
    int  virt_scale_pct();
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
namespace mtr::trigger_overlay {
    bool enabled();
    void tick(IDirect3DDevice9* dev);
}
namespace mtr::npc_overlay {
    bool enabled();
    void tick(IDirect3DDevice9* dev);
}
namespace mtr::prop_overlay {
    bool enabled();
    void tick(IDirect3DDevice9* dev);
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

// Bring the split tabs + FPS overlay draw routine into the unnamed
// namespace so draw_menu() and on_end_scene() can call them unqualified.
// section_begin/end and the other helpers were used by the in-file tabs
// before extraction; with every tab now in menu/tab_*.cpp those usings
// are no longer needed in this TU.
using detail::draw_fps_overlay;
using detail::fps_overlay_enabled;
using detail::set_fps_overlay_enabled;
using detail::fps_overlay_corner;
using detail::set_fps_overlay_corner;
using detail::tab_camera;
using detail::tab_picture;
using detail::tab_world;
using detail::tab_performance;
using detail::tab_tools;
using detail::tab_debug;

std::atomic<bool> g_visible{false};
std::atomic<bool> g_imgui_ready{false};
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<IDirect3DDevice9*> g_device{nullptr};
ImGuiContext* g_ctx = nullptr;
WNDPROC g_orig_wndproc = nullptr;

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

// Section helpers, FPS overlay, and tab style convention live in the split
// menu/menu_helpers.cpp + menu/menu_fps_overlay.cpp under
// `mtr::menu::detail::` (declared in menu/menu_internal.h). The using-decls
// at the top of this anonymous namespace let existing tab bodies still
// resolve the unqualified names while the tab extraction proceeds.

// tab_camera lives in menu/tab_camera.cpp under mtr::menu::detail::.

// tab_display lives in menu/tab_display.cpp under mtr::menu::detail::.

// tab_world lives in menu/tab_world.cpp under mtr::menu::detail::.

// tab_tools lives in menu/tab_tools.cpp under mtr::menu::detail::.

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
            if (ImGui::BeginTabItem("Camera"))      { tab_camera();      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Picture"))     { tab_picture();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("World"))       { tab_world();       ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Performance")) { tab_performance(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Tools"))       { tab_tools();       ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Debug"))       { tab_debug();       ImGui::EndTabItem(); }
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

    // When UI is interactive, the virt-cursor path (driven by DI deltas)
    // is the canonical mouse input source — feeding GetCursorPos /
    // GetAsyncKeyState here would inject pos+button events with frozen
    // OS-cursor coords BEFORE the virt-pos event lands, so clicks would
    // register at the wrong position (ImGui processes events in queue
    // order). Skip when UI is visible; track button state silently to
    // avoid stale-edge synthesis on the next ui_open=false frame.
    const bool ui_open_now = g_visible.load() || mtr::console::is_visible();

    HWND hwnd = g_hwnd.load();
    POINT pt;
    if (!ui_open_now && hwnd && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
        io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
    }

    static bool prev_lmb = false, prev_rmb = false, prev_mmb = false;
    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool mmb = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    if (!ui_open_now) {
        if (lmb != prev_lmb) { io.AddMouseButtonEvent(0, lmb); prev_lmb = lmb; }
        if (rmb != prev_rmb) { io.AddMouseButtonEvent(1, rmb); prev_rmb = rmb; }
        if (mmb != prev_mmb) { io.AddMouseButtonEvent(2, mmb); prev_mmb = mmb; }
    } else {
        prev_lmb = lmb; prev_rmb = rmb; prev_mmb = mmb;
    }

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
    // Test harness: drives in-mod scenarios when -mtrasi-test=<name> is on
    // the cmdline. No-op (single relaxed atomic load) otherwise.
    mtr::test_harness::tick();

    // ImGui draws its own cursor when an INTERACTIVE mtr-asi window is up.
    // FPS overlay is non-interactive (NoInputs), so it doesn't force the
    // cursor on -- but it does need ImGui::Render() to run, hence the
    // separate `any_draw` flag below.
    const bool any_interactive = g_visible.load() || mtr::console::is_visible();
    const bool any_draw        = any_interactive || fps_overlay_enabled()
                              || mtr::trigger_overlay::enabled()
                              || mtr::npc_overlay::enabled()
                              || mtr::prop_overlay::enabled();
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
        // Skip the LL drain feed when UI is interactive — its
        // MSLLHOOKSTRUCT.pt carries the FROZEN OS-cursor position under
        // DI exclusive, and routing button/wheel events through it would
        // fire them at the frozen position before the virt-cursor pos
        // event lands. Drain the queue (so it doesn't backlog) but
        // discard. Buttons + wheel come from the virt path instead.
        if (n > 0 && !any_interactive) {
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

    // Virt-cursor feed (the LAST mouse-pos event of the frame, so it wins
    // over earlier feeders that may carry frozen positions when the OS
    // cursor is pinned by DI / Raw Input / etc.). The virt cursor is fed
    // by DI mouse deltas in the GetDeviceState hook, which are always live
    // regardless of OS cursor state. ImGui sees a perfectly tracking
    // cursor while the menu is up.
    static bool s_virt_prev_open = false;
    if (any_interactive) {
        HWND hwnd_virt = g_hwnd.load();
        if (hwnd_virt) {
            RECT cr;
            if (GetClientRect(hwnd_virt, &cr)) {
                mtr::dinput_hook::set_virt_clamp(cr.right - cr.left,
                                                 cr.bottom - cr.top);
                if (!s_virt_prev_open) {
                    // Hidden -> visible edge: seed virt cursor to the
                    // current GetCursorPos in client coords (or window
                    // center if GetCursorPos isn't useful). This way the
                    // cursor "appears" at the user's last physical mouse
                    // position rather than always at (0,0).
                    POINT pt;
                    int sx = (cr.right - cr.left) / 2;
                    int sy = (cr.bottom - cr.top) / 2;
                    if (GetCursorPos(&pt) && ScreenToClient(hwnd_virt, &pt)) {
                        if (pt.x >= 0 && pt.x < cr.right - cr.left &&
                            pt.y >= 0 && pt.y < cr.bottom - cr.top) {
                            sx = pt.x; sy = pt.y;
                        }
                    }
                    mtr::dinput_hook::seed_virt_cursor(sx, sy);
                }
            }
        }
        s_virt_prev_open = true;

        const int vx = mtr::dinput_hook::virt_cursor_x();
        const int vy = mtr::dinput_hook::virt_cursor_y();
        io.AddMousePosEvent(static_cast<float>(vx), static_cast<float>(vy));

        // Buttons from DI rgbButtons — always live, immune to cursor pin.
        // Edge-detect against the previous virt-button sample so we only
        // emit transitions.
        static bool s_prev_vlmb = false, s_prev_vrmb = false, s_prev_vmmb = false;
        const bool vlmb = mtr::dinput_hook::virt_button(0);
        const bool vrmb = mtr::dinput_hook::virt_button(1);
        const bool vmmb = mtr::dinput_hook::virt_button(2);
        if (vlmb != s_prev_vlmb) { io.AddMouseButtonEvent(0, vlmb); s_prev_vlmb = vlmb; }
        if (vrmb != s_prev_vrmb) { io.AddMouseButtonEvent(1, vrmb); s_prev_vrmb = vrmb; }
        if (vmmb != s_prev_vmmb) { io.AddMouseButtonEvent(2, vmmb); s_prev_vmmb = vmmb; }

        // Wheel — DIMOUSESTATE.lZ already in WHEEL_DELTA units (120/notch).
        if (const int vw = mtr::dinput_hook::virt_drain_wheel(); vw != 0) {
            io.AddMouseWheelEvent(0.0f, static_cast<float>(vw) / 120.0f);
        }
    } else {
        s_virt_prev_open = false;
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
    if (fps_overlay_enabled()) {
        draw_fps_overlay();
    }

    // Trigger box overlay — projects engine view+proj of AABBs onto the
    // foreground draw list. Runs INSIDE the ImGui frame (after NewFrame,
    // before EndFrame) so GetForegroundDrawList() returns the live list.
    // Cheap when disabled (single relaxed atomic).
    mtr::trigger_overlay::tick(dev);

    // NPC overlay — text labels at each NPC's projected world pos.
    // Same lifecycle constraints as trigger overlay; sits AFTER it so
    // labels draw on top of any wireframes that occlude them.
    mtr::npc_overlay::tick(dev);

    // Prop overlay — labels at world props (disassembleable, scannable,
    // targetable, ...). Draws AFTER npc_overlay so disassembly targets
    // (the high-value debug signal) win the screen-space z-order over
    // NPC labels per architecture audit afbf879e.
    mtr::prop_overlay::tick(dev);

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

namespace detail {
void* current_device() { return g_device.load(); }
void* current_hwnd()   { return static_cast<void*>(g_hwnd.load()); }
} // namespace detail

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
