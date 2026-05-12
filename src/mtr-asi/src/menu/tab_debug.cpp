// Debug tab: power-user / diagnostic controls.
//
//   [advanced] Manual sprite transform override — direct slider control of
//                                                  the sprite_matrix
//                                                  multipliers, separate from
//                                                  the per-screen rules in
//                                                  Picture > "HUD and menu
//                                                  aspect ratio".
//   [diagnostic] Sprite list probe — request a one-frame full-list CSV dump
//                                     of every sprite-batcher entry to
//                                     Game/mtr-asi-sprite-probe.csv.
//
// Future homes for sprite_split (currently a TreeNode inside Picture's HUD
// aspect section), state_key probe (also nested inside Picture), force_vis,
// vis_test_probe, scene_vis_log — once those need more breathing room than
// a TreeNode collapse provides, lift them here.

#include "menu_internal.h"
#include "imgui.h"
#include "mtr/coop_spawn_probe.h"
#include "mtr/save_system.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>

namespace mtr::log { void info(const char* fmt, ...); }
namespace mtr::sprite_matrix {
    bool  enabled();
    void  set_enabled(bool v);
    bool  auto_from_rules();
    float mul_a_a1(); float mul_a_a2(); float mul_a_a3();
    float mul_b_a1(); float mul_b_a2(); float mul_b_a3();
    void  set_mul_a(float a1, float a2, float a3);
    void  set_mul_b(float a1, float a2, float a3);
    void  reset();
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
namespace mtr::widget_probe {
    bool         installed();
    unsigned int frame_table_size();
    void         request_dump_next_frame();
    void         caller_audit_arm(int budget);
    void         caller_audit_disarm();
    bool         caller_audit_armed();
    int          caller_audit_count();
}
namespace mtr::aspect          { float current(); }
namespace mtr::ui_aspect_rules { void request_save(); }
namespace mtr::trigger_overlay {
    bool enabled();
    void set_enabled(bool v);
    int  visible_box_count();
    bool show_test_box();
    void set_show_test_box(bool v);
}
namespace mtr::npc_overlay {
    bool enabled();         void set_enabled(bool v);
    int  visible_npc_count();
    bool show_name();       void set_show_name(bool v);
    bool show_pos();        void set_show_pos(bool v);
    bool show_distance();   void set_show_distance(bool v);
    float distance_limit(); void set_distance_limit(float v);
}
namespace mtr::prop_overlay {
    bool enabled();              void set_enabled(bool v);
    int  visible_prop_count();
    bool show_disassembleable(); void set_show_disassembleable(bool v);
    bool show_scannable();       void set_show_scannable(bool v);
    bool show_targetable();      void set_show_targetable(bool v);
    bool show_climbable();       void set_show_climbable(bool v);
    bool show_push_pullable();   void set_show_push_pullable(bool v);
    bool show_levitate();        void set_show_levitate(bool v);
    bool show_lock_to_path();    void set_show_lock_to_path(bool v);
    bool show_ss_target();       void set_show_ss_target(bool v);
    bool show_name();            void set_show_name(bool v);
    bool show_pos();             void set_show_pos(bool v);
    bool show_distance();        void set_show_distance(bool v);
    bool show_tags();            void set_show_tags(bool v);
    float distance_limit();      void set_distance_limit(float v);
}
namespace mtr::peripheral_cull_probe {
    bool     installed();
    bool     force_pass_corners();
    bool     force_pass_sphere();
    void     set_force_pass_corners(bool v);
    void     set_force_pass_sphere(bool v);
    uint64_t last_dispatches();
    uint64_t last_corner_calls();
    uint64_t last_sphere_calls();
    uint64_t last_corner_forced();
    uint64_t last_sphere_forced();
    uint64_t last_engine_corner_delta();
    uint64_t last_engine_sphere_delta();
    bool     snapshot_planes(float out[7][4]);
    bool     force_pass_plane(int idx);
    void     set_force_pass_plane(int idx, bool v);
    uint32_t force_pass_plane_mask();
    void     set_force_pass_plane_mask(uint32_t mask);
}

namespace mtr::menu::detail {

namespace {

DWORD WINAPI direct_load_slot1_worker(LPVOID /*arg*/) {
    mtr::save_system::load_slot(0);
    return 0;
}

}  // namespace

void tab_debug() {
    if (bool open = section_begin("[advanced] Manual sprite transform override")) {
        heading_with_info("Direct slider control of the sprite transform matrix",
            "Use the \"HUD and menu aspect ratio\" section above for the "
            "normal per-screen workflow. This section is for dialing in a "
            "uniform override regardless of which screen is up. When the "
            "per-screen rules are also on, they override scale-X and "
            "translate-X with the per-screen factor; the sliders below "
            "won't affect those components while rules are active.");
        ImGui::Spacing();

        bool en = mtr::sprite_matrix::enabled();
        if (ImGui::Checkbox("On / off##sprite_master2", &en)) {
            mtr::sprite_matrix::set_enabled(en);
            mtr::ui_aspect_rules::request_save();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset all to defaults")) {
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
            float aa1 = mtr::sprite_matrix::mul_a_a1();
            float aa2 = mtr::sprite_matrix::mul_a_a2();
            float aa3 = mtr::sprite_matrix::mul_a_a3();
            ImGui::TextDisabled("Scale matrix (default 2.0, -2.0, 1.0):");
            bool a_changed = false;
            a_changed |= ImGui::SliderFloat("scale X mul##spritea1", &aa1, 0.25f, 4.0f, "%.4f");
            a_changed |= ImGui::SliderFloat("scale Y mul##spritea2", &aa2, 0.25f, 4.0f, "%.4f");
            a_changed |= ImGui::SliderFloat("scale Z mul##spritea3", &aa3, 0.25f, 4.0f, "%.4f");
            if (a_changed) mtr::sprite_matrix::set_mul_a(aa1, aa2, aa3);
            ImGui::Text("=> scale(%.3f, %.3f, %.3f)", 2.0f * aa1, -2.0f * aa2, 1.0f * aa3);
            ImGui::Spacing();

            float ba1 = mtr::sprite_matrix::mul_b_a1();
            float ba2 = mtr::sprite_matrix::mul_b_a2();
            float ba3 = mtr::sprite_matrix::mul_b_a3();
            ImGui::TextDisabled("Translate matrix (default -2.0, -2.0, 0.0):");
            bool b_changed = false;
            b_changed |= ImGui::SliderFloat("translate X mul##spriteb1", &ba1, 0.25f, 4.0f, "%.4f");
            b_changed |= ImGui::SliderFloat("translate Y mul##spriteb2", &ba2, 0.25f, 4.0f, "%.4f");
            b_changed |= ImGui::SliderFloat("translate Z mul##spriteb3", &ba3, 0.25f, 4.0f, "%.4f");
            if (b_changed) mtr::sprite_matrix::set_mul_b(ba1, ba2, ba3);
            ImGui::Text("=> translate(%.3f, %.3f, %.3f)", -2.0f * ba1, -2.0f * ba2, 0.0f * ba3);
            ImGui::TreePop();
        }
        section_end(open);
    }

    if (bool open = section_begin("[diagnostic] Sprite list probe")) {
        heading_with_info("Capture sprite-list data for offline analysis",
            "Captures the sprite-render list to CSV. Useful for figuring "
            "out which sprites belong to menus vs HUD, what data each "
            "carries, etc.\n\n"
            "Workflow: open the game state you want to look at, click an "
            "Arm button, let it capture, then open the CSV in Excel or "
            "sqlite. Off by default - zero cost when disarmed.");
        ImGui::Spacing();

        if (!mtr::sprite_probe::installed()) {
            ImGui::TextDisabled("Probe not installed (hook failed at startup).");
            section_end(open);
        } else {
            const bool armed = mtr::sprite_probe::armed();
            ImGui::Text("Status: ");
            ImGui::SameLine();
            if (armed) {
                ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                    "Capturing (%d frames left, %llu rows so far)",
                    mtr::sprite_probe::frames_remaining(),
                    static_cast<unsigned long long>(mtr::sprite_probe::total_captured()));
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Idle");
            }
            ImGui::Text("Last frame: %llu sprites",
                static_cast<unsigned long long>(mtr::sprite_probe::last_frame_count()));
            ImGui::Spacing();

            ImGui::BeginDisabled(armed);
            if (ImGui::Button("Capture 30 frames##sp_arm30"))   mtr::sprite_probe::arm(30);
            ImGui::SameLine();
            if (ImGui::Button("Capture 60 frames##sp_arm60"))   mtr::sprite_probe::arm(60);
            ImGui::SameLine();
            if (ImGui::Button("Capture 180 frames##sp_arm180")) mtr::sprite_probe::arm(180);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!armed);
            if (ImGui::Button("Stop##sp_disarm")) mtr::sprite_probe::disarm();
            ImGui::EndDisabled();
            ImGui::Spacing();

            char path[MAX_PATH] = {0};
            if (mtr::sprite_probe::csv_path(path, sizeof(path))) {
                ImGui::TextDisabled("Output CSV: %s", path);
            }
            section_end(open);
        }
    }

