// Private header for the menu module's split translation units.
//
// menu.cpp split into menu_helpers / menu_fps_overlay / tab_* under
// `mtr::menu::detail`. Symbols here are NOT part of the public mtr::menu
// API exposed elsewhere in the codebase — those live in the public
// `namespace mtr::menu { ... }` block in menu.cpp (is_visible / shutdown /
// on_end_scene / etc.). This header only ships the internal contracts the
// split files need to call into each other.
//
// Lives in src/, intentionally not in include/, because it is internal to
// the menu translation units.

#pragma once

#include "imgui.h"

namespace mtr::menu::detail {

// === Section + button helpers (menu_helpers.cpp) ===========================
// All tab functions consume these for consistent styling. Reserve the
// coloured buttons (primary/warning/danger) for the *primary* user-facing
// action of a section; default ImGui blue stays for everything else.

bool section_begin(const char* title);
void section_end(bool was_open);

// "(?)" tooltip pill — hover reveals the technical detail. RULE: never
// paste paragraph-length engineering text into the visible UI. Use this.
void info_pill(const char* tooltip_text);

// One-line section sub-heading + pill. Call AT MOST ONCE per section,
// immediately after section_begin().
void heading_with_info(const char* title, const char* details);

bool primary_button(const char* label, ImVec2 size = ImVec2(0, 0));
bool warning_button(const char* label, ImVec2 size = ImVec2(0, 0));
bool danger_button (const char* label, ImVec2 size = ImVec2(0, 0));

// === FPS overlay (menu_fps_overlay.cpp) ====================================
// Corner-pinned non-interactive window. State lives entirely in the
// overlay TU; menu_core just calls the draw routine after ImGui::NewFrame()
// when enabled() is true.

bool fps_overlay_enabled();
void set_fps_overlay_enabled(bool on);
int  fps_overlay_corner();           // 0=TL, 1=TR, 2=BL, 3=BR
void set_fps_overlay_corner(int corner);
void draw_fps_overlay();

// === Tabs (tab_*.cpp) ======================================================
// 6-tab order in the InsertMenu: Camera, Picture, World, Performance, Tools, Debug.

void tab_camera();
void tab_picture();      // Borderless + 3D aspect + HUD aspect + FPS limit + FPS overlay
void tab_world();
void tab_performance();  // High-FPS smoothing block (dt-correctness + decouple + interp)
void tab_tools();        // console + screenshot + cursor speed + cvar dump + status
void tab_debug();        // [advanced] sprite override + [diagnostic] sprite probe

// === Module state accessors (menu.cpp) =====================================
// Tabs occasionally need the underlying device / window for diagnostic
// readouts. These accessors stay in menu.cpp so the state ownership doesn't
// leak across TUs — tabs read, menu.cpp writes.

void* current_device();
void* current_hwnd();

} // namespace mtr::menu::detail
