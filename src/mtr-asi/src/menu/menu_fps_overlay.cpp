// Corner-pinned, non-interactive FPS overlay.
//
// Reads io.Framerate (ImGui's smoothed 1s EMA — already updated every frame
// in NewFrame, so always available). Pins a no-decoration window to one of
// the four screen corners with an 8px margin from the work-area edge.
//
// Decouple-aware: when the sim_decouple module is in THROTTLE mode (or the
// detailed-log toggle is on), the overlay expands to show structured
// RENDER / LOGIC / BLEND lines + per-system status flags. Otherwise it
// stays minimal (one-line legacy display) so users who don't care about
// decouple don't see extra clutter.

#include "menu_internal.h"
#include "imgui.h"
#include "mtr/sim_decouple.h"
#include "mtr/interp.h"

#include <atomic>

namespace mtr::menu::detail {

namespace {

// Default ON — the overlay is non-interactive and the cost is a single
// always-visible widget; useful for any user verifying performance.
std::atomic<bool> g_enabled{true};
std::atomic<int>  g_corner{0};   // 0=TL, 1=TR, 2=BL, 3=BR

} // namespace

bool fps_overlay_enabled()              { return g_enabled.load(); }
void set_fps_overlay_enabled(bool on)   { g_enabled.store(on); }
int  fps_overlay_corner()               { return g_corner.load(); }
void set_fps_overlay_corner(int corner) { g_corner.store(corner); }

void draw_fps_overlay() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const int corner = g_corner.load();
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

            if (s_hz > 0.001) {
                ImGui::Text("LOGIC:   %6.1f Hz  %5.2f ms  (target %d)",
                            s_hz, static_cast<float>(1000.0 / s_hz), tgt);
            } else {
                ImGui::Text("LOGIC:      --- Hz   ---  ms  (target %d)", tgt);
            }

            const float interp_alpha = mtr::interp::current_alpha();
            const bool  cut          = mtr::interp::is_cut_detected();
            (void)alpha;
            ImGui::Text("BLEND:   %5.3f%s", interp_alpha, cut ? "  [CUT]" : "");

            auto draw_flag = [](const char* label, bool on) {
                ImGui::SameLine();
                ImGui::TextColored(on ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f)
                                      : ImVec4(0.55f, 0.55f, 0.6f, 1.0f),
                                   on ? "%s:ON" : "%s:off", label);
            };
            ImGui::Text("HIGH-FPS:");
            draw_flag("LOCK", mtr::sim_decouple::is_throttling());
            draw_flag("CAM",  mtr::interp::view_interp_enabled());
            draw_flag("PLR",  mtr::interp::player_interp_enabled());
            draw_flag("NPC",  mtr::interp::npc_interp_enabled());
            draw_flag("AIM",  mtr::interp::aim_snap_active());
        }
    }
    ImGui::End();
}

} // namespace mtr::menu::detail
