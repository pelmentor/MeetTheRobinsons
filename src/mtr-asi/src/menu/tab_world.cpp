// World tab: draw distance, periphery culling, mesh-LOD distances,
// global actor LOD scale, fog disable.
//
// All controls write into the live engine globals/cvars exposed by
// mtr::draw_dist, mtr::lod, mtr::scene. They take effect immediately.

#include "menu_internal.h"
#include "imgui.h"

#include <cstdint>

namespace mtr::draw_dist {
    bool  has_override();
    float current();
    bool  set(float value);
}
namespace mtr::peripheral_cull_probe {
    uint32_t force_pass_plane_mask();
    void     set_force_pass_plane_mask(uint32_t mask);
}
namespace mtr::scene {
    bool fog_disabled();         void set_fog_disabled(bool v);
    bool no_backface_cull();     void set_no_backface_cull(bool v);
}
namespace mtr::cmdline {
    bool snow_at_boot();
    void set_snow_at_boot(bool v);
}
namespace mtr::lod {
    float lod_scale();              void set_lod_scale(float v);
    float focus_dist();             void set_focus_dist(float v);
    float high_dist();              void set_high_dist(float v);
    float medium_dist();            void set_medium_dist(float v);
}
namespace mtr::msaa {
    bool     enabled();             void     set_enabled(bool v);
    uint32_t sample_count();        void     set_sample_count(uint32_t n);
    uint32_t actual_count();        uint32_t actual_quality();
}