    if (bool open = section_begin("[diagnostic] Gradient widget identity dump")) {
        heading_with_info("One click: capture everything needed to discriminate the gradient widgets",
            "Stand on the screen with the IDS_TOP / IDS_BOTTOM / "
            "IDS_HIGHLIGHT_BACKGROUND gradients visible (the main "
            "GameSelectScreen menu). Click the button. The mod will:\n\n"
            "  1. Arm the sprite-batcher CSV for 60 frames "
            "(state_key, position, UV per sprite per frame).\n"
            "  2. Dump the next frame's widget side-table to mtr-asi.log "
            "(SpriteEntry* -> widget_name pairs that the always-on "
            "sub_4E9350 hook has captured).\n\n"
            "Outputs:\n"
            "  Game/mtr-asi-sprite-probe.csv\n"
            "  Game/mtr-asi.log (search for \"frame side-table contents\")\n\n"
            "No verbose probe arming, no freeze. Both captures complete "
            "within ~¼ second; you can close the menu (F2) immediately "
            "after clicking. Quit the game when done and ship both files.");
        ImGui::Spacing();

        const bool sp_ready = mtr::sprite_probe::installed();
        const bool wp_ready = mtr::widget_probe::installed();
        const bool sp_armed = sp_ready && mtr::sprite_probe::armed();

        ImGui::Text("Sprite probe: %s",
            sp_ready ? (sp_armed ? "capturing" : "ready") : "not installed");
        ImGui::Text("Widget probe: %s (last-frame side-table = %u entries)",
            wp_ready ? "ready" : "not installed",
            wp_ready ? mtr::widget_probe::frame_table_size() : 0u);
        ImGui::Spacing();

        ImGui::BeginDisabled(!sp_ready || !wp_ready || sp_armed);
        if (ImGui::Button("Capture gradient diag (60-frame CSV + side-table dump)##gd_capture")) {
            mtr::sprite_probe::arm(60);
            mtr::widget_probe::request_dump_next_frame();
            mtr::log::info("gradient-diag: button clicked; sprite_probe armed for"
                           " 60 frames + widget side-table dump requested");
        }
        ImGui::EndDisabled();

        // Phase 0B caller-PC audit (widget-name capture plan v3, 2026-05-09).
        // Pairs every sub_4E9350 caller's return address with the resulting
        // SpriteEntry's state_key + sort_key. Use to find which Render
        // method submits a given widget — that's the vtable slot we want
        // to hook for engine-name capture.
        ImGui::Spacing();
        const bool ca_armed = wp_ready && mtr::widget_probe::caller_audit_armed();
        ImGui::Text("Caller-PC audit: %s (unique pairs captured = %d)",
            wp_ready
                ? (ca_armed ? "armed" : "ready")
                : "not installed",
            wp_ready ? mtr::widget_probe::caller_audit_count() : 0);
        ImGui::BeginDisabled(!wp_ready || ca_armed);
        if (ImGui::Button("Arm caller-PC audit (256 unique pairs)##ca_arm")) {
            mtr::widget_probe::caller_audit_arm(256);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!wp_ready || !ca_armed);
        if (ImGui::Button("Disarm##ca_disarm")) {
            mtr::widget_probe::caller_audit_disarm();
        }
        ImGui::EndDisabled();

        section_end(open);
    }

