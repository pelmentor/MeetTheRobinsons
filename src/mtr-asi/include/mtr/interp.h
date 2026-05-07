// Render interpolation infrastructure (M2 of high-fps decouple plan).
//
// Captures consecutive sim ticks' view + world matrices into prev/curr
// double buffer, computes interpolation alpha each render frame, and
// detects sim-tick cuts (large pos / rotation deltas → no interp).
//
// This module is intentionally just the plumbing. M3 (view interp), M4
// (Wilbur transform interp), M5 (NPC interp) all read from prev/curr and
// alpha here. Until those clients exist, the globals are not modified —
// the snapshots are just observed for telemetry.
//
// Threading: snapshots are written from camera_apply_all_active's POST
// hook (render thread) and read from per-render-frame interp clients
// (also render thread). All on one thread; no locking needed beyond
// std::atomic publish-via-counter for the snapshot index.

#pragma once

#include <cstdint>

namespace mtr::interp {

// Engine view + world matrix pair, copied verbatim from the D3D globals
// (0x724C10 = view, 0x724C50 = world). 16 floats each = 64 bytes per
// matrix; matches D3DMATRIX layout (row-major). qpc records the QPC
// timestamp at which the snapshot was captured (== POST camera_apply_all_active
// of the iteration whose sim tick produced the underlying entity state).

struct Snapshot {
    float    view[16];     // matches global at 0x724C10
    float    world[16];    // matches global at 0x724C50
    uint64_t qpc;          // captured at POST camera_apply_all_active
    bool     valid;        // false until first capture
};

// Install camera_apply_all_active POST hook. Idempotent.
void install();

// Called from sim_decouple's hk_sim_aggregator AFTER the orig sim has
// run. Marks "curr needs refresh on next camera_apply" so the snapshot
// shifts one slot when the next render frame's camera_apply fires.
//
// When sim is throttle-skipped this is NOT called → next camera_apply
// keeps curr unchanged → prev stays at last sim's value, curr stays at
// last sim's value, alpha grows up to 1.0.
void on_sim_tick_post();

// Read accessors for clients (M3+ interp implementations). Both return
// references to internal storage; caller must not mutate.

const Snapshot& prev();
const Snapshot& curr();

// Alpha: how far through the current sim window we are. Computed each
// render frame as clamp((now - curr.qpc) / sim_step, 0.0, 1.0). Returns
// 1.0 when the snapshot infrastructure is not ready, so non-interp
// clients see "freshest sim" semantics by default.
float current_alpha();

// True when both prev and curr are valid (captured at least 2 distinct
// sim ticks since install).
bool has_two_snapshots();

// Cut detection (M2.2). Returns true for the current render frame if
// the curr<->prev delta exceeds the configured thresholds. Clients use
// this to skip interp for that frame (snap to curr instead of lerp).
bool is_cut_detected();

// Threshold tuning. Defaults: 5.0 world units of translation, 30 deg of
// rotation between consecutive sim ticks → cut. Live-tunable from UI.
float cut_translation_threshold();
void  set_cut_translation_threshold(float v);
float cut_rotation_threshold_deg();
void  set_cut_rotation_threshold_deg(float deg);

// Diagnostics (UI surface). Cumulative since install.
uint64_t snapshots_taken();
uint64_t cuts_detected();

// View interp client (M3.1). When enabled, writes
// lerp+slerp(prev.view, curr.view, alpha) into globals 0x724C10/0x724C50
// at POST camera_apply_all_active, AFTER the snapshot capture. Skipped
// when is_cut_detected() or has_two_snapshots() is false.
//
// Default OFF — user opts in via UI. Engine doesn't read globals after
// EndScene, so no PRE/POST fence is needed; the next render frame's
// camera_apply naturally overwrites globals.

bool view_interp_enabled();
void set_view_interp_enabled(bool on);

// Cumulative count of frames where the view-interp write fired.
uint64_t view_interp_writes();

// M3.2 — aim-mode snap (input-bound).
//
// Engine-side aim detection (`SetPathCamTargetingBehavior` script command
// callback) lives in SecuROM-thunked code and isn't statically traceable.
// As a pragmatic substitute we poll a user-chosen Windows VK code each
// render frame; while the key is held, view + player + NPC interp clamp
// alpha=1.0 (snap to freshest sim, zero interp latency). Default key:
// VK_RBUTTON (right mouse button). Toggleable + key-rebindable from UI.

bool aim_snap_enabled();
void set_aim_snap_enabled(bool on);
int  aim_snap_vk();                 // Windows virtual-key code
void set_aim_snap_vk(int vk);
bool aim_snap_active();             // true on render frames where key was down

// Player transform interp client (M4) ---------------------------------------
//
// Snapshots the player entity's pos (offset +0x58, 12 bytes) + rotation
// 3x3 (offset +0x70, 36 bytes) at POST simulation_tick_aggregator. PRE
// next sim aggregator: restore saved pre-interp values so sim's reads
// are clean (M2.3 fence pattern). POST camera_apply_all_active: save +
// write lerp+slerp(prev, curr, alpha) into the entity.
//
// The player entity is found via entity_lookup_by_name_retry("player", 1)
// and cached. Re-lookup on null or after ~60 frames (cheap; level-load
// invalidates).

bool player_interp_enabled();
void set_player_interp_enabled(bool on);
uint64_t player_interp_writes();

// Called from sim_decouple's hk_sim_aggregator. Order:
//   PRE  orig: pre_sim_restore_player()    -- undo last frame's interp write
//   POST orig: post_sim_capture_player()   -- snapshot the freshest sim state
void pre_sim_restore_player();
void post_sim_capture_player();

// M4.3 teleport detection. Same idea as cut detection but per-entity:
// translation delta > 10.0 units between consecutive sim ticks → snap
// (no interp this window).
float player_teleport_threshold();
void  set_player_teleport_threshold(float v);
uint64_t player_teleports_detected();

// NPC transform interp client (M5) ------------------------------------------
//
// Walks the engine's per-frame transform list head `dword_724DE4` (a
// non-encrypted singly-linked list) and snapshots pos+rot for each node.
// Reuses M4's player-style fence pattern but per-entity, hashed by the
// node's inner-transform pointer (`*(node+92)`). Skips the player (already
// covered by M4) and any node with the engine's "skip transform" flag.
//
// Storage cap: kMaxNpcSlots fixed-size array. Hash collisions linear probe;
// stale slots aged out after kStaleSlotsAgeoutFrames sim ticks of absence.

bool npc_interp_enabled();
void set_npc_interp_enabled(bool on);
uint64_t npc_interp_writes();         // cumulative entity-frames written
uint64_t npc_active_slots();          // currently tracked entities
uint64_t npc_teleports_detected();

} // namespace mtr::interp
