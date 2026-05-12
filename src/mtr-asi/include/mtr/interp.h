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

// Camera-world-space view interp toggle. When ON (default), the view
// matrix is interpolated by extracting the camera's world position from
// each snapshot, lerping the world position, and reconstructing the
// translation row from interp_R + interp_cam_pos. This is mathematically
// correct: lerp(view_matrix0, view_matrix1) ≠ inv(lerp(camera0, camera1)).
// The visible difference is sub-pixel at typical 60Hz sim → 240Hz render
// but matters for fast camera orbits. Toggle off to A/B test against the
// older direct-matrix-lerp behaviour.
bool view_interp_camspace();
void set_view_interp_camspace(bool on);

// Halo follow-fix client (M3.3) --------------------------------------------
//
// Hooks HaloComponent::Update (sub_6678D0 @0x6678D0, vtable[+4] of vtable
// 0x6DD400) PRE-orig. While the hook fires, temporarily overrides the
// camera struct's cached view-projection matrix at +0x110 with a
// view-interp'd version, calls orig (which projects all halos through
// the interp'd VP), then restores. This eliminates the "halo offset
// from tagged entity, modulating with camera rotation" bug that view-
// interp causes — halos use the same interp'd view that the geometry
// renderer uses.
//
// Default OFF — gated alongside view_interp. Auto-vetoed when view-
// interp itself is OFF or sim_decouple is mini-game-vetoed. When the
// gate fails the hook trampolines straight to orig with no override.

bool halo_interp_enabled();
void set_halo_interp_enabled(bool on);

// Cumulative count of render frames where the halo-VP override fired.
uint64_t halo_interp_writes();

// Cumulative count of render frames where the halo hook bypassed the
// override (feature off, no snapshots, cut detected, mini-game mode,
// camera struct unresolved). Diagnostics only.
uint64_t halo_interp_skips();

// M3.2 — aim-mode snap (input-bound).
//
// Engine-side aim detection (`SetPathCamTargetingBehavior` script command
// callback) is registered from the script command-table region — plain
// code, but we hadn't completed that RE pass when this client shipped.
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

// Player handle staleness diagnostics. If staleness is real, the
// "char janky/blurred" report could be partly explained by writing
// interp values to a freed-then-reused entity slot.
//
// player_handle_swaps: count of cache-pointer changes since install.
// Should be small in steady state — non-zero on level transitions /
// mode swaps (Wilbur ↔ MiniHamsterPlayer ↔ DigDug ↔ ChargeBall).
uint64_t player_handle_swaps();

// Force a fresh entity_lookup_by_name_retry on the next call to
// get_player(). Clears prev/curr snapshots if pointer changes. Useful
// for debugging mode-switch / level-transition issues from the menu.
void force_player_handle_refresh();

// Fence-violation diagnostic (M4 specific). Counts render frames
// where the entity's position differed from what we wrote between
// camera_apply-POST and next sim_aggregator PRE. Non-zero = something
// else in the engine modifies entity transforms in our supposed
// "interp window", invalidating the M4 save-write-restore fence.
//
// If this number grows continuously with player_interp ON, the M4
// approach is unsafe for this engine and we'd need a "shadow render"
// pattern (intercept the read sites instead of writing the entity).
uint64_t player_fence_violations();

// Cross-subsystem entity pose primitives -----------------------------------
//
// Used by coop/ (MtrRemotePlayer) to hold and apply target poses without
// linking against interp_internal.h. The struct layout matches the private
// `detail::PlayerSnap` byte-for-byte; the public wrappers just forward.
//
// Engine layout (verified): pos at +0x58 (12 bytes), rotation 3x3 at +0x70
// (36 bytes, row-major float[9]). Same offsets the M4 player interp uses.

struct EntityPose {
    float    pos[3];
    float    rot[9];
    uint64_t qpc;
    bool     valid;
};

// QueryPerformanceCounter sample using the cached frequency. Exposed so
// coop/ can stamp its snapshots without re-implementing the call.
uint64_t qpc_now();

// Read engine_entity's pos+rot under SEH. On success, fills `out` with
// pos/rot + qpc_now() + valid=true and returns true. On AV/fault, leaves
// `out` unchanged and returns false. Caller decides whether to drop the
// snapshot or hold the last good one.
bool capture_entity_pose(const void* engine_entity, EntityPose& out);

// Compose alpha-blend(prev, curr) and write the result into engine_entity's
// pos+rot fields. When prev.valid is false, snaps to curr (no blend). When
// curr.valid is false, this is a no-op. Caller is responsible for SEH if
// the entity pointer is potentially stale — this function does NOT wrap.
void apply_entity_pose_interp(const EntityPose& prev,
                              const EntityPose& curr,
                              float alpha,
                              void* engine_entity);

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