    if (bool open = section_begin("Trigger box overlay")) {
        heading_with_info("Project AABBs onto the screen via engine view+proj",
            "Phase 1 scaffold: draws a hardcoded green wireframe AABB at "
            "world (0,0,0) with extents (10,10,10). Validates that the "
            "view matrix at 0x724C10 + projection at 0x745AA0 + the "
            "homogeneous parametric clip pipeline produce a coherent "
            "screen-space wireframe.\n\n"
            "If you see a green box that stays attached to a fixed world "
            "location as you move, the projection is working. Lines that "
            "cross the camera near plane should clip cleanly with no "
            "tearing or NaN.\n\n"
            "Next phases (per research/findings/trigger-box-overlay-plan-"
            "2026-05-09.md): walk the engine entity manager for "
            "trigger_volume / triggerbox / triggeraoe entities and draw "
            "their actual bounding boxes.");
        ImGui::Spacing();

        bool en = mtr::trigger_overlay::enabled();
        if (ImGui::Checkbox("Enable overlay##trig_en", &en)) {
            mtr::trigger_overlay::set_enabled(en);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(Phase 1 \xE2\x80\x94 hardcoded test box)");

        ImGui::BeginDisabled(!en);
        bool tb = mtr::trigger_overlay::show_test_box();
        if (ImGui::Checkbox("Show test box at world (0,0,0)##trig_test", &tb)) {
            mtr::trigger_overlay::set_show_test_box(tb);
        }
        ImGui::EndDisabled();

        ImGui::TextDisabled("Visible boxes this frame: %d",
            mtr::trigger_overlay::visible_box_count());

        section_end(open);
    }

    if (bool open = section_begin("NPC overlay")) {
        heading_with_info("Project NPC info onto the world via engine view+proj",
            "Text labels at each NPC's world position. Walks the engine "
            "transform list at dword_724DE4 (same data structure the M5 "
            "interp + freecam MMB-tp use). Per-NPC reads:\n"
            "  - name from entity+0x50 (with strict ASCII validation)\n"
            "  - pos from *(entity+0x48)+0x10 (renderer's source) "
            "with fallback to entity+0x58\n"
            "  - distance from camera (world matrix row 3 = cam_pos)\n\n"
            "Labels are drawn on top of the trigger-box overlay using the "
            "same projection scaffold (shared math in overlay_math.h). "
            "Single SEH guard wraps the entire walker so a corrupted "
            "entity bails out cleanly without crashing the frame.\n\n"
            "Phase 1 ships name+pos+distance. Phase 2 (anim state) and "
            "Phase 3 (kv_get registry dump) are gated on additional RE work.");
        ImGui::Spacing();

        bool en = mtr::npc_overlay::enabled();
        if (ImGui::Checkbox("Enable overlay##npc_en", &en)) {
            mtr::npc_overlay::set_enabled(en);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(walks dword_724DE4 every frame)");

        ImGui::BeginDisabled(!en);
        bool sn = mtr::npc_overlay::show_name();
        if (ImGui::Checkbox("Show name##npc_sn", &sn)) mtr::npc_overlay::set_show_name(sn);
        ImGui::SameLine();
        bool sp = mtr::npc_overlay::show_pos();
        if (ImGui::Checkbox("Show pos##npc_sp", &sp)) mtr::npc_overlay::set_show_pos(sp);
        ImGui::SameLine();
        bool sd = mtr::npc_overlay::show_distance();
        if (ImGui::Checkbox("Show distance##npc_sd", &sd)) mtr::npc_overlay::set_show_distance(sd);

        float dl = mtr::npc_overlay::distance_limit();
        if (ImGui::SliderFloat("Distance limit (0 = no limit)##npc_dl",
                               &dl, 0.0f, 5000.0f, "%.0f")) {
            mtr::npc_overlay::set_distance_limit(dl);
        }
        ImGui::EndDisabled();

        ImGui::TextDisabled("Visible NPCs this frame: %d",
            mtr::npc_overlay::visible_npc_count());

        section_end(open);
    }

    if (bool open = section_begin("Prop overlay")) {
        heading_with_info("Project prop info onto the world (disassembleable etc.)",
            "Sister to NPC overlay. Walks the engine transform list and "
            "for each entity calls the kv-bag accessor to check for "
            "prop properties (propDisassembleable, propScannable, "
            "propTargetable, propClimbable, propPushPullable, propLevitate, "
            "propLockToPath, propSSTarget). Shows a label at each prop's "
            "world position.\n\n"
            "When Wilbur disassembles a prop, the engine removes the "
            "entity from the transform list and the label disappears "
            "next frame \xE2\x80\x94 no engine-side death-event hook needed.\n\n"
            "Phase 1 default: only disassembleable shown. Other tags are "
            "available in the 'Tag filters' tree below \xE2\x80\x94 enabling them "
            "adds one kv-call per entity per frame, which is fine for "
            "small toggle counts but profile via the autonomous validation "
            "before enabling all 8 in production.");
        ImGui::Spacing();

        bool en = mtr::prop_overlay::enabled();
        if (ImGui::Checkbox("Enable overlay##prop_en", &en)) {
            mtr::prop_overlay::set_enabled(en);
        }

        ImGui::BeginDisabled(!en);
        float dl = mtr::prop_overlay::distance_limit();
        if (ImGui::SliderFloat("Distance limit (0 = no limit)##prop_dl",
                               &dl, 0.0f, 5000.0f, "%.0f")) {
            mtr::prop_overlay::set_distance_limit(dl);
        }

        if (ImGui::TreeNode("Tag filters \xE2\x80\x94 which props to show")) {
            bool t;
            t = mtr::prop_overlay::show_disassembleable();
            if (ImGui::Checkbox("propDisassembleable (yellow)##prop_t_dis", &t))
                mtr::prop_overlay::set_show_disassembleable(t);
            t = mtr::prop_overlay::show_scannable();
            if (ImGui::Checkbox("propScannable##prop_t_scan", &t))
                mtr::prop_overlay::set_show_scannable(t);
            t = mtr::prop_overlay::show_targetable();
            if (ImGui::Checkbox("propTargetable##prop_t_tgt", &t))
                mtr::prop_overlay::set_show_targetable(t);
            t = mtr::prop_overlay::show_climbable();
            if (ImGui::Checkbox("propClimbable##prop_t_climb", &t))
                mtr::prop_overlay::set_show_climbable(t);
            t = mtr::prop_overlay::show_push_pullable();
            if (ImGui::Checkbox("propPushPullable##prop_t_push", &t))
                mtr::prop_overlay::set_show_push_pullable(t);
            t = mtr::prop_overlay::show_levitate();
            if (ImGui::Checkbox("propLevitate*##prop_t_lev", &t))
                mtr::prop_overlay::set_show_levitate(t);
            t = mtr::prop_overlay::show_lock_to_path();
            if (ImGui::Checkbox("propLockToPath##prop_t_path", &t))
                mtr::prop_overlay::set_show_lock_to_path(t);
            t = mtr::prop_overlay::show_ss_target();
            if (ImGui::Checkbox("propSSTarget##prop_t_ss", &t))
                mtr::prop_overlay::set_show_ss_target(t);
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Label fields \xE2\x80\x94 what to show in each label")) {
            bool t;
            t = mtr::prop_overlay::show_name();
            if (ImGui::Checkbox("Name##prop_f_name", &t)) mtr::prop_overlay::set_show_name(t);
            t = mtr::prop_overlay::show_tags();
            if (ImGui::Checkbox("Tags [...] suffix##prop_f_tags", &t)) mtr::prop_overlay::set_show_tags(t);
            t = mtr::prop_overlay::show_pos();
            if (ImGui::Checkbox("Position (x,y,z)##prop_f_pos", &t)) mtr::prop_overlay::set_show_pos(t);
            t = mtr::prop_overlay::show_distance();
            if (ImGui::Checkbox("Distance from camera##prop_f_dist", &t)) mtr::prop_overlay::set_show_distance(t);
            ImGui::TreePop();
        }
        ImGui::EndDisabled();

        ImGui::TextDisabled("Visible props this frame: %d",
            mtr::prop_overlay::visible_prop_count());

        section_end(open);
    }

    if (bool open = section_begin("[diagnostic] Peripheral cull probe")) {
        heading_with_info("What's making things at the edges of the screen disappear?",
            "Per the peripheral-cull-pipeline-2026-05-09 RE doc, the engine "
            "uses a single global frustum struct at 0x726498 for ALL "
            "per-object visibility tests. Prior overrides on the per-camera "
            "projection cache (outer+0xD4) did not affect this gate.\n\n"
            "This probe reads the engine's two cull counters (sphere reject + "
            "corner reject) and the live 7-plane frustum, and lets you "
            "short-circuit either cull stage to confirm where the gate is.\n\n"
            "Bottom-right tells you the live planes; toggle Force-pass on the "
            "stage you suspect, look at the screen edges, and watch the "
            "counters.");
        ImGui::Spacing();

        if (!mtr::peripheral_cull_probe::installed()) {
            ImGui::TextDisabled("Probe not installed (MinHook setup failed).");
            section_end(open);
        } else {
            ImGui::TextDisabled("Per-frame counts (last completed frame):");
            ImGui::BulletText("Cull dispatches (entries to 0x4E0AD0): %llu",
                static_cast<unsigned long long>(mtr::peripheral_cull_probe::last_dispatches()));
            ImGui::BulletText("Sphere-cull stage:  hooked %llu / engine rejected %llu / forced-pass %llu",
                static_cast<unsigned long long>(mtr::peripheral_cull_probe::last_sphere_calls()),
                static_cast<unsigned long long>(mtr::peripheral_cull_probe::last_engine_sphere_delta()),
                static_cast<unsigned long long>(mtr::peripheral_cull_probe::last_sphere_forced()));
            ImGui::BulletText("Corner-cull stage:  hooked %llu / engine rejected %llu / forced-pass %llu",
                static_cast<unsigned long long>(mtr::peripheral_cull_probe::last_corner_calls()),
                static_cast<unsigned long long>(mtr::peripheral_cull_probe::last_engine_corner_delta()),
                static_cast<unsigned long long>(mtr::peripheral_cull_probe::last_corner_forced()));
            ImGui::Spacing();

            ImGui::TextDisabled("Short-circuit cull stages (force-pass):");
            bool fpc = mtr::peripheral_cull_probe::force_pass_corners();
            if (ImGui::Checkbox("Force-pass corner cull (0x4E0370) - test if THIS is the gate##cull_fpc", &fpc)) {
                mtr::peripheral_cull_probe::set_force_pass_corners(fpc);
            }
            info_pill("If corner objects stop disappearing when this is on, "
                "we have confirmed the cull is at cull_aabb_corners_vs_global_frustum. "
                "Performance cost: every object that would have been corner-culled "
                "now goes to the GPU draw path. Expect frame drop in heavy scenes.");

            bool fps = mtr::peripheral_cull_probe::force_pass_sphere();
            if (ImGui::Checkbox("Force-pass sphere cull (0x4DFF20)##cull_fps", &fps)) {
                mtr::peripheral_cull_probe::set_force_pass_sphere(fps);
            }
            info_pill("Sphere/center cull stage. Less likely to be the corner-cull "
                "gate (sphere vs plane is symmetric in screen-space), but useful "
                "as A/B comparison.");
            ImGui::Spacing();

            if (ImGui::TreeNode("Live frustum planes (g_cull_frustum at 0x726498+128)")) {
                heading_with_info("Per-plane force-pass — find which plane is far / near / etc.",
                    "Each row is one of the 7 planes inside g_cull_frustum. "
                    "Toggle 'pass' on a plane to short-circuit ITS contribution "
                    "to the cull (we overwrite that plane with (0,0,0,-1) for "
                    "the duration of each cull dispatch, then restore).\n\n"
                    "Practical use: planes 1 and 6 are the gated near/far in "
                    "the standard 6-plane frustum layout. Toggling plane 6 "
                    "(or 1) typically gives 'infinite draw distance'. Other "
                    "planes are top/bottom/left/right (toggle to see "
                    "off-frustum geometry).\n\n"
                    "If you can't tell which is far: look at the d column — "
                    "the plane with the largest |d| is usually far.");
                ImGui::Spacing();
                float planes[7][4]{};
                if (mtr::peripheral_cull_probe::snapshot_planes(planes)) {
                    static const char* slot_names[7] = {
                        "+128 plane 0", "+144 plane 1 (skip if [+456])",
                        "+160 plane 2", "+176 plane 3", "+192 plane 4",
                        "+208 plane 5", "+224 plane 6 (skip if [+457])",
                    };
                    if (ImGui::BeginTable("cull_planes", 6,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("pass");
                        ImGui::TableSetupColumn("slot");
                        ImGui::TableSetupColumn("nx");
                        ImGui::TableSetupColumn("ny");
                        ImGui::TableSetupColumn("nz");
                        ImGui::TableSetupColumn("d");
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < 7; ++i) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            bool fp = mtr::peripheral_cull_probe::force_pass_plane(i);
                            char id[32]; std::snprintf(id, sizeof(id), "##fpp%d", i);
                            if (ImGui::Checkbox(id, &fp)) {
                                mtr::peripheral_cull_probe::set_force_pass_plane(i, fp);
                            }
                            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", slot_names[i]);
                            ImGui::TableSetColumnIndex(2); ImGui::Text("%+.4f", planes[i][0]);
                            ImGui::TableSetColumnIndex(3); ImGui::Text("%+.4f", planes[i][1]);
                            ImGui::TableSetColumnIndex(4); ImGui::Text("%+.4f", planes[i][2]);
                            ImGui::TableSetColumnIndex(5); ImGui::Text("%+.4f", planes[i][3]);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::Spacing();
                    if (ImGui::Button("Infinite draw distance (toggle planes 1 + 6)")) {
                        const uint32_t cur = mtr::peripheral_cull_probe::force_pass_plane_mask();
                        const uint32_t bits = (1u << 1) | (1u << 6);
                        mtr::peripheral_cull_probe::set_force_pass_plane_mask(cur ^ bits);
                    }
                    info_pill("Toggles the gated near + far plane slots. If "
                        "draw distance becomes effectively infinite, plane 6 "
                        "is far and that's the one. If geometry near the "
                        "camera disappears, plane 1 was actually NEAR — un-"
                        "toggle it. Use individual checkboxes above for "
                        "fine-grained control.");
                    ImGui::SameLine();
                    if (ImGui::Button("Clear all##fpp_clear")) {
                        mtr::peripheral_cull_probe::set_force_pass_plane_mask(0);
                    }
                } else {
                    ImGui::TextDisabled("No frustum snapshot yet (engine not "
                        "populated, or first frame still pending).");
                }
                ImGui::TreePop();
            }
            section_end(open);
        }
    }

    if (bool open = section_begin("[experimental] Direct save load (RULE No.1 root-cause)")) {
        heading_with_info("Load slot 1 by direct API call, bypassing menu nav",
            "First-cut implementation of mtr::save_system::load_slot. Sets "
            "the engine save-system state machine's request opcode to 5 "
            "(LOAD), spawns a worker thread that runs the engine's pump "
            "function (sub_575D60 @ 0x00575D60), waits for the done flag.\n\n"
            "Pre-conditions:\n"
            "  - Engine must be past WinMain init. Safest: click after the "
            "main menu (GameSelectScreen) is visible.\n"
            "  - At least one save in slot 1 (display name 'ROBINSON HOUSE' "
            "etc).\n\n"
            "What gets logged (mtr-asi.log):\n"
            "  - dyn_buf address (unk_72F824[0])\n"
            "  - timed_out flag (true if pump didn't signal done within 10s)\n"
            "  - result code from the engine ([196]; 0 = success)\n\n"
            "What this does NOT do:\n"
            "  - Does NOT push a gameplay screen after load. Engine state "
            "has the save data but you stay on whatever screen you were "
            "on. Resume-gameplay is a separate phase (TBD).\n"
            "  - May freeze briefly while the pump runs (worker thread, but "
            "engine main thread may serialize on save subsystem locks).\n\n"
            "Risk: first-cut. May crash the engine if state assumptions "
            "are wrong. SEH-wrapped but the engine's panic handler may eat "
            "the exception silently. If the game freezes after click, force-"
            "kill via Task Manager.\n\n"
            "RULE No.1 rationale: replaces the menu-nav-via-DIK-injection "
            "approach with a direct call to the engine's documented save-"
            "system entry point (the pump function). No simulated "
            "keystrokes, no menu state-machine to wrestle with.");
        ImGui::Spacing();

        const bool busy = mtr::save_system::load_in_progress();
        ImGui::TextDisabled("Status: %s", busy ? "load in progress" : "idle");
        ImGui::Spacing();

        if (ImGui::Button(busy ? "Loading..." : "Load slot 1 (direct API)##save_load_slot1")
            && !busy) {
            mtr::log::info("[debug-tab] user clicked: direct load slot 1");
            // Run on a worker thread so the UI doesn't freeze. load_slot
            // itself spawns its own pump thread; this just queues the
            // request without blocking ImGui.
            HANDLE t = CreateThread(nullptr, 0, &direct_load_slot1_worker,
                                    nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }

        section_end(open);
    }

    if (bool open = section_begin("[experimental] Coop spawn probe (Phase 0C SHIPPED)")) {
        heading_with_info("Spawn a wilbur entity via the engine factory + clean teardown",
            "Phase 0C derisk experiment — SHIPPED + GREEN (2026-05-11).\n\n"
            "Calls entity_factory_construct (0x5B96F0) from the mod with "
            "the engine's exact wilbur bag = "
            "{model_name=avatars/wilbur_low, class=wilbur} "
            "(captured via Phase 0C-step-2g KV walker), observes 9 "
            "breadcrumb hooks fire, then tears down the orphan via "
            "vtable[0](entity, 1) (Phase 0C-step-2k) so the engine stays "
            "stable. Test loop confirms end-to-end pass.\n\n"
            "Pre-conditions:\n"
            "  - Be IN-GAME (not main menu / game-select / loading screen). "
            "The factory expects an active scene + level loaded.\n\n"
            "What gets logged (mtr-asi.log):\n"
            "  - Factory return pointer (null = registry miss / ctor refused)\n"
            "  - All 9 breadcrumb hits along the construction ladder "
            "(reg, merge, validate1, transform, actor_init, bbd10, "
            "register_active, post_init)\n"
            "  - Transform-list delta at dword_724DE4 (delta=1 = active "
            "sim path, delta=0 = queued/orphan)\n"
            "  - STEP2K teardown line: 'vtable[0](entity, 1) returned "
            "cleanly' (or 'EXCEPTION inside vtable[0]' if dtor faults).\n\n"
            "What this does NOT do (= Phase 2 work):\n"
            "  - No networking. No input binding.\n"
            "  - No PERSISTENT orphan — the entity is destroyed before the "
            "next sim tick. Keep-alive crashes ~150ms later inside "
            "sub_5CB160 (future/promise resolver) on a DIFFERENT entity — "
            "our orphan disturbs shared scene state. See Phase 0C-step-2j "
            "VEH capture in breadcrumb-trail.md.\n\n"
            "Decision context: research/findings/coop-phase-0a-audit-"
            "2026-05-10.md (Option A from both audits). Phase 1 transport "
            "(UDP) is the next coop milestone after this.");
        ImGui::Spacing();

        const auto last = mtr::coop_spawn_probe::last_result();

        if (ImGui::Button("Run probe (one-shot)##coop_probe_run")) {
            mtr::coop_spawn_probe::try_spawn_p2();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Calls factory once. Check mtr-asi.log.");

        ImGui::Spacing();
        if (!last.attempted) {
            ImGui::TextDisabled("No attempt yet this session.");
        } else {
            ImGui::Text("Last attempt:");
            if (last.succeeded) {
                ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                    "  factory returned %p (then torn down)", last.entity);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                    "  factory FAILED");
            }
            ImGui::Text("  screen at attempt: \"%s\"", last.screen_name);
            ImGui::Text("  transform list: %d -> %d (delta %+d)",
                        last.list_count_before, last.list_count_after,
                        last.list_delta);
            ImGui::Text("  bag slots after init: [%p, %p, %p]",
                        last.slot0_after_init, last.slot1_after_init,
                        last.slot2_after_init);
            ImGui::Text("  sub_55AF00 reached: %s (v13=%p)",
                        last.post_init_reached ? "yes" : "no",
                        last.post_init_v13_arg);
            ImGui::TextWrapped("  msg: %s", last.message);
        }

        section_end(open);
    }

}

} // namespace mtr::menu::detail
