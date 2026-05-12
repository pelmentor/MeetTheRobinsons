// Direct save-system API — bypass the menu layer per RULE №1.
//
// The engine's save subsystem is driven by a state-machine pump at
// `sub_575D60` (0x00575D60), which the engine itself spawns as a
// per-operation thread (CreateThread call site at runtime address
// ~0x02100180, in the SecuROM-decompressed area). The pump reads a
// request opcode from a state buffer pointed to by `unk_72F824[0]` and
// dispatches: case 4=save, case 5=load, case 0xC=enumerate-slots, etc.
//
// Implementation strategy: set the state, spawn our own thread that
// runs the engine's pump function directly, wait for the done flag.
// No DIK injection, no menu nav. Hooks into the engine's intended
// save-system API at the LOWEST machine-readable layer.
//
// Caveats (all empirical, see save_system.cpp):
//  - Some popups in the load handler (sub_575090) are gated by skip
//    flags we set; others fire on actual errors and would block. For
//    a valid save with no autosave-conflict, the happy path is
//    popup-free.
//  - After load completes the engine state has the save data BUT no
//    gameplay screen is pushed yet. resume_gameplay() handles the
//    follow-up transition (TBD — separate RE pass).

#pragma once

namespace mtr::save_system {

// Load the save in the specified slot (0-indexed: slot 0 = first save).
// Blocks the calling thread until the engine's pump signals completion
// (or timeout, default 10s). Spawns an internal worker that runs the
// save-system pump, so it's safe to call from the main thread.
//
// Returns true on success (engine reported result code 0). Returns
// false on:
//   - Already-in-progress collision
//   - Worker thread spawn failure
//   - Pump timeout (engine didn't signal done within 10s)
//   - Non-zero result code from the engine (slot empty, file
//     corrupt, autosave conflict that we couldn't suppress, etc.)
//
// On error, check the log for the result code (`save_system: load_slot
// (N) result=K`). Codes are engine-internal — not yet mapped 1:1.
bool load_slot(int slot_idx);

// Returns true if a load is currently in progress (worker pump still
// running). Useful for serializing back-to-back load requests.
bool load_in_progress();

}  // namespace mtr::save_system
