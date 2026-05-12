// Per-site route for VibrateJoystick::vt[13] on the orphan. See header for
// the principle-4 reasoning. Three-agent design consensus 2026-05-12.

#include "mtr/coop_vibrate_route.h"
#include "mtr/coop/remote_player_manager.h"
#include "mtr/coop/remote_player.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::coop_vibrate_route {

namespace {

// === Engine VAs ============================================================
//
// VibrateJoystick wrapper's vt[13] body. Audited at b7.10 row 28 (vtable
// 0x006C1AB0); empirically confirmed as the C1 culprit on 2026-05-12 (live
// test with -mtrasi-coop-confirm-vibratejoystick). The body's first
// instructions (`push ecx; push esi; mov esi, ecx; mov eax, [esi+4]; ...`)
// give a clean MinHook target — function start at 0x00532B40 is preceded by
// `CC CC` alignment filler at 0x00532B3E, so the prologue is safe to patch.
constexpr uintptr_t kVibrateJoystickTickVA = 0x00532B40;

// Offset within the wrapper at which the owner-entity pointer lives. Read by
// the engine body itself at instruction 0x00532B44 (`mov eax, [esi+4]`).
// Confirmed via filter's read of the same offset for orphan unlink.
constexpr uintptr_t kWrapperOwnerOffset = 0x04;

// === Hook signature ========================================================
//
// vt[13] is __thiscall(this) returning a small thing (the disasm shows
// `pop ecx; ret` at the function tail). MinHook PRE-trampoline gets this
// via __fastcall: ECX=this, EDX=dummy.
using PFN_Vt13 = void (__fastcall*)(void* this_, void* edx_dummy);

PFN_Vt13 g_tramp = nullptr;

// === SEH-guarded owner read ================================================
//
// Reads `*(this + 4)` defensively. The wrapper is heap-allocated; the offset
// is well-known; in practice this never faults — but the filter already
// SEH-wraps the equivalent read, and consistency is worth one __try.

bool read_owner_seh(void* wrapper, uint32_t* out_owner) {
    __try {
        const auto base = reinterpret_cast<uintptr_t>(wrapper);
        *out_owner = *reinterpret_cast<uint32_t*>(base + kWrapperOwnerOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// === Stats =================================================================

std::mutex g_stats_mu;
Stats      g_stats{};

// === Hook body =============================================================

void __fastcall hook_vt13(void* this_, void* edx_dummy) {
    uint32_t call_idx = 0;
    {
        std::lock_guard<std::mutex> lk(g_stats_mu);
        ++g_stats.total_calls;
        call_idx = g_stats.total_calls;
    }

    // Resolve owner.
    uint32_t owner = 0;
    const bool owner_ok = read_owner_seh(this_, &owner);
    if (!owner_ok) {
        std::lock_guard<std::mutex> lk(g_stats_mu);
        ++g_stats.owner_read_faults;
        // Pass through — engine handles its own state; we don't get to skip
        // on a read fault because we don't know whether this is the orphan.
        g_tramp(this_, edx_dummy);
        return;
    }

    // Phase 1.4d (2026-05-12): route through the authoritative wrapper
    // registry. MtrPlayerManager owns the orphan lifecycle now; ask it
    // directly whether `owner` belongs to a known wrapper, and whether
    // that wrapper is a Remote. This replaces the prior probe-internal
    // `live_orphan_entity()` cache which only knew about the manual
    // -mtrasi-coop-keep-orphan path and returned NULL in the
    // Phase 1.4d session-active auto-spawn case (audit 2026-05-12,
    // 95% confidence). The manager-route also satisfies Principle 7:
    // the wrapper layer owns the wrapper-state surface; per-site
    // routes consult that surface rather than maintaining a parallel
    // probe-internal cache (audit 2026-05-12, 82% confidence,
    // RULE №2 dedup).
    void* owner_ptr = reinterpret_cast<void*>(owner);
    auto* rp = mtr::coop::MtrPlayerManager::instance().by_engine_entity(owner_ptr);
    const bool is_orphan = rp != nullptr
                            && rp->role() == mtr::coop::MtrRemotePlayer::Role::Remote;
    if (is_orphan) {
        std::lock_guard<std::mutex> lk(g_stats_mu);
        ++g_stats.routed_to_orphan;
        // One-shot loud log so the trace shows when the route engages.
        static std::atomic<bool> g_first_route_logged{false};
        bool expected = false;
        if (g_first_route_logged.compare_exchange_strong(expected, true)) {
            mtr::log::info("[coop_vibrate_route] ROUTED first time call #%u"
                           " wrapper=%p owner=0x%08X (registered Remote in"
                           " MtrPlayerManager). VibrateJoystick short-circuited"
                           " for the remote-player ghost.",
                           call_idx, this_, owner);
        }
        return;  // Early return — DO NOT call trampoline.
    }

    // Not the orphan — pass through.
    {
        std::lock_guard<std::mutex> lk(g_stats_mu);
        ++g_stats.passed_through;
    }
    g_tramp(this_, edx_dummy);
}

} // namespace

bool install() {
    static bool installed = false;
    if (installed) return true;

    void* target = reinterpret_cast<void*>(kVibrateJoystickTickVA);
    void* tramp  = nullptr;

    if (MH_CreateHook(target, reinterpret_cast<void*>(&hook_vt13), &tramp) != MH_OK) {
        mtr::log::info("[coop_vibrate_route] MH_CreateHook(VibrateJoystick::vt[13]"
                       " @ 0x%08X) FAILED", kVibrateJoystickTickVA);
        return false;
    }
    g_tramp = reinterpret_cast<PFN_Vt13>(tramp);

    if (MH_EnableHook(target) != MH_OK) {
        mtr::log::info("[coop_vibrate_route] MH_EnableHook(VibrateJoystick::vt[13])"
                       " FAILED");
        return false;
    }

    installed = true;
    mtr::log::info("[coop_vibrate_route] installed. PRE hook on 0x%08X. Routes"
                   " VibrateJoystick::vt[13] to early-return when called on a"
                   " Remote wrapper owned by MtrPlayerManager (the orphan).",
                   kVibrateJoystickTickVA);
    return true;
}

Stats stats() {
    std::lock_guard<std::mutex> lk(g_stats_mu);
    return g_stats;
}

} // namespace mtr::coop_vibrate_route
