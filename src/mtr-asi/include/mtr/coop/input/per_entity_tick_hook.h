// Per-wilbur-entity tick PRE/POST hook — Phase 1.6 step 4.
//
// Hooks the wilbur+0xD04 subscriber-list walker at sub_5AD9B0 (0x005AD9B0).
// IDA-verified 2026-05-12:
//   - __thiscall(wilbur*) — ECX = wilbur entity pointer at entry.
//   - 8 clean prologue bytes (push esi; mov esi,ecx; call sub_5B39C0).
//   - Reads *(this+0xD04) as the component-chain head and iterates with
//     `component->vtable[13](component)` per node.
//   - Single caller at 0x00552500 (which is itself a small __thiscall(wilbur*)
//     parent). One PRE-hook fire per wilbur-component-tick per sim frame.
//
// Step 4 (this file): logging only. Identifies P1 and P2 per-tick invocations
// by consulting protagonist_registry::player_idx_for(ECX). Throttled log
// output so a long run does not flood the log.
//
// Step 5+ will add the controlmapper_dev::swap_to_player(idx) call in PRE
// and a restore to player 0 in POST — the MTA SwitchContext analogue. See
// reference/mtasa-blue/Client/multiplayer_sa/multiplayer_keysync.cpp:325.

#pragma once

#include <cstdint>

namespace mtr::coop::per_entity_tick_hook {

// Install the PRE/POST hook on sub_5AD9B0. Idempotent. Returns true on success.
bool install();

// Diagnostic counters (lifetime totals since install).
struct Stats {
    uint64_t total_fires       = 0;  // total PRE-hook invocations seen
    uint64_t fires_p1          = 0;  // ECX == wilbur registered as idx=0
    uint64_t fires_p2          = 0;  // ECX == wilbur registered as idx=1
    uint64_t fires_unknown     = 0;  // ECX not in protagonist_registry (idx < 0)
};
Stats stats();

} // namespace mtr::coop::per_entity_tick_hook
