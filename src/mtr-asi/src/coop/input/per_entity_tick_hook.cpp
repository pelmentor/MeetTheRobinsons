// Per-wilbur-entity tick PRE/POST hook — Step 4 logging-only implementation.
// See mtr/coop/input/per_entity_tick_hook.h for the design rationale and the
// IDA verification record (2026-05-12).

#include "mtr/coop/input/per_entity_tick_hook.h"

#include "mtr/coop/input/controlmapper_dev.h"
#include "mtr/protagonist_registry.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::coop::per_entity_tick_hook {

namespace {

// === Engine VA =============================================================

constexpr uintptr_t kSubscriberWalkerVA = 0x005AD9B0;

// === Trampoline ============================================================
//
// __thiscall(wilbur*) — MinHook surfaces this as __fastcall: ECX = this,
// EDX = unused dummy. Return type is `_DWORD*` (whatever this is, the engine
// returns it back via EAX). We do not consume the value; we just forward.
using PFN_Walker = void* (__fastcall*)(void* this_, void* edx_dummy);
PFN_Walker g_tramp = nullptr;

// === State =================================================================

std::atomic<uint64_t> g_total      {0};
std::atomic<uint64_t> g_p1_fires   {0};
std::atomic<uint64_t> g_p2_fires   {0};
std::atomic<uint64_t> g_unk_fires  {0};

// Throttle: log first kFirstLogPerIdx fires per (idx 0, idx 1) verbatim,
// then a heartbeat every kHeartbeatPeriod fires per idx.
constexpr uint64_t kFirstLogPerIdx  = 20;
constexpr uint64_t kHeartbeatPeriod = 600;  // ~10s of sim at 60 Hz per wilbur

// === Hook body =============================================================

void* __fastcall hook_walker(void* this_, void* edx_dummy) {
    const uint32_t entity = reinterpret_cast<uint32_t>(this_);
    const int      idx    = mtr::protagonist_registry::player_idx_for(entity);

    g_total.fetch_add(1, std::memory_order_relaxed);

    if (idx == 0) {
        const uint64_t n = g_p1_fires.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= kFirstLogPerIdx || (n % kHeartbeatPeriod) == 0) {
            mtr::log::info("[petk] entity=0x%08X idx=0 (P1) fire#%llu",
                           entity, static_cast<unsigned long long>(n));
        }
    } else if (idx == 1) {
        const uint64_t n = g_p2_fires.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= kFirstLogPerIdx || (n % kHeartbeatPeriod) == 0) {
            mtr::log::info("[petk] entity=0x%08X idx=1 (P2) fire#%llu",
                           entity, static_cast<unsigned long long>(n));
        }
    } else {
        // idx < 0: not a registered wilbur. This is hit by other entity
        // types whose tick path also uses sub_5AD9B0 (scene singletons,
        // FE-loader components). The swap below is gated on idx >= 0, so
        // those entities pass through with dev_p1 untouched.
        const uint64_t n = g_unk_fires.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 5) {
            mtr::log::info("[petk] entity=0x%08X idx=-1 (UNKNOWN) fire#%llu",
                           entity, static_cast<unsigned long long>(n));
        }
    }

    // Step 5 (2026-05-12) — MTA SwitchContext analogue. PRE-swap if this
    // wilbur is a registered P1/P2; POST-restore to player 0 inside a
    // __finally so a fault in the trampoline can't leave instance+4
    // pinned to dev_p2. Audit fix 2026-05-12 (88% MTA-fidelity + 82%
    // re-entrancy): POST gated on idx >= 0 to match PRE (so non-wilbur
    // entities never spuriously write instance+4). Reference:
    // reference/mtasa-blue/Client/multiplayer_sa/multiplayer_keysync.cpp:584
    // (ReturnContextToLocalPlayer) + :595-624 (abort path restore).
    void* result = nullptr;
    if (idx >= 0) {
        mtr::coop::controlmapper_dev::swap_to_player(idx);
        __try {
            result = g_tramp(this_, edx_dummy);
        } __finally {
            mtr::coop::controlmapper_dev::swap_to_player(0);
        }
    } else {
        // idx < 0: no swap, no restore. Pass through with engine's
        // current dev (always dev_p1 outside a P1/P2 swap window).
        result = g_tramp(this_, edx_dummy);
    }
    return result;
}

} // anonymous namespace

bool install() {
    static bool installed = false;
    if (installed) return true;

    void* target = reinterpret_cast<void*>(kSubscriberWalkerVA);
    void* tramp  = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hook_walker), &tramp)
            != MH_OK) {
        mtr::log::info("[petk] install FAILED: MH_CreateHook on "
                       "sub_5AD9B0 @ 0x%08X (wilbur+0xD04 subscriber walker)",
                       kSubscriberWalkerVA);
        return false;
    }
    g_tramp = reinterpret_cast<PFN_Walker>(tramp);

    if (MH_EnableHook(target) != MH_OK) {
        mtr::log::info("[petk] install FAILED: MH_EnableHook on "
                       "sub_5AD9B0 @ 0x%08X", kSubscriberWalkerVA);
        return false;
    }

    installed = true;
    mtr::log::info("[petk] installed. PRE/POST-hook on sub_5AD9B0 @ 0x%08X "
                   "(wilbur+0xD04 subscriber-list walker). Per-entity-tick "
                   "P1/P2 dev swap active (MTA SwitchContext analogue); "
                   "PRE calls controlmapper_dev::swap_to_player(idx), POST "
                   "restores idx=0 inside __finally for SEH safety.",
                   kSubscriberWalkerVA);
    return true;
}

Stats stats() {
    Stats s;
    s.total_fires   = g_total    .load(std::memory_order_relaxed);
    s.fires_p1      = g_p1_fires .load(std::memory_order_relaxed);
    s.fires_p2      = g_p2_fires .load(std::memory_order_relaxed);
    s.fires_unknown = g_unk_fires.load(std::memory_order_relaxed);
    return s;
}

} // namespace mtr::coop::per_entity_tick_hook
