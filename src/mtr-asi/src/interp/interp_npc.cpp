// NPC transform interpolation client (M5).
//
// Walks the engine's per-frame transform list head `dword_724DE4` (a
// non-encrypted singly-linked list) and snapshots pos+rot for each node.
// Reuses M4's player-style fence pattern but per-entity, hashed by the
// node's inner-transform pointer (`*(node+0x5C)`). Skips the player (M4
// covers it) and any node with the engine's "skip transform" flag.
//
// Storage cap: kMaxNpcSlots fixed-size array. Hash collisions linear-probe;
// stale slots aged out after kStaleSlotsAgeoutFrames sim ticks of absence.
//
// Per-tick SEH guards each entity-memory access — the engine can free a
// node mid-frame and our age-out hasn't fired yet, so a stale slot must
// not crash the game.

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

using detail::PlayerSnap;

constexpr size_t kMaxNpcSlots             = 64;
constexpr int    kStaleSlotsAgeoutFrames  = 6;

struct NpcSlot {
    void*      inner;        // key: *(node+0x5C); zero means slot empty
    PlayerSnap prev;
    PlayerSnap curr;
    PlayerSnap saved;        // pre-interp state (for fence)
    bool       save_valid;
    bool       teleport;
    int        last_seen_tick;
};

NpcSlot g_npc_slots[kMaxNpcSlots]{};
std::atomic<int>      g_npc_tick_counter{0};
std::atomic<bool>     g_npc_interp_enabled{false};
std::atomic<uint64_t> g_npc_interp_writes{0};
std::atomic<uint64_t> g_npc_teleports{0};

// Find or allocate a slot for an inner-transform pointer. Linear-probe a
// hash bucket. Returns nullptr if all slots are taken (cap hit).
NpcSlot* find_or_alloc_npc(void* inner) {
    if (!inner) return nullptr;
    const uintptr_t k = reinterpret_cast<uintptr_t>(inner);
    const size_t base = (k >> 4) & (kMaxNpcSlots - 1);
    for (size_t i = 0; i < kMaxNpcSlots; ++i) {
        const size_t idx = (base + i) & (kMaxNpcSlots - 1);
        if (g_npc_slots[idx].inner == inner) return &g_npc_slots[idx];
        if (!g_npc_slots[idx].inner) {
            g_npc_slots[idx] = NpcSlot{};
            g_npc_slots[idx].inner = inner;
            return &g_npc_slots[idx];
        }
    }
    return nullptr;
}

void age_out_npc_slots(int current_tick) {
    for (size_t i = 0; i < kMaxNpcSlots; ++i) {
        if (g_npc_slots[i].inner
            && current_tick - g_npc_slots[i].last_seen_tick > kStaleSlotsAgeoutFrames) {
            g_npc_slots[i] = NpcSlot{};
        }
    }
}

// Walk the engine's transform list, calling cb(node, inner) for every node
// we should consider. Skips:
//   - nodes with the +0x44 0x10 "skip" flag set
//   - the player's node (M4 covers it)
//   - nodes with NULL inner ptr
template <typename Fn>
void walk_transform_list(Fn cb, void* player) {
    auto* node = *reinterpret_cast<uint8_t**>(detail::kTransformListHeadVA);
    int safety = 8192;  // hard ceiling against pathological list cycles
    while (node && safety-- > 0) {
        const uint8_t flags = *(node + 0x44);
        uint8_t* next       = *reinterpret_cast<uint8_t**>(node + 0x04);
        if ((flags & 0x10) == 0) {
            void* inner  = *reinterpret_cast<void**>(node + 0x5C);
            void* entity = *reinterpret_cast<void**>(node + 0x40);
            if (inner && entity != player) {
                cb(node, inner);
            }
        }
        node = next;
    }
}

} // namespace

