// Player-entity registry — Phase 2 step (b1+b2) infrastructure.
//
// The engine has NO built-in player_idx field on the player entity (verified
// by full ctor-chain audit; see research/findings/coop-phase0-input-
// separation-point-2026-05-11.md "Phase 2 step (b1)" section). Instead of
// adding a field, we maintain a mod-owned side-table.
//
// IMPORTANT — class semantics correction (live-test 2026-05-11): the actual
// gameplay player class is `wilbur` (allocator 0x48D8A0, 3,404 bytes), NOT
// `protagonist` (0x5B71C0, 3,276 bytes). The protagonist class is registered
// in the engine's class registry but never spawned during normal gameplay or
// save-load. The registry hook therefore targets the wilbur allocator wrapper
// at 0x48D8A0 (POST-return) — entity_factory_construct dispatches to it via
// class_entry->vtable[+4] for any bag-class="wilbur" call.
//
//   - `wilbur_alloc_and_ctor_3404b` (0x48D8A0) is hooked POST-return; the
//     returned wilbur instance pointer is appended to a small array (cap=2).
//   - `player_idx_for(this_ptr)` linearly searches and returns 0 for the
//     first-registered wilbur, 1 for the second, -1 if unknown.
//
// First-registered = P1; second = P2. The engine currently constructs exactly
// one wilbur (single-player gameplay). The test_harness's try_spawn_p2 also
// hits this path with class=wilbur, producing an orphan that gets immediately
// torn down — its slot is currently NOT freed (no dtor hook yet); the cap=2
// is large enough that this is harmless for current testing. Phase 2 step
// (b7) introduces a real P2 via factory call with class=wilbur and the
// registry picks it up automatically.
//
// Defaults: hook installed unconditionally at boot, but registration logging
// is gated on cmdline flag `-mtrasi-protag-registry-log` to keep the log
// quiet by default.

#pragma once

#include <cstdint>

namespace mtr::protagonist_registry {

// Snapshot of the registry state. Always returns a stable copy so the caller
// is not racing with concurrent register/unregister calls.
struct Observation {
    int      count                 = 0;     // # of currently-registered instances
    uint32_t instance[2]           = {0};   // slot 0 = P1, slot 1 = P2
    uint64_t total_registrations   = 0;     // lifetime registrations (incl. churn)
    uint64_t total_unregistrations = 0;     // lifetime unregistrations
};

// Install both hooks (ctor POST + dtor PRE) via MinHook. Idempotent.
// Returns true on success.
bool install();

// Manual register/unregister API — primarily for testing. The hooks call
// these automatically during normal operation.
void register_instance(uint32_t this_ptr);
void unregister_instance(uint32_t this_ptr);

// Lookup: returns the player_idx (0 or 1) for a registered protagonist
// instance, or -1 if not registered.
int  player_idx_for(uint32_t this_ptr);

// Snapshot for diagnostics / menu display.
Observation observation();

} // namespace mtr::protagonist_registry
