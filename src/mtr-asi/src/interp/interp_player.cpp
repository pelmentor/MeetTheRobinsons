// Player (Wilbur) transform interpolation client (M4).
//
// Snapshots the player entity's pos (offset +0x58, 12 bytes) + rotation
// 3x3 (offset +0x70, 36 bytes) at POST simulation_tick_aggregator. PRE
// next sim aggregator: restore saved pre-interp values so sim's reads
// are clean (M2.3 fence pattern). POST camera_apply_all_active: save +
// write lerp+slerp(prev, curr, alpha) into the entity.
//
// The player entity is found via entity_lookup_by_name_retry("player", 1)
// and cached. Re-lookup on null, on staleness (10 frames), or on user
// request (force_player_handle_refresh). Pointer swap detection clears
// snapshots so the next window doesn't blend across an entity replacement.
//
// Fence-violation diagnostic: every render-frame write captures what we
// wrote into g_last_written_player. The next sim's pre-restore compares
// entity memory; any difference means SOMETHING modified the entity
// between our write and the next sim's PRE — falsifying the M4 fence
// assumption ("nothing reads/writes entity transforms in that window").

#include "interp_internal.h"
#include "mtr/interp.h"
#include "mtr/sim_decouple.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::interp {

namespace {

// Engine helper at 0x5AC8F0 is __thiscall (not __stdcall): requires the
// entity manager pointer in ECX (loaded from the global at 0x7425AC).
// Calling without ECX set walks arbitrary memory off whatever-was-in-ECX
// and either faults or stalls. All engine call sites prefix the call
// with `mov ecx, dword_7425AC`. See freecam.cpp's resolve_player_entity
// for the canonical pattern.
constexpr uintptr_t kEntityLookupByNameRetryVA = 0x005AC8F0;
constexpr uintptr_t kEntityManagerPtrVA        = 0x007425AC;

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

using detail::PlayerSnap;

PlayerSnap g_prev_player{};
PlayerSnap g_curr_player{};

// Pre-interp save: what was in entity BEFORE we wrote interp this render
// frame. PRE-sim hook restores from here so sim sees clean state.
PlayerSnap g_saved_player{};

// Cached player pointer + invalidation. Refreshed every kPlayerHandleRefreshFrames
// frames or whenever the cached pointer reads back as null. Tightened from
// 60 to 10 frames because player entity can be replaced on level transitions
// / mode changes (Wilbur ↔ MiniHamsterPlayer ↔ DigDug). 60-frame staleness
// meant up to 1 second of writing interp values to freed-then-pool-reused
// memory which COULD be a different entity now.
std::atomic<void*> g_player_handle{nullptr};
std::atomic<int>   g_player_handle_age{0};
constexpr int      kPlayerHandleRefreshFrames = 10;

std::atomic<uint64_t> g_player_handle_swaps{0};
std::atomic<bool>     g_player_interp_enabled{false};
std::atomic<uint64_t> g_player_interp_writes{0};
std::atomic<bool>     g_player_save_valid{false};
std::atomic<bool>     g_player_teleport_for_curr{false};
std::atomic<float>    g_player_teleport_threshold{10.0f};
std::atomic<uint64_t> g_player_teleports{0};

// Force a fresh lookup on next get_player() call. Set by the menu's
// "Force player relookup" button or whenever the user toggles a mode.
std::atomic<bool> g_player_handle_force_refresh{false};

// Fence-violation diagnostic. Each render frame's M4 write captures
// what we wrote into g_last_written_player; the next sim's pre-restore
// compares entity memory to it.
PlayerSnap            g_last_written_player{};
std::atomic<bool>     g_last_written_valid{false};
std::atomic<uint64_t> g_player_fence_violations{0};

// Read the cached player pointer; refresh on staleness or null. Returns
// nullptr if the engine doesn't have a player entity yet (early startup,
// between level loads).
//
// Side effect on swap: when the lookup returns a different pointer than
// the cached one, we invalidate the prev/curr player snapshots so the
// next snapshot starts a fresh window.
void* get_player() {
    void* p = g_player_handle.load(std::memory_order_acquire);
    const int age = g_player_handle_age.fetch_add(1, std::memory_order_relaxed);
    const bool force = g_player_handle_force_refresh.exchange(false, std::memory_order_acq_rel);
    if (!p || age >= kPlayerHandleRefreshFrames || force) {
        // Engine's entity registry isn't initialized until well after our
        // hooks are live (the first sim tick fires during early boot,
        // before the actor world or registry exists). resolve_player_entity
        // returns nullptr if the manager global is still NULL, and SEH-
        // wraps the actual lookup call.
        void* fresh = resolve_player_entity();
        if (fresh && fresh != p && p != nullptr) {
            g_curr_player.valid = false;
            g_prev_player.valid = false;
            g_player_save_valid.store(false, std::memory_order_release);
            g_player_handle_swaps.fetch_add(1, std::memory_order_relaxed);
        }
        g_player_handle.store(fresh, std::memory_order_release);
        g_player_handle_age.store(0, std::memory_order_relaxed);
        return fresh;
    }
    return p;
}

} // namespace

