// Camera tab: FreeCam controls + Field-of-view override.
//
// FreeCam is the most-used control in this menu, so it gets a prominent
// state-coloured full-width button at the top of the tab. The body sections
// for FreeCam pose / sliders only render while FreeCam is on, to keep the
// idle tab compact.

#include "menu_internal.h"
#include "imgui.h"

#include <cfloat>
#include <cstdint>

namespace mtr::freecam {
    bool  active();
    void  set_active(bool on);
    void  get_pose(float pos[3], float* yaw, float* pitch);
    float move_speed();           void set_move_speed(float v);
    float mouse_sens();           void set_mouse_sens(float v);
    void  request_teleport_to_camera();
    uint64_t mmb_teleport_writes();
    uint64_t mmb_teleport_skips();
    int      teleport_hold_frames_setting();
    void     set_teleport_hold_frames_setting(int frames);
    int      teleport_hold_remaining();
    uint64_t teleport_hold_writes();
    void     cancel_teleport_hold();
}
namespace mtr::fov {
    bool  has_override();
    float current();
    bool  set(float value);
}

namespace mtr::menu::detail {

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

    if (bool open = section_begin("Free camera")) {
        const bool fc_on = mtr::freecam::active();
        if (fc_on) {
            float pos[3]; float yaw, pitch;
            mtr::freecam::get_pose(pos, &yaw, &pitch);
            ImGui::Text("Position  %8.2f  %8.2f  %8.2f", pos[0], pos[1], pos[2]);
            ImGui::Text("Look      yaw %6.1f\xC2\xB0   pitch %6.1f\xC2\xB0",
                        yaw * 57.29578f, pitch * 57.29578f);
            ImGui::Spacing();

            float ms = mtr::freecam::move_speed();
            if (ImGui::SliderFloat("Fly speed##fcms", &ms, 1.0f, 500.0f, "%.1f u/s",
                                   ImGuiSliderFlags_Logarithmic)) {
                mtr::freecam::set_move_speed(ms);
            }
            float mouse = mtr::freecam::mouse_sens() * 1000.0f;
            if (ImGui::SliderFloat("Mouse sensitivity##fcsens", &mouse, 0.5f, 15.0f, "%.2f")) {
                mtr::freecam::set_mouse_sens(mouse / 1000.0f);
            }

            ImGui::Spacing();
            // Clickable button equivalent to pressing MMB while in free
            // camera. Shown only when free camera is active so it can't
            // accidentally fire during normal gameplay.
            if (ImGui::Button("Teleport player to camera (or press MMB)")) {
                mtr::freecam::request_teleport_to_camera();
            }
            ImGui::TextDisabled("  Teleports: %llu  skips: %llu",
                                static_cast<unsigned long long>(mtr::freecam::mmb_teleport_writes()),
                                static_cast<unsigned long long>(mtr::freecam::mmb_teleport_skips()));

            // Teleport hold — overrides the engine's snap-back. Without
            // this, entity_transform_tick (in sim_aggregator) re-writes
            // player+0x58 from anim samples on the next sim tick and
            // Wilbur snaps back to where he was, then the AI plays the
            // "on-edge keep balance" recovery anim. This re-writes the
            // pos in sim_aggregator POST for N frames after MMB.
            int hold_setting = mtr::freecam::teleport_hold_frames_setting();
            if (ImGui::SliderInt("Hold teleport for N sim ticks##fchold",
                                 &hold_setting, 0, 600,
                                 hold_setting == 0 ? "off (single shot)" : "%d ticks")) {
                mtr::freecam::set_teleport_hold_frames_setting(hold_setting);
            }
            info_pill(
                "After MMB teleport, the engine's entity_transform_tick "
                "(in sim_aggregator) overwrites player+0x58 from anim "
                "samples on the next sim tick — Wilbur visibly snaps back "
                "and the AI plays the 'on-edge keep balance' recovery. "
                "This setting re-writes the teleport pos in the sim "
                "aggregator POST hook (after entity_transform_tick has "
                "run, before render reads pos) for N more sim ticks. "
                "0 = single shot (snap-back behavior). 60 \xE2\x89\x88 1 sec at "
                "60Hz sim. Long values let you park Wilbur for camera "
                "shots; the AI's recovery anim still plays but pos sticks.");
            const int hold_rem = mtr::freecam::teleport_hold_remaining();
            if (hold_rem > 0) {
                ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                                   "  Hold active: %d ticks remaining (%llu re-writes total)",
                                   hold_rem,
                                   static_cast<unsigned long long>(mtr::freecam::teleport_hold_writes()));
                ImGui::SameLine();
                if (ImGui::SmallButton("Release##fchold_release")) {
                    mtr::freecam::cancel_teleport_hold();
                }
            } else {
                ImGui::TextDisabled("  Hold inactive (re-writes so far: %llu)",
                                    static_cast<unsigned long long>(mtr::freecam::teleport_hold_writes()));
            }
        } else {
            ImGui::TextDisabled("Free camera is off. Click the toggle above or press F3 in-game.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Controls:");
        ImGui::TextDisabled("  WASD - move    Space / C - up / down    Shift - 4x speed");
        ImGui::TextDisabled("  Mouse - look   Wheel - change fly speed");
        ImGui::TextDisabled("  Arrows - look (if no mouse)");
        ImGui::TextDisabled("  MMB - teleport player to camera (only while free camera is on)");
        ImGui::TextDisabled("Open Insert / F2 to use the menu - that frees the cursor for UI.");
        section_end(open);
    }

    if (bool open = section_begin("Field of view")) {
        const bool fov_on = mtr::fov::has_override();
        float fov_cur = mtr::fov::current();
        static float fov_ui = 90.0f;
        if (fov_on) fov_ui = fov_cur;
        ImGui::Text("%s   %.1f\xC2\xB0", fov_on ? "Custom FOV" : "Default (engine value)", fov_cur);
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

} // namespace mtr::menu::detail