namespace detail {

void npc_apply_interp_for_render_frame(float alpha) {
    if (!g_npc_interp_enabled.load(std::memory_order_relaxed)) return;
    if (!mtr::sim_decouple::effective_is_throttling())         return;

    for (size_t i = 0; i < kMaxNpcSlots; ++i) {
        NpcSlot& s = g_npc_slots[i];
        if (!s.inner || !s.prev.valid || !s.curr.valid || s.teleport) continue;
        __try {
            // Save before writing.
            auto* base = static_cast<uint8_t*>(s.inner);
            std::memcpy(s.saved.pos, base + kPlayerPosOffset, kPlayerPosBytes);
            std::memcpy(s.saved.rot, base + kPlayerRotOffset, kPlayerRotBytes);
            s.save_valid = true;

            PlayerSnap out;
            compose_player_interp(s.prev, s.curr, alpha, out);
            std::memcpy(base + kPlayerPosOffset, out.pos, kPlayerPosBytes);
            std::memcpy(base + kPlayerRotOffset, out.rot, kPlayerRotBytes);
            g_npc_interp_writes.fetch_add(1, std::memory_order_relaxed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s = NpcSlot{};
        }
    }
}

} // namespace detail

// === Public API ============================================================

void pre_sim_restore_npcs() {
    if (!g_npc_interp_enabled.load(std::memory_order_relaxed)) return;
    for (size_t i = 0; i < kMaxNpcSlots; ++i) {
        NpcSlot& s = g_npc_slots[i];
        if (!s.inner || !s.save_valid) continue;
        // SEH around the entity-memory writes — a stale slot (entity
        // already freed and our age-out hasn't fired yet) must not crash
        // the game.
        __try {
            auto* base = static_cast<uint8_t*>(s.inner);
            std::memcpy(base + detail::kPlayerPosOffset, s.saved.pos, detail::kPlayerPosBytes);
            std::memcpy(base + detail::kPlayerRotOffset, s.saved.rot, detail::kPlayerRotBytes);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s = NpcSlot{};
            continue;
        }
        s.save_valid = false;
    }
}

void post_sim_capture_npcs() {
    if (!g_npc_interp_enabled.load(std::memory_order_relaxed)) return;
    const int tick = g_npc_tick_counter.fetch_add(1, std::memory_order_relaxed) + 1;

    // Skip the player node — M4 (interp_player.cpp) covers it, and our
    // dispatch order has player_apply running BEFORE npc_apply, so an
    // unfiltered walk here would let the M5 write overwrite M4's pose.
    void* player = detail::current_player_handle();

    walk_transform_list([&](uint8_t* /*node*/, void* inner) {
        NpcSlot* s = find_or_alloc_npc(inner);
        if (!s) return;
        s->last_seen_tick = tick;
        if (s->curr.valid) {
            s->prev = s->curr;
        }
        __try {
            const auto* base = static_cast<const uint8_t*>(inner);
            std::memcpy(s->curr.pos, base + detail::kPlayerPosOffset, detail::kPlayerPosBytes);
            std::memcpy(s->curr.rot, base + detail::kPlayerRotOffset, detail::kPlayerRotBytes);
            s->curr.qpc   = detail::qpc_now();
            s->curr.valid = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *s = NpcSlot{};
            return;
        }
        // Per-entity teleport: same threshold as player.
        if (s->prev.valid && s->curr.valid) {
            const float dx = s->curr.pos[0] - s->prev.pos[0];
            const float dy = s->curr.pos[1] - s->prev.pos[1];
            const float dz = s->curr.pos[2] - s->prev.pos[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            const float t  = mtr::interp::player_teleport_threshold();
            const bool tp  = d2 > t * t;
            s->teleport = tp;
            if (tp) g_npc_teleports.fetch_add(1, std::memory_order_relaxed);
        }
    }, player);
    age_out_npc_slots(tick);
}

bool npc_interp_enabled() { return g_npc_interp_enabled.load(std::memory_order_relaxed); }
void set_npc_interp_enabled(bool on) {
    const bool prev_v = g_npc_interp_enabled.exchange(on, std::memory_order_relaxed);
    if (prev_v != on) {
        mtr::log::info("interp: npc_interp %s", on ? "enabled" : "disabled");
    }
    if (!on) {
        // Flush any in-flight saves so toggling off cleanly returns
        // every entity to actual sim state.
        pre_sim_restore_npcs();
        // Then clear the slot table so a future re-enable starts fresh.
        for (size_t i = 0; i < kMaxNpcSlots; ++i) g_npc_slots[i] = NpcSlot{};
    }
}
uint64_t npc_interp_writes() { return g_npc_interp_writes.load(std::memory_order_relaxed); }
uint64_t npc_active_slots()  {
    uint64_t n = 0;
    for (size_t i = 0; i < kMaxNpcSlots; ++i) if (g_npc_slots[i].inner) ++n;
    return n;
}
uint64_t npc_teleports_detected() { return g_npc_teleports.load(std::memory_order_relaxed); }

} // namespace mtr::interp
