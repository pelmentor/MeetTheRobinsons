// Trigger box overlay — 3D-projected debug visualization of trigger volumes
// (and other axis-aligned bounding boxes). Draws via ImGui's foreground
// draw list using the engine's view + projection matrices.
//
// See research/findings/trigger-box-overlay-plan-2026-05-09.md (v2) for
// the full plan, audit history, and phase gates.

#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct _D3DMATRIX;
typedef struct _D3DMATRIX D3DMATRIX;

namespace mtr::trigger_overlay {

bool enabled();
void set_enabled(bool v);
int  visible_box_count();   // last-frame count, for UI status

// Phase 1 + 2: hardcoded test box at (0,0,0) extents (10,10,10).
// Set true to draw it for visual validation of the projection scaffold.
bool show_test_box();
void set_show_test_box(bool v);

// Autonomous validation export: when set to N > 0, the next N rendered
// frames emit machine-parseable TRIGGER_OVERLAY_* log lines containing
// the view+proj matrices, the box parameters, and each surviving edge's
// screen-space endpoints. An offline validator (tools/validate-overlay-
// frames.ps1) re-runs the projection math on those matrices and asserts
// the logged screen coordinates match within a fixed tolerance — closes
// Phase 1 acceptance without needing eyes on the screen.
//
// Decrements once per rendered frame; auto-disarms at zero. No-op when
// the overlay is disabled.
void set_export_frames(int n);
int  export_frames_remaining();

// Called from inside mtr::menu::on_end_scene, AFTER ImGui::NewFrame() and
// BEFORE ImGui::Render(). GetForegroundDrawList() requires an in-progress
// frame.
void tick(IDirect3DDevice9* dev);

} // namespace mtr::trigger_overlay
