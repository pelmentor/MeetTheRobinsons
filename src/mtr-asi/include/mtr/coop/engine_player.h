// Engine-wrapper accessor for the engine's "wilbur" entity (the local player).
//
// Why this exists (architectural principle #7):
//   Phase 1.2 audit (2026-05-12) flagged that `kPlayerCtrlMgrVA = 0x728A40`
//   plus its SEH-wrapped read had duplicated copies in both
//   `coop_spawn_probe.cpp` (engine-wrapper layer) and `remote_player_manager
//   .cpp` (gameplay/network layer). Principle #7 wants engine layout
//   knowledge confined to the engine-wrapper subtree; gameplay code should
//   talk to it through a clean accessor. This header is that accessor.
//
// Surface is intentionally tiny — one function. If more engine_wilbur-side
// state is needed later (e.g. pos+rot, mode subclass, control mapper ptr),
// extend this module rather than re-introducing the raw VA dereference
// elsewhere.

#pragma once

namespace mtr::coop::engine_player {

// Read the engine's wilbur entity pointer from its well-known VA.
// Returns nullptr if the slot isn't populated yet (engine still booting)
// or if the read itself faults. SEH-wrapped internally; safe to call from
// any thread that the sim already tolerates.
void* engine_wilbur_ptr() noexcept;

} // namespace mtr::coop::engine_player