namespace mtr::menu::detail {

void tab_world() {
    if (bool open = section_begin("Draw distance")) {
        heading_with_info("How far the camera draws geometry",
            "Pushes back the camera's far cull plane so distant geometry "
            "renders. Engine default is 1500 units. Higher = see more, more "
            "stuff drawn (lower FPS). Off = use engine default.\n\n"
            "Type a value into the input box and press Enter to apply, or "
            "click a preset. The engine also runs a SEPARATE per-object "
            "frustum cull at g_cull_frustum (0x726498) — the 'Infinite' "
            "toggle below force-passes its near + far planes so distant "
            "geometry actually survives both gates.");
        ImGui::Spacing();

        const bool  dd_on  = mtr::draw_dist::has_override();
        const float dd_cur = mtr::draw_dist::current();

        ImGui::Text("Current:");
        ImGui::SameLine();
        ImGui::TextColored(dd_on ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f)
                                 : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           dd_on ? "Override %.1f" : "Default (engine value 1500)", dd_cur);
        ImGui::Spacing();

        // Typed input — applies on Enter or focus loss. NO auto-sync from
        // dd_cur (the previous version reset dd_ui to dd_cur every frame,
        // which fought the user's typing).
        static float dd_ui = 5000.0f;
        if (ImGui::InputFloat("Distance (units)##dd_typed", &dd_ui, 100.0f, 1000.0f, "%.1f",
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (dd_ui < 0.0f) dd_ui = 0.0f;
            mtr::draw_dist::set(dd_ui);
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply##dd_apply")) {
            if (dd_ui < 0.0f) dd_ui = 0.0f;
            mtr::draw_dist::set(dd_ui);
        }
        ImGui::Spacing();

        ImGui::TextDisabled("Presets (click to apply directly):");
        if (ImGui::Button("3k##dd"))    { dd_ui = 3000.0f;     mtr::draw_dist::set(dd_ui); } ImGui::SameLine();
        if (ImGui::Button("10k##dd"))   { dd_ui = 10000.0f;    mtr::draw_dist::set(dd_ui); } ImGui::SameLine();
        if (ImGui::Button("50k##dd"))   { dd_ui = 50000.0f;    mtr::draw_dist::set(dd_ui); } ImGui::SameLine();
        if (ImGui::Button("100k##dd"))  { dd_ui = 100000.0f;   mtr::draw_dist::set(dd_ui); } ImGui::SameLine();
        if (ImGui::Button("1M##dd"))    { dd_ui = 1000000.0f;  mtr::draw_dist::set(dd_ui); } ImGui::SameLine();
        if (ImGui::Button("10M##dd"))   { dd_ui = 10000000.0f; mtr::draw_dist::set(dd_ui); } ImGui::SameLine();
        if (ImGui::Button("Off##dd"))   { mtr::draw_dist::set(0.0f); }
        ImGui::Spacing();

        // Engine-side per-object frustum cull also runs a far-plane test
        // against g_cull_frustum (a different struct from the per-camera
        // projection cache the slider above writes). For TRULY infinite
        // draw distance we also need to neutralize its gated near + far
        // planes (slots 1 and 6).
        constexpr uint32_t kCullFarMask = (1u << 1) | (1u << 6);
        const uint32_t cur_mask = mtr::peripheral_cull_probe::force_pass_plane_mask();
        bool inf_dd = (cur_mask & kCullFarMask) == kCullFarMask;
        if (ImGui::Checkbox("Infinite (also bypass cull-frustum near + far)##dd_inf", &inf_dd)) {
            const uint32_t cur = mtr::peripheral_cull_probe::force_pass_plane_mask();
            mtr::peripheral_cull_probe::set_force_pass_plane_mask(
                inf_dd ? (cur | kCullFarMask) : (cur & ~kCullFarMask));
            // Pair with a stupidly large draw_dist so D3D-side projection
            // far is also pushed out, on FIRST enable only — don't stomp
            // on a custom value the user has already typed.
            if (inf_dd && !mtr::draw_dist::has_override()) {
                dd_ui = 1.0e7f;
                mtr::draw_dist::set(dd_ui);
            }
        }
        info_pill("Toggles force-pass on planes 1 + 6 of g_cull_frustum (the "
            "gated near + far slots). On first enable, also bumps the per-"
            "camera draw_dist override to 10,000,000 if you don't already "
            "have a custom value set.\n\n"
            "If geometry near the camera vanishes when this is on, plane 1 "
            "is being treated as 'near' and force-passing it broke clipping. "
            "Use the per-plane toggles in Insert > Debug > Peripheral cull "
            "probe to find which plane is actually far for your scene.");
        section_end(open);
    }

    if (bool open = section_begin("Anti-aliasing (MSAA)")) {
        heading_with_info("Smooth jagged edges via multi-sample AA",
            "Multi-sample anti-aliasing reduces jagged edges on polygon "
            "boundaries by sampling each pixel several times and averaging. "
            "Implemented at the D3D9 device-creation level: the present-"
            "params MultiSampleType is set to the requested count and the "
            "device is recreated. Under DXVK (Wilbur's runtime path), this "
            "maps to native Vulkan VK_SAMPLE_COUNT_xx_BIT \xE2\x80\x94 hardware "
            "multisample on the GPU, not a post-process.\n\n"
            "Cost: roughly linear in sample count. 4x is the typical sweet "
            "spot; 8x and 16x cost more for diminishing returns. NONE = off.\n\n"
            "Requires a device reset to take effect: the new sample count "
            "is applied on the next CreateDevice or Reset call (alt-tab "
            "out + back is the simplest way to force one).");
        ImGui::Spacing();

        bool aa_on = mtr::msaa::enabled();
        if (ImGui::Checkbox("Enable MSAA##msaa_en", &aa_on)) {
            mtr::msaa::set_enabled(aa_on);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(applies on next device reset \xE2\x80\x94 alt-tab out + back)");

        ImGui::BeginDisabled(!aa_on);
        const uint32_t cur = mtr::msaa::sample_count();
        ImGui::Text("Sample count:");
        ImGui::SameLine();
        if (ImGui::RadioButton("2x##msaa_2",  cur == 2))  mtr::msaa::set_sample_count(2);  ImGui::SameLine();
        if (ImGui::RadioButton("4x##msaa_4",  cur == 4))  mtr::msaa::set_sample_count(4);  ImGui::SameLine();
        if (ImGui::RadioButton("8x##msaa_8",  cur == 8))  mtr::msaa::set_sample_count(8);  ImGui::SameLine();
        if (ImGui::RadioButton("16x##msaa_16", cur == 16)) mtr::msaa::set_sample_count(16);
        ImGui::EndDisabled();

        const uint32_t actual = mtr::msaa::actual_count();
        const uint32_t actual_q = mtr::msaa::actual_quality();
        if (actual > 0) {
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                "Active: %ux MSAA (quality %u)", actual, actual_q);
        } else if (aa_on) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                "Awaiting device reset (alt-tab out + back to apply)");
        } else {
            ImGui::TextDisabled("Active: off");
        }
        section_end(open);
    }

    if (bool open = section_begin("Mesh detail distances")) {
        heading_with_info("Distances at which mesh detail drops",
            "Engine reduces mesh detail past these distances. Defaults: "
            "Focus 100, High 250, Medium 500 units. Push them higher to "
            "see more detail farther away; lower for better FPS.");
        ImGui::Spacing();

        float focus  = mtr::lod::focus_dist();
        float high   = mtr::lod::high_dist();
        float medium = mtr::lod::medium_dist();
        ImGui::Text("Focus %.1f    High %.1f    Medium %.1f", focus, high, medium);
        if (ImGui::SliderFloat("Focus distance##fd",  &focus,  10.0f,  5000.0f, "%.1f"))  mtr::lod::set_focus_dist(focus);
        if (ImGui::SliderFloat("High distance##hd",   &high,   10.0f,  5000.0f, "%.1f"))  mtr::lod::set_high_dist(high);
        if (ImGui::SliderFloat("Medium distance##md", &medium, 10.0f, 10000.0f, "%.1f"))  mtr::lod::set_medium_dist(medium);
        section_end(open);
    }

    if (bool open = section_begin("Actor detail distance")) {
        heading_with_info("Global multiplier for character/actor detail distance",
            "ActorLOD.LODScale \xE2\x80\x94 global multiplier applied to actor LOD distance "
            "decisions. > 1.0 keeps higher detail farther; < 1.0 drops detail "
            "earlier. Engine default 1.0.");
        ImGui::Spacing();

        float ls = mtr::lod::lod_scale();
        ImGui::Text("Detail scale = %.3f", ls);
        if (ImGui::SliderFloat("Detail scale##ls", &ls, 0.1f, 10.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) {
            mtr::lod::set_lod_scale(ls);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to 1.0##ls_reset")) mtr::lod::set_lod_scale(1.0f);
        section_end(open);
    }

    if (bool open = section_begin("Fog")) {
        heading_with_info("Disable in-game fog",
            "Forces the engine's fog off. Useful for clearer distant view "
            "in scenes that fog out aggressively.");
        ImGui::Spacing();
        bool fd = mtr::scene::fog_disabled();
        if (ImGui::Checkbox("Disable fog", &fd)) mtr::scene::set_fog_disabled(fd);
        ImGui::SameLine();
        ImGui::TextColored(fd ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           fd ? "  Off (no fog)" : "  Default (engine fog)");
        section_end(open);
    }

    if (bool open = section_begin("Snow Easter egg")) {
        heading_with_info("Activate the -letitsnow boot flag",
            "Avalanche shipped a hidden cmdline flag '-letitsnow' wired into "
            "the engine's argv parser (the same parser that handles "
            "-dxwindowed / -dxfullscreen / -dxresolution). The flag string "
            "lives at 0xF003E4 in the runtime-decompressed code region; "
            "directly flipping the post-parse flag at runtime is hard "
            "because the parser uses register-indirect addressing in that "
            "region, so we use the engine's own activation path: inject "
            "'-letitsnow' into argv at boot via the GetCommandLineA/W hook.\n\n"
            "Setting persists in Game/mtr-asi-ui.ini under [Boot]letitsnow=1. "
            "Effect (per Disney/Avalanche convention): a snow weather effect "
            "in any level. NOT verified at runtime in this build — if it "
            "appears to do nothing, the flag may be vestigial / dead code in "
            "the retail binary.");
        ImGui::Spacing();
        bool snow = mtr::cmdline::snow_at_boot();
        if (ImGui::Checkbox("Enable snow at boot (requires restart)##snow_boot", &snow)) {
            mtr::cmdline::set_snow_at_boot(snow);
        }
        ImGui::SameLine();
        ImGui::TextColored(snow ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           snow ? "  Will inject -letitsnow on next launch"
                                : "  Disabled");
        section_end(open);
    }

    if (bool open = section_begin("Render both faces")) {
        heading_with_info("Render geometry from both sides (disable backface culling)",
            "Forces D3DRS_CULLMODE = D3DCULL_NONE on every draw. The "
            "engine's fixed-function and shader paths use clockwise winding "
            "for front faces, so anything wound away from the camera gets "
            "skipped by default — this option turns that off.\n\n"
            "Effect: see the inside of hollow objects, see the back of "
            "single-sided banners / decals, see geometry from underneath. "
            "Cost: roughly 2x triangle work for opaque geometry, plus "
            "potential z-fighting on degenerate two-sided surfaces.\n\n"
            "Note: this is purely a D3D-state filter — it has no effect on "
            "the engine's per-object frustum cull (use the peripheral cull "
            "probe in the Debug tab for that).");
        ImGui::Spacing();
        bool nb = mtr::scene::no_backface_cull();
        if (ImGui::Checkbox("Disable backface culling##nbf", &nb)) {
            mtr::scene::set_no_backface_cull(nb);
        }
        ImGui::SameLine();
        ImGui::TextColored(nb ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           nb ? "  Both faces drawn" : "  Default (front faces only)");
        section_end(open);
    }
}

} // namespace mtr::menu::detail
