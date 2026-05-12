// NPC visual debug overlay — projection of NPC data (name, pos, distance,
// later: animation state, registry properties) as text labels at each
// NPC's world position via ImGui's foreground draw list.
//
// Sister module to mtr::trigger_overlay. Shares projection math via
// include/mtr/overlay_math.h. Same lifecycle constraints: tick() must
// run inside mtr::menu::on_end_scene, after ImGui::NewFrame and before
// ImGui::Render.
//
// See research/findings/npc-overlay-plan-2026-05-09.md (v2) for the
// full plan and audit history.

#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace mtr::npc_overlay {

bool enabled();
void set_enabled(bool v);
int  visible_npc_count();   // last-frame count, for UI status

// Per-field show toggles. Each field is rendered conditionally; cheap
// when off (skipped per-NPC at render time).
bool show_name();         void set_show_name(bool v);
bool show_pos();          void set_show_pos(bool v);
bool show_distance();     void set_show_distance(bool v);

// Distance fade — labels fade out beyond this distance (engine units).
// 0 = always opaque.
float distance_limit();   void set_distance_limit(float v);

// Autonomous-validation export (mirrors trigger_overlay::set_export_frames).
// When N > 0, the next N tick()s emit machine-parseable
// NPC_OVERLAY_FRAME_BEGIN / VIEW / PROJ / NPC / FRAME_END log lines.
void set_export_frames(int n);
int  export_frames_remaining();

// Called from INSIDE mtr::menu::on_end_scene, after ImGui::NewFrame()
// and before ImGui::Render(). GetForegroundDrawList() requires a live
// frame.
void tick(IDirect3DDevice9* dev);

} // namespace mtr::npc_overlay
