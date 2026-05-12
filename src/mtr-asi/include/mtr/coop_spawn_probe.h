// Coop spawn probe — Phase 0C derisk experiment. SHIPPED + GREEN (2026-05-11).
//
// What it does: calls `entity_factory_construct` (0x5B96F0) from the mod
// with the engine's exact wilbur bag (`model_name=avatars/wilbur_low` +
// `class=wilbur`), observes all 9 breadcrumb hooks firing, then tears
// down the spawned orphan via `vtable[0](entity, 1)` so the engine stays
// stable. Verified by autonomous test loop end-to-end (load-save-1 exits
// 0 with result=pass).
//
// What it proves: the factory IS the gate for spawning a second player
// entity in coop, the required bag is just 2 KVs (not 45 like NPCs),
// and the engine accepts our spawn + teardown cleanly. This is the
// foundation for coop Phase 1 (transport) and Phase 2 (input routing).
//
// What it does NOT do (= future Phase 2 work):
//   - No networking.
//   - No input binding.
//   - No persistent orphan (entity is destroyed before next sim tick).
//     Keep-alive crashes ~150ms post-spawn inside sub_5CB160 — see
//     research/findings/coop-phase-0b-breadcrumb-trail-2026-05-10.md
//     "Phase 0C-step-2j" for the captured crash chain.
//
// Decision rationale + full step trail: see
//   research/findings/coop-phase-0b-breadcrumb-trail-2026-05-10.md
//   research/findings/coop-phase-0a-audit-2026-05-10.md (Option A)
//
// Public API:
//   install()       — install permanent diagnostic hooks + VEH; call once
//                     from dllmain after MinHook init. Idempotent.
//   try_spawn_p2()  — one-shot factory call + observation + teardown.
//                     Returns true if the factory returned non-null;
//                     the spawned entity is destroyed before returning.
//   last_result()   — read the last attempt's ProbeResult for menu display.

#pragma once

#include <cstdint>

namespace mtr::coop_spawn_probe {

// Per-attempt result. Populated by try_spawn_p2(); read by the menu.
struct ProbeResult {
    bool        attempted        = false;
    bool        succeeded        = false;   // factory returned non-null
    void*       entity           = nullptr; // factory return
    int         list_count_before = 0;      // dword_724DE4 walker count BEFORE
    int         list_count_after  = 0;      // dword_724DE4 walker count AFTER
    int         list_delta        = 0;      // after - before (1 = active path)
    // Bag descriptor dwords after bag_init_from_template_THUNK returns.
    // Engine pattern (sub_43D167 layout, verified 2026-05-10): the descriptor
    // is a 12-byte struct where the factory's bag-arg points at offset +0 and
    // bag_init writes the head pointer to offset +8. Slots logged in linear
    // memory order so the head pointer should appear in slot2_after_init.
    void*       slot0_after_init  = nullptr; // descriptor +0 (factory arg base)
    void*       slot1_after_init  = nullptr; // descriptor +4 (engine fills?)
    void*       slot2_after_init  = nullptr; // descriptor +8 (head pointer slot)

