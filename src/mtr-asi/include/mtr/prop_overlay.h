// Prop visual debug overlay — labels at world props (disassembleable /
// scannable / targetable / climbable / push-pullable / levitate / lock-
// to-path / superstep-target). Sister to mtr::npc_overlay; reuses the
// projection scaffold + autonomous validation pattern.
//
// "Death" of a prop (e.g. when Wilbur's disassembler destroys it) is
// handled FREE: the engine removes the entity from the transform list
// at dword_724DE4. The overlay re-walks the list every frame, so a
// dead prop simply doesn't get drawn. No engine-side death event hook.
//
// See research/findings/prop-overlay-plan-2026-05-10.md (v2) for the
// full plan and audit history.

#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace mtr::prop_overlay {

bool enabled();
void set_enabled(bool v);
int  visible_prop_count();   // last-frame count, for UI status

// Per-tag visibility filters. Each is checked per-entity per-frame via
// kv-get (cheap when the entity doesn't have the property).
//
// Phase 1 default: only `disassembleable` ON. Other tags ship as
// Phase 2 once profiling confirms 8-key-per-entity overhead is fine.
bool show_disassembleable();   void set_show_disassembleable(bool v);
bool show_scannable();         void set_show_scannable(bool v);
bool show_targetable();        void set_show_targetable(bool v);
bool show_climbable();         void set_show_climbable(bool v);
bool show_push_pullable();     void set_show_push_pullable(bool v);
bool show_levitate();          void set_show_levitate(bool v);
bool show_lock_to_path();      void set_show_lock_to_path(bool v);
bool show_ss_target();         void set_show_ss_target(bool v);

// Per-prop label fields.
bool show_name();              void set_show_name(bool v);
bool show_pos();               void set_show_pos(bool v);
bool show_distance();          void set_show_distance(bool v);
bool show_tags();              void set_show_tags(bool v);

// Distance fade (engine units). 0 = always opaque.
float distance_limit();        void set_distance_limit(float v);

// Autonomous-validation export. When N > 0, the next N tick()s emit
// machine-parseable PROP_OVERLAY_FRAME_BEGIN / PROP / FRAME_END log
// lines plus a per-frame walk-time microseconds reading.
void set_export_frames(int n);
int  export_frames_remaining();

// Called from INSIDE mtr::menu::on_end_scene, after npc_overlay::tick()
// (so prop labels draw on top of NPC labels — disassembly targets are
// the high-value signal in this overlay) and before ImGui::EndFrame().
void tick(IDirect3DDevice9* dev);

} // namespace mtr::prop_overlay
