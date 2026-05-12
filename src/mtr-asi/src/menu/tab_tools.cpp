// Tools tab: engine console + screenshot + menu cursor speed + engine
// variable dump + Status. Utilities + actions; the per-feature settings
// for FPS / aspect / smoothing live in their own tabs.
//
// The "Status" section reads d3d device + window via the
// detail::current_device() / current_hwnd() accessors exported from
// menu.cpp, so this TU never needs a friend-style include of the menu
// core's anonymous-namespace state.

#include "menu_internal.h"
#include "imgui.h"

#include <windows.h>
#include <cstdint>

namespace mtr::log        { void info(const char* fmt, ...); }
namespace mtr::d3d9hook   { int last_pp_width(); int last_pp_height(); }
namespace mtr::screenshot {
    void        request();
    const char* last_path();
    std::uint64_t last_path_tag();
}
namespace mtr::cvar_dump {
    size_t count();
    bool   dump_default();
}
namespace mtr::console {
    bool is_visible();
    void set_visible(bool on);
}
namespace mtr::dinput_hook {
    void set_virt_scale_pct(int percent);
    int  virt_scale_pct();
}

namespace mtr::menu::detail {

void tab_tools() {
    if (bool open = section_begin("Engine console")) {
        bool con_vis = mtr::console::is_visible();
        if (ImGui::Checkbox("Show engine console (or press F2)", &con_vis)) {
            mtr::console::set_visible(con_vis);
        }
        ImGui::SameLine();
        info_pill(
            "Engine command prompt. Output mirrored to mtr-asi.log. "
            "Try: `context view`, `list`, `set Verbosity debug`.");
        section_end(open);
    }

    if (bool open = section_begin("Screenshot")) {
        if (ImGui::Button("Capture now (or press F12)", ImVec2(220.0f, 0.0f))) {
            mtr::screenshot::request();
        }
        ImGui::SameLine();
        if (mtr::screenshot::last_path_tag() > 0) {
            ImGui::TextWrapped("Last: %s", mtr::screenshot::last_path());
        } else {
            ImGui::TextDisabled("Captures the current view (including the menu).");
        }
        section_end(open);
    }

    if (bool open = section_begin("Menu cursor speed")) {
        heading_with_info("Cursor sensitivity in this menu",
            "Speed of the in-menu cursor when the Insert menu (or freecam) "
            "consumes mouse input. Independent of the OS pointer-speed slider "
            "and the in-game look sensitivity.\n\n"
            "Technical: scales the raw DirectInput mickey deltas the game "
            "reports each poll (DIMOUSESTATE.lX/lY) before integrating into "
            "the virtual cursor used by ImGui. 100 = passthrough (1.0x), "
            "200 = 2.0x faster, 50 = 0.5x slower. Sub-pixel residue is "
            "carried so values < 100 don't quantise small movements away.");
        int pct = mtr::dinput_hook::virt_scale_pct();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt("Speed##menu_cursor_speed", &pct, 25, 400, "%d %%")) {
            mtr::dinput_hook::set_virt_scale_pct(pct);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##menu_cursor_speed_reset")) {
            mtr::dinput_hook::set_virt_scale_pct(75);
        }
        section_end(open);
    }

    if (bool open = section_begin("Engine variable dump")) {
        heading_with_info("List every engine variable to a text file",
            "Captures every variable the engine registers and writes them "
            "to mtr_cvars.txt next to Wilbur.exe. Useful for finding new "
            "optimization knobs - search the file for lod, cull, draw, "
            "fade, occlud, sector, etc.");
        ImGui::Spacing();
        ImGui::Text("Registered so far: %zu", mtr::cvar_dump::count());
        ImGui::Spacing();
        if (ImGui::Button("Dump variables to mtr_cvars.txt", ImVec2(260.0f, 0.0f))) {
            bool ok = mtr::cvar_dump::dump_default();
            mtr::log::info("cvar_dump: dump button -> %s", ok ? "ok" : "FAILED");
        }
        section_end(open);
    }

// FPS limiter + FPS overlay live in menu/tab_picture.cpp under mtr::menu::detail::.

// High-FPS smoothing lives in menu/tab_performance.cpp under mtr::menu::detail::.

    if (bool open = section_begin("Status")) {
        ImGui::Text("D3D device:  %p", current_device());
        ImGui::Text("Window:      %p", current_hwnd());
        ImGui::Text("Render size: %d x %d",
                    mtr::d3d9hook::last_pp_width(), mtr::d3d9hook::last_pp_height());
        if (HWND h = static_cast<HWND>(current_hwnd())) {
            RECT r{};
            if (GetClientRect(h, &r)) {
                ImGui::Text("Window size: %ld x %ld", r.right - r.left, r.bottom - r.top);
            }
        }
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Hotkeys: Insert (this menu)  F2 (console)  F3 (free camera)  F12 (screenshot)");
        section_end(open);
    }
}

} // namespace mtr::menu::detail
