// FreeCam - native, crutchless implementation.
//
// Hook point: camera_apply_all_active (sub_4C1E40), POST.
// camera_apply_all_active is called once per frame by render_frame_top_level
// (sub_4D22D0). Internally it iterates g_active_camera_array, calls each
// camera's per-camera apply (sub_4C1BA0), which writes:
//   *(outer+0x34) view-matrix-ptr -> g_d3d_view_matrix_global @ 0x724C10
//   matrix4_invert_affine(view) ->                              0x724C50  (world)
// Whichever camera is "active" (PathCam during gameplay, ScriptCam during
// scripted shots, deathcam after death, etc.) ends up writing those globals.
// Hooking PathCam_tick alone misses the others -- that's why hitting walls
// "locked" the camera (a transient ScriptCam took over and its view won).
//
// Doing it after camera_apply_all_active means: ALL camera classes' state
// stays coherent (engine ticks them, applies them, all internal logic runs),
// then we last-write to the SAME globals the engine just wrote. The render
// pipeline downstream of the apply (culling, frustum, scene render) reads
// those same globals -- our pose propagates everywhere.
//
// Coordinate system: RH (engine uses game_PerspectiveFovRH, verified). Y-up.
//   forward(world) = (-sin(yaw)*cos(pitch), sin(pitch), cos(yaw)*cos(pitch))
//   yaw=0 looks +Z; yaw+ turns toward camera-right. Pitch+ tilts up.
//
// Activation: F3 toggles. On first activation we seed pose from the engine's
// current view matrix so the camera doesn't snap to (0,0,0). Mouse-look is
// primary (cursor-recenter); arrows are fallback. Mouse wheel scales
// move_speed. MMB teleports the player entity (Wilbur) to the freecam
// position via entity_lookup_by_name_retry("player", 1) -> entity[+0x58].

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::freecam {

namespace {

// Engine global view/world matrices. Written by sub_4C1BA0 (per-camera apply)
// during camera_apply_all_active. Read by everything downstream of the apply
// (culling, frustum extraction, render). We last-write here in our POST-hook.
constexpr uintptr_t kViewMatrixGlobalVA  = 0x00724C10;
constexpr uintptr_t kWorldMatrixGlobalVA = 0x00724C50;

struct State {
    bool  active            = false;
    bool  pose_seeded       = false;
    float pos[3]            = { 0.0f, 0.0f, 0.0f };
    float yaw               = 0.0f;
    float pitch             = 0.0f;
    float move_speed        = 25.0f;
    float look_speed        = 1.5f;     // arrow-keys fallback (rad/s)
    float mouse_sens        = 0.003f;   // radians per pixel
    float fast_mult         = 4.0f;
    float min_speed         = 1.0f;
    float max_speed         = 2000.0f;
};

State            g_st;
std::mutex       g_mu;
// High-resolution dt source. GetTickCount64 (the previous source) has
// ~15.625ms resolution on Windows — at 240Hz render the dt sequence is
// 0,0,0,16ms,0,0,0,16ms which makes movement visibly choppy. QPC is
// ~100ns resolution and gives a smooth dt at any frame rate. Both
// counters are initialized lazily on first tick (g_qpc_freq=0 sentinel
// guards the first read so we never divide by zero). Single-thread use
// (only freecam::tick reads/writes them); std::atomic on the prev-tick
// reservoir keeps it consistent with the existing code style.
std::atomic<int64_t> g_qpc_freq{0};
std::atomic<int64_t> g_qpc_last{0};

// Mouse-look uses a "recenter to window center" approach: each tick we
// compute the client-area center in screen coords, take dx/dy = cursor -
// center, apply, then SetCursorPos back to center. Anchoring to a fixed
// position (set on first tick) had a fatal flaw -- in fullscreen-exclusive
// the OS can pin the cursor at screen edges, where movement in one
// direction is clipped to 0 and yaw/pitch can no longer rotate that way.
// Window-center recenter avoids the issue entirely.
bool              g_mouse_skip_first_delta = true;

// Set by menu/console when their UI is visible -- we should NOT grab the
// cursor for mouse-look in that case (let ImGui own it).
std::atomic<bool> g_ui_visible{false};

// Captured each PathCam_tick (set by d3d9_hook). Kept around for any
// future PathCam-side feature (e.g. snapping freecam pose to PathCam's
// follow target on activation); the MMB-teleport path no longer needs
// it now that we resolve the player via entity_lookup_by_name_retry.
std::atomic<void*> g_last_controller{nullptr};

// MMB latch: edge-triggered request set by the menu's input poll; consumed
// in tick().
std::atomic<int> g_pending_mmb{0};

// Player-entity lookup. Engine helper at 0x5AC8F0 is __thiscall, NOT
// __stdcall: it requires the entity manager pointer in ECX (loaded from
// the global at 0x7425AC) and reads the name + an unused-second-arg
// from the stack (`retn 8`). Calling without setting ECX walks an
// arbitrary linked list off whatever-garbage-is-in-ECX and either
// faults or stalls (we observed ~1s freezes from MMB teleports before
// this fix). All engine call sites do `mov ecx, dword_7425AC` before
// calling — that's the manager-pointer global verified across multiple
// callers (sub_402770, ai_resolve_nav_vector, sub_5034F0, etc.).
//
// Entity layout:
//   +0x48 = ptr to render sub-component (the model/render-transform
//           component the renderer actually walks). Independent from
//           the entity's +0x58 game-logic pos.
//   +0x58 = vec3 game-logic pos (read by AI/scripts).
//   +0x70 = 3x3 rotation matrix.
//
// Sub-component layout (validated by 2026-05-09 runtime trace probe):
//   sub+0x04 = vec3 "feet" pos.
//   sub+0x10 = vec3 "center" pos (= feet + (0, 1, 0)) — THIS is what
//              the renderer reads for the model transform.
//
// The engine has an altitude-gated sync from entity+0x58 → sub+0x10:
// only updates sub+0x10 when the new pos is "near" the navmesh.
// At altitude the sync refuses and sub+0x10 stays at the last good
// navmesh anchor — that's why MMB-tp at high cam altitude is invisible
// (the +0x58 hold writes have no render effect). Bypass the gate by
// writing sub+0x10 directly every render frame during the hold.
constexpr uintptr_t kEntityLookupByNameRetryVA = 0x005AC8F0;
constexpr uintptr_t kEntityManagerPtrVA        = 0x007425AC;
constexpr uintptr_t kEntityPosOffset           = 0x58;
constexpr uintptr_t kEntitySubComponentOffset  = 0x48;
constexpr uintptr_t kSubRenderPosOffset        = 0x10;
constexpr float     kSubRenderPosYOffset       = 1.0f;
using PFN_EntityLookupRetry = void* (__thiscall*)(void* self, const char* name, int unused);
const auto g_entity_lookup_retry =
    reinterpret_cast<PFN_EntityLookupRetry>(kEntityLookupByNameRetryVA);

void* resolve_player_entity() {
    void* mgr = *reinterpret_cast<void* volatile*>(kEntityManagerPtrVA);
    if (!mgr) return nullptr;
    void* p = nullptr;
    __try {
        p = g_entity_lookup_retry(mgr, "player", 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p;
}

// Forward declarations of hold-state atomics (defined ~80 lines below).
// The transform-skip hook below needs to read g_teleport_hold_remaining;
// the rest of the hold state is defined in its natural place near the
// other MMB-teleport state.
extern std::atomic<int> g_teleport_hold_remaining;

// === entity_transform_tick player-skip hook =================================
//
// `entity_transform_tick` (sub_4B9F60 @ 0x4B9F60) is called from
// `simulation_tick_aggregator` and walks the global transform-list head
// `dword_724DE4` (linked list, next-pointer at offset +4 of each node). For
// each node, if `*(node+68) & 0x10` is CLEAR, it processes the node — for
// player-shaped nodes (mode 8), that means computing an interpolated
// pos+rotation from the current anim sample and writing them to:
//
//   *(node+92) + 0x58   — pos vec3 (subject's game-logic pos)
//   *(node+92) + 0x70   — rotation 3x3
//
// `*(node+92)` is the subject — for the player's transform-list node it's
// the player entity. After MMB-tp at altitude, the engine puts the player
// into a "balance recovery" / off-navmesh-anchor anim, and that anim's pos
// curve drives `player+0x58` BACK to the navmesh anchor every sim tick. We
// can re-write `+0x58` after the fact (which on_post_sim_aggregator does)
// but the renderer's world matrix is built from the same anim pipeline —
// so the visible model snaps to the anchor for several frames before any
// of our writes take effect downstream.
//
// Root-cause fix: set the engine's OWN skip-bit `0x10` at `node+68` for the
// player's node before orig runs, clear it after. The anim still ticks (so
// the rest of the engine stays coherent) but the per-tick pos/rot write
// for the player skips entirely. With `+0x58` no longer being clobbered,
// our hold-write sticks and downstream consumers (sub+0x10 sync, render
// matrix builder) see our teleport target as the new "current pos".
constexpr uintptr_t kEntityTransformTickVA   = 0x004B9F60;
constexpr uintptr_t kTransformListHeadAddrVA = 0x00724DE4;
constexpr uintptr_t kTransformNodeNextOffset = 4;
constexpr uintptr_t kTransformNodeFlagsOffset = 68;
constexpr uintptr_t kTransformNodeSubjectOffset = 92;
constexpr uint8_t   kTransformNodeSkipBit    = 0x10;

using PFN_EntityTransformTick = void (__cdecl*)();
PFN_EntityTransformTick g_orig_entity_transform_tick = nullptr;

// Walk the transform-list (head at *0x724DE4, next at +4) and find the
// node whose `*(node+92) == player`. Returns nullptr if not found or on
// fault. Capped iteration as a safety belt against corrupt list state.
void* find_player_transform_node(void* player) {
    if (!player) return nullptr;
    void* found = nullptr;
    __try {
        void* node = *reinterpret_cast<void* volatile*>(kTransformListHeadAddrVA);
        for (int i = 0; i < 8192 && node; ++i) {
            auto* n = static_cast<uint8_t*>(node);
            void* subject = *reinterpret_cast<void* const*>(n + kTransformNodeSubjectOffset);
            if (subject == player) {
                found = node;
                break;
            }
            node = *reinterpret_cast<void* const*>(n + kTransformNodeNextOffset);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = nullptr;
    }
    return found;
}

void __cdecl hk_entity_transform_tick() {
    void* skipped_node = nullptr;
    if (g_teleport_hold_remaining.load(std::memory_order_relaxed) > 0) {
        void* player = resolve_player_entity();
        skipped_node = find_player_transform_node(player);
        if (skipped_node) {
            __try {
                auto* flags = static_cast<uint8_t*>(skipped_node) + kTransformNodeFlagsOffset;
                *flags |= kTransformNodeSkipBit;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                skipped_node = nullptr;
            }
        }
    }

    g_orig_entity_transform_tick();

    if (skipped_node) {
        __try {
            auto* flags = static_cast<uint8_t*>(skipped_node) + kTransformNodeFlagsOffset;
            *flags &= static_cast<uint8_t>(~kTransformNodeSkipBit);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

bool g_transform_hook_installed = false;

void install_transform_skip_hook_impl() {
    if (g_transform_hook_installed) return;
    if (MH_CreateHook(reinterpret_cast<void*>(kEntityTransformTickVA),
                      reinterpret_cast<void*>(&hk_entity_transform_tick),
                      reinterpret_cast<void**>(&g_orig_entity_transform_tick)) != MH_OK) {
        mtr::log::info("freecam: MH_CreateHook(entity_transform_tick) failed");
        return;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(kEntityTransformTickVA)) != MH_OK) {
        mtr::log::info("freecam: MH_EnableHook(entity_transform_tick) failed");
        return;
    }
    g_transform_hook_installed = true;
    mtr::log::info("freecam: entity_transform_tick player-skip hook installed @0x%08X",
                   kEntityTransformTickVA);
}

// MMB-teleport diagnostics (surfaced in tab_camera).
std::atomic<uint64_t> g_mmb_teleport_writes{0};
std::atomic<uint64_t> g_mmb_teleport_skips{0};

// Snap-back trace: armed on each MMB-tp, decremented at multiple points
// in the pump per frame, logs sub+0x10 + +0x58 each time. Lets us see
// WHEN within the frame gravity drops Wilbur — the difference between
// "logged at sim_aggregator POST" vs "logged at EndScene PRE" pinpoints
// the writer that beats our hold.
std::atomic<int>      g_snap_trace_remaining{0};
std::atomic<uint64_t> g_snap_trace_frame{0};
std::atomic<float>    g_snap_trace_target_x{0.0f};
std::atomic<float>    g_snap_trace_target_y{0.0f};
std::atomic<float>    g_snap_trace_target_z{0.0f};

void log_pos_at(const char* phase) {
    int rem = g_snap_trace_remaining.load(std::memory_order_acquire);
    if (rem <= 0) return;
    void* player = nullptr;
    {
        void* mgr = *reinterpret_cast<void* volatile*>(kEntityManagerPtrVA);
        if (mgr) {
            __try { player = g_entity_lookup_retry(mgr, "player", 1); }
            __except (EXCEPTION_EXECUTE_HANDLER) { player = nullptr; }
        }
    }
    if (!player) return;
    __try {
        const uint8_t* base = static_cast<const uint8_t*>(player);
        const float* p58 = reinterpret_cast<const float*>(base + 0x58);
        void* sub = *reinterpret_cast<void* const*>(base + 0x48);
        const float* sf = sub ? reinterpret_cast<const float*>(static_cast<uint8_t*>(sub) + 0x10) : nullptr;
        mtr::log::info("freecam: trace[%s] +58=(%.2f,%.2f,%.2f) sub+10=(%.2f,%.2f,%.2f)",
                       phase,
                       p58[0], p58[1], p58[2],
                       sf ? sf[0] : 0.0f, sf ? sf[1] : 0.0f, sf ? sf[2] : 0.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Teleport-hold state. After MMB fires teleport_player_to(), the engine's
// entity_transform_tick (called from sim_aggregator) overwrites entity+0x58
// with the lerped anim sample on the next sim tick — Wilbur visibly snaps
// back to where he was, then the AI plays "on-edge keep balance" because
// our teleport target was off the navmesh. To make the teleport stick, we
// re-write entity+0x58 in sim_aggregator POST (after entity_transform_tick
// has run, before render reads the pos) for the next N frames.
//
// Hold-frames default = 60 (~1 sec at 60 fps). 0 = no hold (snap-back
// behavior). Sufficiently large values let the user park Wilbur in mid-
// air for camera shots; the AI's recovery anims still play but pos sticks.
std::atomic<int>   g_teleport_hold_frames_setting{60};
std::atomic<int>   g_teleport_hold_remaining{0};
std::atomic<float> g_teleport_target_x{0.0f};
std::atomic<float> g_teleport_target_y{0.0f};
std::atomic<float> g_teleport_target_z{0.0f};
std::atomic<uint64_t> g_teleport_hold_writes{0};

// Player-entity teleport. Extracted out of tick() because tick() holds
// std::scoped_lock (a destructor-having object) and MSVC forbids __try
// in functions that require unwinding. This helper takes the pos by
// value, does the SEH-wrapped lookup + write, and reports the outcome
// via the diagnostic counters. No locks held while we touch engine
// memory.
// Write the 3-float pos to player+0x58 (game-logic pos). Returns true if the
// entity was resolved and the write didn't fault. Used by teleport_player_to
// + on_post_sim_aggregator (sim-rate hold).
bool write_player_pos(float px, float py, float pz) {
    void* player = resolve_player_entity();
    if (!player) return false;
    bool ok = false;
    __try {
        auto* base = static_cast<uint8_t*>(player);
        const float pos[3] = { px, py, pz };
        std::memcpy(base + kEntityPosOffset, pos, sizeof(pos));
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// Write the 3-float pos to *(player+0x48)+0x10 — the model "center"
// pos. The engine has an altitude-gated sync from entity+0x58 → sub+0x10
// that refuses high jumps; bypass by writing directly. Engine convention
// is feet + (0, 1, 0). The actual snap-back fix lives in our
// entity_transform_tick hook (skip-bit 0x10 on player's node) which
// stops the engine from re-running the anim-driven pos write each tick.
// This sub+0x10 write keeps the model's render transform aligned during
// hold so the engine's eventual recompute still has a coherent state.
bool write_player_render_pos(float px, float py, float pz) {
    void* player = resolve_player_entity();
    if (!player) return false;
    bool ok = false;
    __try {
        auto* base = static_cast<uint8_t*>(player);
        if (void* sub_a = *reinterpret_cast<void* const*>(base + 0x48)) {
            auto* sb = static_cast<uint8_t*>(sub_a);
            const float center_pos[3] = { px, py + 1.0f, pz };
            std::memcpy(sb + 0x10, center_pos, sizeof(center_pos));
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

void teleport_player_to(float px, float py, float pz) {
    void* player = resolve_player_entity();
    if (!player) {
        g_mmb_teleport_skips.fetch_add(1, std::memory_order_relaxed);
        mtr::log::info("freecam: MMB teleport -- no player entity (between levels?)");
        return;
    }
    // One-shot dump of *(player+0x50) — the second heap pointer in the
    // player entity (Avatar string was at +0x54). +0x48 sub turned out
    // not to be the renderer's pos source; +0x50 is the next candidate.
    static std::atomic<bool> s_sub50_dumped{false};
    if (!s_sub50_dumped.exchange(true)) {
        __try {
            const uint8_t* base = static_cast<const uint8_t*>(player);
            void* sub_b = *reinterpret_cast<void* const*>(base + 0x50);
            if (sub_b) {
                const uint8_t* sb = static_cast<const uint8_t*>(sub_b);
                char line[16 * 3 + 1];
                mtr::log::info("freecam: --- *(player+0x50) sub-B %p (192 bytes) ---", sub_b);
                for (size_t row = 0; row < 12; ++row) {
                    size_t off = row * 16;
                    for (size_t col = 0; col < 16; ++col) {
                        std::snprintf(&line[col * 3], 4, "%02X ", sb[off + col]);
                    }
                    line[16 * 3] = 0;
                    mtr::log::info("  subB+0x%02zX  %s", off, line);
                }
                const float* sf = reinterpret_cast<const float*>(sb);
                mtr::log::info("  subB@+0x00 floats: %.2f %.2f %.2f %.2f", sf[0], sf[1], sf[2], sf[3]);
                mtr::log::info("  subB@+0x10 floats: %.2f %.2f %.2f %.2f", sf[4], sf[5], sf[6], sf[7]);
                mtr::log::info("  subB@+0x40 floats: %.2f %.2f %.2f %.2f", sf[16], sf[17], sf[18], sf[19]);
                mtr::log::info("  subB@+0x50 floats: %.2f %.2f %.2f %.2f", sf[20], sf[21], sf[22], sf[23]);
            } else {
                mtr::log::info("freecam: *(player+0x50) is NULL");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mtr::log::info("freecam: subB dump faulted");
        }
    }

    if (write_player_pos(px, py, pz)) {
        // Also write the render-side sub-component pos directly. This is
        // what makes the teleport actually visible — without this, the
        // engine's altitude-gated sync refuses high jumps and Wilbur's
        // model stays at the last navmesh anchor while +0x58 sits at our
        // teleport target unread by render.
        write_player_render_pos(px, py, pz);
        g_mmb_teleport_writes.fetch_add(1, std::memory_order_relaxed);
        mtr::log::info("freecam: MMB teleport -- player -> (%.2f, %.2f, %.2f)",
                       px, py, pz);
        // Arm the post-sim hold: re-write +0x58 + sub+0x10 every sim
        // tick + every render frame for the next N sim ticks so the
        // engine's snap-back doesn't take effect.
        const int hold = g_teleport_hold_frames_setting.load(std::memory_order_relaxed);
        if (hold > 0) {
            g_teleport_target_x.store(px, std::memory_order_relaxed);
            g_teleport_target_y.store(py, std::memory_order_relaxed);
            g_teleport_target_z.store(pz, std::memory_order_relaxed);
            g_teleport_hold_remaining.store(hold, std::memory_order_release);
        }
        // Arm phase-trace probe: log player pos at multiple phases per
        // render frame for the next 60 frames, so we can pinpoint the
        // pump phase that drops sub+0x10 between our writes.
        g_snap_trace_target_x.store(px, std::memory_order_relaxed);
        g_snap_trace_target_y.store(py, std::memory_order_relaxed);
        g_snap_trace_target_z.store(pz, std::memory_order_relaxed);
        g_snap_trace_frame.store(0, std::memory_order_relaxed);
        g_snap_trace_remaining.store(20, std::memory_order_release);
        log_pos_at("MMB_post");
    } else {
        g_mmb_teleport_skips.fetch_add(1, std::memory_order_relaxed);
        mtr::log::info("freecam: MMB teleport -- entity write faulted");
    }
}

// Per-render-frame render-pos hold writer. Called from freecam::tick at
// the top of every render frame. While the sim-rate teleport hold is
// active, re-writes sub+0x10 (the renderer's pos field) at render rate
// to defeat the sub-component's per-render-frame gravity (which would
// otherwise drop Wilbur ~0.04 units in Y between sim ticks). Cheap when
// no hold is active (single relaxed atomic load).
void render_hold_tick() {
    // Phase-trace: log pos AT FRAME START (i.e. as the engine sees it
    // when EndScene PRE fires — gravity has just acted between previous
    // EndScene and now). This is the BEFORE side of the per-frame loop.
    log_pos_at("frame_start");

    if (g_teleport_hold_remaining.load(std::memory_order_relaxed) > 0) {
        const float tx = g_teleport_target_x.load(std::memory_order_relaxed);
        const float ty = g_teleport_target_y.load(std::memory_order_relaxed);
        const float tz = g_teleport_target_z.load(std::memory_order_relaxed);
        if (write_player_render_pos(tx, ty, tz)) {
            g_teleport_hold_writes.fetch_add(1, std::memory_order_relaxed);
        }
        log_pos_at("frame_after_write");
    }

    // Decrement the trace counter (separate from sim-rate hold counter).
    int rem = g_snap_trace_remaining.load(std::memory_order_relaxed);
    if (rem > 0) {
        g_snap_trace_remaining.store(rem - 1, std::memory_order_relaxed);
    }
}

// Mouse-wheel accumulator. Filled by the WH_MOUSE_LL hook (in menu.cpp);
// drained by tick() to scale move_speed.
std::atomic<int> g_wheel_accum{0};

constexpr float kPitchLimit = 1.5533f;  // ~89 deg

// Compute camera basis. RH. yaw=0 looks +Z; positive yaw rotates the look
// vector toward the camera's RIGHT (-X in world). Hence the sign on sy below.
void compute_basis(float yaw, float pitch,
                   float fwd[3], float xaxis[3], float yaxis[3], float zaxis[3]) {
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);

    fwd[0] = -sy * cp;
    fwd[1] =  sp;
    fwd[2] =  cy * cp;

    // RH view: zaxis = -forward
    zaxis[0] = -fwd[0];  zaxis[1] = -fwd[1];  zaxis[2] = -fwd[2];

    // xaxis = normalise(cross((0,1,0), zaxis)) = (zaxis.z, 0, -zaxis.x)
    xaxis[0] =  zaxis[2];
    xaxis[1] =  0.0f;
    xaxis[2] = -zaxis[0];
    const float xlen = std::sqrt(xaxis[0]*xaxis[0] + xaxis[2]*xaxis[2]);
    if (xlen > 1e-6f) {
        xaxis[0] /= xlen; xaxis[2] /= xlen;
    } else {
        xaxis[0] = 1.0f; xaxis[2] = 0.0f;
    }

    // yaxis = cross(zaxis, xaxis)
    yaxis[0] = zaxis[1]*xaxis[2] - zaxis[2]*xaxis[1];
    yaxis[1] = zaxis[2]*xaxis[0] - zaxis[0]*xaxis[2];
    yaxis[2] = zaxis[0]*xaxis[1] - zaxis[1]*xaxis[0];
}

bool key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// LookAt-RH style view matrix from current pose. Caller holds g_mu.
void build_view_matrix_locked(D3DMATRIX* out) {
    float fwd[3], xax[3], yax[3], zax[3];
    compute_basis(g_st.yaw, g_st.pitch, fwd, xax, yax, zax);

    out->m[0][0] = xax[0]; out->m[0][1] = yax[0]; out->m[0][2] = zax[0]; out->m[0][3] = 0.0f;
    out->m[1][0] = xax[1]; out->m[1][1] = yax[1]; out->m[1][2] = zax[1]; out->m[1][3] = 0.0f;
    out->m[2][0] = xax[2]; out->m[2][1] = yax[2]; out->m[2][2] = zax[2]; out->m[2][3] = 0.0f;
    out->m[3][0] = -(g_st.pos[0]*xax[0] + g_st.pos[1]*xax[1] + g_st.pos[2]*xax[2]);
    out->m[3][1] = -(g_st.pos[0]*yax[0] + g_st.pos[1]*yax[1] + g_st.pos[2]*yax[2]);
    out->m[3][2] = -(g_st.pos[0]*zax[0] + g_st.pos[1]*zax[1] + g_st.pos[2]*zax[2]);
    out->m[3][3] = 1.0f;
}

// World matrix is the inverse of the view matrix. For a rigid LookAt-RH view
// (orthonormal rotation + translation), the inverse has the rotation
// transposed and translation = -R^T * t = eye position in world. Caller
// holds g_mu.
void build_world_matrix_locked(D3DMATRIX* out) {
    float fwd[3], xax[3], yax[3], zax[3];
    compute_basis(g_st.yaw, g_st.pitch, fwd, xax, yax, zax);

    // Columns of world rotation = view-axes rows = world-axes.
    out->m[0][0] = xax[0]; out->m[0][1] = xax[1]; out->m[0][2] = xax[2]; out->m[0][3] = 0.0f;
    out->m[1][0] = yax[0]; out->m[1][1] = yax[1]; out->m[1][2] = yax[2]; out->m[1][3] = 0.0f;
    out->m[2][0] = zax[0]; out->m[2][1] = zax[1]; out->m[2][2] = zax[2]; out->m[2][3] = 0.0f;
    out->m[3][0] = g_st.pos[0]; out->m[3][1] = g_st.pos[1]; out->m[3][2] = g_st.pos[2]; out->m[3][3] = 1.0f;
}

} // namespace

bool active() {
    std::scoped_lock lock(g_mu);
    return g_st.active;
}

void set_active(bool a) {
    std::scoped_lock lock(g_mu);
    if (a == g_st.active) return;
    g_st.active = a;
    if (a) {
        // Seed pose from next post-tick engine view-matrix overwrite.
        g_st.pose_seeded = false;
    }
    mtr::log::info("freecam: active=%d (yaw=%.2f pitch=%.2f pos=%.1f,%.1f,%.1f)",
                   a ? 1 : 0, g_st.yaw, g_st.pitch,
                   g_st.pos[0], g_st.pos[1], g_st.pos[2]);
}

void get_pose(float pos[3], float* yaw, float* pitch) {
    std::scoped_lock lock(g_mu);
    pos[0] = g_st.pos[0]; pos[1] = g_st.pos[1]; pos[2] = g_st.pos[2];
    if (yaw)   *yaw   = g_st.yaw;
    if (pitch) *pitch = g_st.pitch;
}

float move_speed() { std::scoped_lock lock(g_mu); return g_st.move_speed; }
float look_speed() { std::scoped_lock lock(g_mu); return g_st.look_speed; }
float mouse_sens() { std::scoped_lock lock(g_mu); return g_st.mouse_sens; }
void  set_move_speed(float v) { std::scoped_lock lock(g_mu); g_st.move_speed = v; }
void  set_look_speed(float v) { std::scoped_lock lock(g_mu); g_st.look_speed = v; }
void  set_mouse_sens(float v) { std::scoped_lock lock(g_mu); g_st.mouse_sens = v; }

void set_ui_visible(bool v)        { g_ui_visible.store(v); }
void set_last_controller(void* c)  { g_last_controller.store(c); }
void request_teleport_to_camera()  { g_pending_mmb.store(1); }

// Called by the WH_MOUSE_LL hook (menu.cpp). delta is signed wheel ticks
// (typically +-120 per click). Accumulated and consumed in tick().
void accumulate_wheel(int delta) {
    g_wheel_accum.fetch_add(delta);
}

void tick() {
    // Per-render-frame teleport-hold writer: re-writes sub+0x10 at
    // render rate so gravity in the sub-component doesn't drop Wilbur
    // between sim ticks. No-op when no hold is active.
    render_hold_tick();

    // High-resolution dt via QueryPerformanceCounter. First call lazily
    // queries the frequency; subsequent calls just diff the counter.
    LARGE_INTEGER qpc_now{};
    QueryPerformanceCounter(&qpc_now);
    int64_t freq = g_qpc_freq.load(std::memory_order_relaxed);
    if (freq == 0) {
        LARGE_INTEGER qpc_freq{};
        QueryPerformanceFrequency(&qpc_freq);
        freq = qpc_freq.QuadPart;
        if (freq <= 0) freq = 1;
        g_qpc_freq.store(freq, std::memory_order_relaxed);
    }
    int64_t prev_qpc = g_qpc_last.exchange(qpc_now.QuadPart,
                                           std::memory_order_relaxed);
    if (prev_qpc == 0) return;  // first tick — no dt yet
    float dt = static_cast<float>(qpc_now.QuadPart - prev_qpc)
             / static_cast<float>(freq);
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0.0f) dt = 0.0f;   // QPC monotonic; defensive against driver glitches

    const bool ui_open = g_ui_visible.load();
    const int pending_mmb = g_pending_mmb.exchange(0);
    const int wheel = g_wheel_accum.exchange(0);

    // Locked region in an explicit inner scope so the mutex unwinds
    // before we make any engine calls below (teleport_player_to uses
    // __try, which MSVC forbids alongside destructor-having locals at
    // the same function scope — C2712).
    float tp_x = 0.0f, tp_y = 0.0f, tp_z = 0.0f;
    bool  do_tp = false;
    {
    std::scoped_lock lock(g_mu);
    if (!g_st.active) {
        g_mouse_skip_first_delta = true;
        return;
    }

    // Wheel adjusts move_speed exponentially. ~120 units per click; 5% per
    // unit. So one click ~= 6x boost; one click down ~= 6x reduction. Clamp
    // to [min_speed, max_speed].
    if (wheel != 0) {
        const float factor = std::exp(static_cast<float>(wheel) * 0.0015f);
        g_st.move_speed *= factor;
        if (g_st.move_speed < g_st.min_speed) g_st.move_speed = g_st.min_speed;
        if (g_st.move_speed > g_st.max_speed) g_st.move_speed = g_st.max_speed;
    }

    const bool fast = key_down(VK_SHIFT);
    const float vmove = g_st.move_speed * (fast ? g_st.fast_mult : 1.0f) * dt;
    const float vlook = g_st.look_speed * dt;

    // Mouse-look (PRIMARY). Recenter to the foreground window's client-area
    // center each frame; delta = cursor - center; SetCursorPos back to center.
    // The center recentering keeps deltas symmetric in both directions even
    // in fullscreen-exclusive (where the OS pins the cursor at edges of the
    // working area, which would otherwise clip leftward/upward delta to 0).
    if (!ui_open) {
        HWND hwnd = GetForegroundWindow();
        RECT rc;
        if (hwnd && GetClientRect(hwnd, &rc)) {
            POINT center{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
            ClientToScreen(hwnd, &center);

            POINT cur;
            if (GetCursorPos(&cur)) {
                if (!g_mouse_skip_first_delta) {
                    const int dx = cur.x - center.x;
                    const int dy = cur.y - center.y;
                    if (dx != 0 || dy != 0) {
                        g_st.yaw   += static_cast<float>(dx) * g_st.mouse_sens;
                        g_st.pitch -= static_cast<float>(dy) * g_st.mouse_sens;
                    }
                }
            }
            // Always recenter, even on the first tick after activation -- the
            // game / OS may have parked the cursor anywhere; resetting to the
            // window center every frame guarantees the next read is symmetric.
            SetCursorPos(center.x, center.y);
            g_mouse_skip_first_delta = false;
        }
    } else {
        g_mouse_skip_first_delta = true;
    }

    float fwd[3], xax[3], yax[3], zax[3];
    compute_basis(g_st.yaw, g_st.pitch, fwd, xax, yax, zax);

    if (key_down('W')) for (int i = 0; i < 3; ++i) g_st.pos[i] += fwd[i] * vmove;
    if (key_down('S')) for (int i = 0; i < 3; ++i) g_st.pos[i] -= fwd[i] * vmove;
    if (key_down('D')) for (int i = 0; i < 3; ++i) g_st.pos[i] += xax[i] * vmove;
    if (key_down('A')) for (int i = 0; i < 3; ++i) g_st.pos[i] -= xax[i] * vmove;
    if (key_down(VK_SPACE)) g_st.pos[1] += vmove;
    if (key_down('C'))      g_st.pos[1] -= vmove;

    // Arrow keys = fallback look (consistent with mouse: right = right turn).
    if (key_down(VK_LEFT))  g_st.yaw   -= vlook;
    if (key_down(VK_RIGHT)) g_st.yaw   += vlook;
    if (key_down(VK_UP))    g_st.pitch += vlook;
    if (key_down(VK_DOWN))  g_st.pitch -= vlook;

    if (g_st.pitch >  kPitchLimit) g_st.pitch =  kPitchLimit;
    if (g_st.pitch < -kPitchLimit) g_st.pitch = -kPitchLimit;

    // Snapshot the camera pos for the deferred teleport (the SEH path
    // can't run while scoped_lock is still in scope — C2712).
    if (pending_mmb) {
        tp_x = g_st.pos[0];
        tp_y = g_st.pos[1];
        tp_z = g_st.pos[2];
        do_tp = true;
    }
    } // unlock g_mu

    if (do_tp) {
        teleport_player_to(tp_x, tp_y, tp_z);
    }
}

uint64_t mmb_teleport_writes() { return g_mmb_teleport_writes.load(std::memory_order_relaxed); }
uint64_t mmb_teleport_skips()  { return g_mmb_teleport_skips.load(std::memory_order_relaxed); }

// True while the snap-back diagnostic trace is armed (set by MMB-tp,
// decremented per render frame). Used by d3d9_hook to gate a verbose
// log of D3DTS_WORLD matrices so we can identify which world matrix
// the renderer is using for the player model.
bool trace_armed() {
    return g_snap_trace_remaining.load(std::memory_order_relaxed) > 0;
}

// Public forwarder for the MinHook install of entity_transform_tick.
// Called from dllmain after MH_Initialize succeeds. See the in-namespace
// implementation comment for why this hook exists.
void install_transform_skip_hook() {
    install_transform_skip_hook_impl();
}

int      teleport_hold_frames_setting() {
    return g_teleport_hold_frames_setting.load(std::memory_order_relaxed);
}
void     set_teleport_hold_frames_setting(int frames) {
    if (frames < 0)    frames = 0;
    if (frames > 1200) frames = 1200;
    g_teleport_hold_frames_setting.store(frames, std::memory_order_relaxed);
}
int      teleport_hold_remaining() {
    return g_teleport_hold_remaining.load(std::memory_order_relaxed);
}
uint64_t teleport_hold_writes() {
    return g_teleport_hold_writes.load(std::memory_order_relaxed);
}
void     cancel_teleport_hold() {
    g_teleport_hold_remaining.store(0, std::memory_order_relaxed);
}

// Called from sim_decouple's hk_sim_aggregator AFTER orig sim has run
// (so entity_transform_tick has already written the snap-back pos), but
// BEFORE the render reads pos. Re-writes both player+0x58 (game-logic
// pos) and sub+0x10 (render pos) with our teleport target until the
// hold counter expires. Decrements the hold counter at sim rate; the
// per-render-frame render_hold_tick handles the visual hold between
// sim ticks. Cheap when the hold is 0 (just a relaxed atomic load).
void on_post_sim_aggregator() {
    log_pos_at("post_sim_PRE");
    int rem = g_teleport_hold_remaining.load(std::memory_order_acquire);
    if (rem <= 0) {
        log_pos_at("post_sim_POST_no_hold");
        return;
    }
    const float tx = g_teleport_target_x.load(std::memory_order_relaxed);
    const float ty = g_teleport_target_y.load(std::memory_order_relaxed);
    const float tz = g_teleport_target_z.load(std::memory_order_relaxed);
    write_player_pos(tx, ty, tz);
    write_player_render_pos(tx, ty, tz);
    log_pos_at("post_sim_POST");
    g_teleport_hold_remaining.store(rem - 1, std::memory_order_release);
}

// Public: build a LookAt-RH view matrix from the current freecam pose.
// Used by d3d9_hook's per-camera apply hook to overwrite the engine's view
// before sub_4C1BA0 propagates it to globals + D3D.
bool build_view_matrix(D3DMATRIX* out) {
    if (!out) return false;
    std::scoped_lock lock(g_mu);
    if (!g_st.active) return false;
    build_view_matrix_locked(out);
    return true;
}

void on_engine_view(const D3DMATRIX& vm) {
    std::scoped_lock lock(g_mu);
    if (!g_st.active || g_st.pose_seeded) return;

    // RH-LookAt: cols of the rotation 3x3 are view-space axes in world coords.
    const float xax[3] = { vm.m[0][0], vm.m[1][0], vm.m[2][0] };
    const float yax[3] = { vm.m[0][1], vm.m[1][1], vm.m[2][1] };
    const float zax[3] = { vm.m[0][2], vm.m[1][2], vm.m[2][2] };

    // eye = -t.x*xaxis - t.y*yaxis - t.z*zaxis.
    const float tx = vm.m[3][0], ty = vm.m[3][1], tz = vm.m[3][2];
    g_st.pos[0] = -(tx*xax[0] + ty*yax[0] + tz*zax[0]);
    g_st.pos[1] = -(tx*xax[1] + ty*yax[1] + tz*zax[1]);
    g_st.pos[2] = -(tx*xax[2] + ty*yax[2] + tz*zax[2]);

    // forward = -zaxis. With our convention forward.x = -sin(yaw)*cos(pitch),
    // so yaw = atan2(-forward.x, forward.z). pitch = asin(forward.y).
    const float fwd[3] = { -zax[0], -zax[1], -zax[2] };
    g_st.pitch = std::asin(fwd[1]);
    g_st.yaw   = std::atan2(-fwd[0], fwd[2]);
    g_st.pose_seeded = true;

    mtr::log::info("freecam: seeded from engine view -- pos=(%.1f,%.1f,%.1f) yaw=%.3f pitch=%.3f",
                   g_st.pos[0], g_st.pos[1], g_st.pos[2], g_st.yaw, g_st.pitch);
}

// Called from d3d9_hook AFTER camera_apply_all_active runs. Engine's per-camera
// applies have already written the active camera's view/world to the globals
// (0x724C10 / 0x724C50). We last-write our pose so culling/render see it.
void apply_to_globals() {
    if (!active()) return;

    D3DMATRIX* view_global  = reinterpret_cast<D3DMATRIX*>(kViewMatrixGlobalVA);
    D3DMATRIX* world_global = reinterpret_cast<D3DMATRIX*>(kWorldMatrixGlobalVA);

    // Seed from engine's current view (the one camera_apply just wrote)
    // before we overwrite, so first activation lands on the engine's pose.
    on_engine_view(*view_global);

    {
        std::scoped_lock lock(g_mu);
        if (!g_st.active) return;
        D3DMATRIX vm{}, wm{};
        build_view_matrix_locked(&vm);
        build_world_matrix_locked(&wm);
        std::memcpy(view_global,  &vm, sizeof(D3DMATRIX));
        std::memcpy(world_global, &wm, sizeof(D3DMATRIX));
    }
}

} // namespace mtr::freecam