namespace detail {

void* current_player_handle() {
    return g_player_handle.load(std::memory_order_acquire);
}

void player_apply_interp_for_render_frame(float alpha) {
    if (!g_player_interp_enabled.load(std::memory_order_relaxed))    return;
    if (!g_prev_player.valid || !g_curr_player.valid)                return;
    if (g_player_teleport_for_curr.load(std::memory_order_relaxed))  return;
    if (!mtr::sim_decouple::effective_is_throttling())               return;

    void* p = g_player_handle.load(std::memory_order_acquire);
    if (!p) return;

    // Save BEFORE writing so the next pre-sim restore returns the entity
    // to the "real" sim_curr state, not our last interp.
    capture_pos_rot(g_saved_player, p);
    g_player_save_valid.store(true, std::memory_order_release);

    PlayerSnap out;
    compose_player_interp(g_prev_player, g_curr_player, alpha, out);
    write_pos_rot(out, p);
    g_player_interp_writes.fetch_add(1, std::memory_order_relaxed);

    // Fence diagnostic: remember what we wrote so the next pre-sim
    // restore can flag if the entity's pos/rot got modified by something
    // else in the meantime.
    g_last_written_player = out;
    g_last_written_valid.store(true, std::memory_order_release);
}

} // namespace detail

// === Public API ============================================================
// pre_sim_restore_player + post_sim_capture_player are called from
// sim_decouple's hk_sim_aggregator (PRE / POST orig respectively).

void pre_sim_restore_player() {
    if (!g_player_save_valid.load(std::memory_order_acquire)) return;
    void* p = g_player_handle.load(std::memory_order_acquire);
    if (!p) {
        g_player_save_valid.store(false, std::memory_order_release);
        g_last_written_valid.store(false, std::memory_order_release);
        return;
    }

    // Fence violation check: compare entity's CURRENT pos/rot to what we
    // wrote at last camera_apply-POST. Position-only check (rotation
    // matrix entries can drift slightly through float round-trip even
    // without external writes; position is the cleanest detector).
    if (g_last_written_valid.load(std::memory_order_acquire)) {
        __try {
            const auto* base = static_cast<const uint8_t*>(p);
            float live_pos[3];
            std::memcpy(live_pos, base + detail::kPlayerPosOffset, detail::kPlayerPosBytes);
            const float dx = live_pos[0] - g_last_written_player.pos[0];
            const float dy = live_pos[1] - g_last_written_player.pos[1];
            const float dz = live_pos[2] - g_last_written_player.pos[2];
            // 0.001 covers float-precision round-trip noise.
            if (dx*dx + dy*dy + dz*dz > 0.001f * 0.001f) {
                g_player_fence_violations.fetch_add(1, std::memory_order_relaxed);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Entity stale — same as save_valid clearance below.
        }
        g_last_written_valid.store(false, std::memory_order_release);
    }

    detail::write_pos_rot(g_saved_player, p);
    g_player_save_valid.store(false, std::memory_order_release);
}

void post_sim_capture_player() {
    // Gate on the toggle so a crash-on-boot path can't fire before any
    // user opt-in. Without this gate the unconditional g_entity_lookup
    // call below fires from the very first sim tick during engine init,
    // before the actor world is initialized — which AVs out of the
    // lookup machinery.
    if (!g_player_interp_enabled.load(std::memory_order_relaxed)) return;
    void* p = get_player();
    if (!p) return;
    __try {
        if (g_curr_player.valid) {
            g_prev_player = g_curr_player;
        }
        detail::capture_pos_rot(g_curr_player, p);

        // Teleport detect: |pos delta| > threshold -> snap, no interp this window.
        if (g_prev_player.valid && g_curr_player.valid) {
            const float dx = g_curr_player.pos[0] - g_prev_player.pos[0];
            const float dy = g_curr_player.pos[1] - g_prev_player.pos[1];
            const float dz = g_curr_player.pos[2] - g_prev_player.pos[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            const float t  = g_player_teleport_threshold.load(std::memory_order_relaxed);
            const bool tp  = d2 > t * t;
            g_player_teleport_for_curr.store(tp, std::memory_order_relaxed);
            if (tp) {
                g_player_teleports.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Player handle stale or entity freed mid-capture. Drop the
        // cached pointer + invalidate snapshots so the next call
        // re-lookups cleanly.
        g_player_handle.store(nullptr, std::memory_order_release);
        g_curr_player.valid = false;
        g_prev_player.valid = false;
    }
}

bool player_interp_enabled() { return g_player_interp_enabled.load(std::memory_order_relaxed); }
void set_player_interp_enabled(bool on) {
    const bool prev_v = g_player_interp_enabled.exchange(on, std::memory_order_relaxed);
    if (prev_v != on) {
        mtr::log::info("interp: player_interp %s", on ? "enabled" : "disabled");
    }
    if (!on) {
        // Make sure any in-flight save gets flushed back to the entity so
        // toggling off mid-frame doesn't leave the entity stuck on an
        // interp value.
        pre_sim_restore_player();
    }
}
uint64_t player_interp_writes() { return g_player_interp_writes.load(std::memory_order_relaxed); }

float player_teleport_threshold() {
    return g_player_teleport_threshold.load(std::memory_order_relaxed);
}
void set_player_teleport_threshold(float v) {
    if (v < 0.0f) v = 0.0f;
    g_player_teleport_threshold.store(v, std::memory_order_relaxed);
}
uint64_t player_teleports_detected() {
    return g_player_teleports.load(std::memory_order_relaxed);
}

uint64_t player_handle_swaps() {
    return g_player_handle_swaps.load(std::memory_order_relaxed);
}

void force_player_handle_refresh() {
    g_player_handle_force_refresh.store(true, std::memory_order_release);
}

uint64_t player_fence_violations() {
    return g_player_fence_violations.load(std::memory_order_relaxed);
}

} // namespace mtr::interp