    // === Phase 0B breadcrumb ladder ========================================
    //
    // Each flag fires when the corresponding engine fn is entered/returned
    // during the factory call. Reading them as a sequence localizes the
    // crash to a 1-step interval in the factory body. Order of fires for
    // a successful spawn (with annotated VAs):
    //
    //   0. registry_pre     (sub_5A04F0 entered  — class registry lookup;
    //                        proves the bag accessor returned a class string)
    //   1. ctor_pre         (sub_5B71C0 entered  — protagonist ctor wrapper)
    //   2. ctor_post        (sub_5B71C0 returned — entity allocated+init'd)
    //   3. merge_pre        (sub_4B95A0 entered  — bag_merge_into)
    //   4. register_active  (sub_5AD410 entered  — active scene path) OR
    //      register_queued  (sub_5AD3E0 entered  — queued path)
    //   5. post_init_reached (sub_55AF00 entered — final post-init step)
    //
    // post_init_reached is also retained from the pre-Phase-0B version of
    // this struct so callers don't need to migrate.
    bool        registry_pre      = false;  // sub_5A04F0 entered
    void*       registry_class_arg = nullptr; // class string passed to lookup
    bool        ctor_pre          = false;  // sub_5B71C0 entered
    bool        ctor_post         = false;  // sub_5B71C0 returned (no exception in ctor)
    void*       ctor_returned_ptr = nullptr; // entity ptr returned by ctor
    bool        merge_pre         = false;  // bag_merge_into entered
    // Phase 0C-step-1 (2026-05-10): vtable[10]=sub_5B7010 calls sub_5B1E10
    // first; if the factory crashes between transform_setup POST and
    // register_active/queued, the fault is somewhere in sub_5B7010. These
    // two flags localize it: PRE fires when sub_5B1E10 is entered, POST when
    // it returns successfully. PRE=1 POST=0 → fault inside sub_5B1E10 body.
    // PRE=1 POST=1 → fault is later in sub_5B7010 (the alloc/sub_5794D0
    // post-step or sub_5B6E80 dispatch).
    bool        actor_init_pre    = false;  // sub_5B1E10 entered
    bool        actor_init_post   = false;  // sub_5B1E10 returned
    // Phase 0C-step-2a: sub_5BBD10 is the first fault site inside actor_init.
    // It derefs *(a1 + 164) without a null-check, so when entity+0x1EC = NULL
    // (our case — primary bag chain not seeded), the call faults at address
    // 0xA4. We hook with a NULL-arg short-circuit that returns 0 (mirroring
    // the engine's own early returns on null branches inside sub_5BBD10).
    // bypass_fired = the hook fired AND short-circuited. If actor_init_post
    // then becomes 1, the bypass alone is sufficient; if not, there are more
    // fault sites inside actor_init.
    bool        bbd10_pre              = false;  // sub_5BBD10 entered (in probe scope)
    void*       bbd10_arg              = nullptr; // arg passed (= entity+0x1EC value)
    bool        bbd10_null_bypass_fired = false; // hook short-circuited because arg=NULL
    bool        register_active   = false;  // sub_5AD410 entered
    bool        register_queued   = false;  // sub_5AD3E0 entered
    bool        post_init_reached = false;  // sub_55AF00 PRE-logger fired during call
    void*       post_init_v13_arg = nullptr; // value passed to sub_55AF00 as 2nd arg

    char        screen_name[64]   = {0};    // gate state at attempt time
    char        message[256]      = {0};    // human-readable summary for UI
};

// Install hooks. Now installs the actor_init (sub_5B1E10) hook permanently
// at boot for Phase 0C-step-2c global logging — see set_actor_init_global_log
// below. Idempotent. Call once from dllmain after MinHook is initialized.
bool install();

// One-shot probe: build the bag, call the factory, observe.
// Returns true if the factory returned non-null (entity constructed),
// false on gate failure / exception / null return.
//
// Safe to call from the main thread (e.g. a Debug-tab button handler).
// The entire factory call is SEH-wrapped — an access violation inside the
// engine code is caught and reported via last_result().
bool try_spawn_p2();

// Read the last attempt's result. Always returns a valid struct (zero-
// initialized if no attempt yet — ProbeResult::attempted == false).
ProbeResult last_result();

// Phase 0C-step-2c (2026-05-10): toggle the global-mode actor_init logger.
// When enabled, every call to sub_5B1E10 OUTSIDE the probe window logs a
// line capturing the entity ptr, entity+0x1EC value (bag chain head), and
// caller return address. Rate-limited to a fixed number of lines per
// session (the line count resets when the toggle flips on). Default ON at
// install() — disable from a UI button if logging gets noisy.
//
// Goal: disambiguate "engine's normal primary-actor spawn ALSO goes through
// sub_5B1E10 but with non-NULL entity+0x1EC" (= we need to seed the bag
// chain) vs "engine's spawn doesn't go through this fn at all" (= there's
// a separate entry point for the player at level load).
void set_actor_init_global_log(bool enabled);

// Read the current count of global-mode log lines emitted. Useful for menu
// status display.
int actor_init_global_log_count();

// Gameplay-state gate. True when the engine's entity manager exists AND
// entity_lookup_by_name_retry("player", 1) returns non-null, meaning the
// engine has allocated the local player and a level is loaded.
//
// Used as the precondition for try_spawn_p2(); also exposed publicly so
// the Phase 1.4d auto-spawn path in MtrPlayerManager::do_pulse can poll
// it from the sim thread without re-implementing the lookup. SEH-wrapped
// internally — safe to call even if the entity manager is in a transient
// inconsistent state during level transitions.
bool is_in_gameplay();

} // namespace mtr::coop_spawn_probe
