// Per-site route for VibrateJoystick on the orphan — strategy (b) per
// principle 4 (targeted fix, not broad suppression).
//
// Hooks VibrateJoystick::vt[13] at 0x00532B40 PRE. If the wrapper's owner
// (`*(this+4)`) is the live orphan entity, early-return without calling
// the trampoline. Otherwise pass through.
//
// **Why per-site is correct here, not a "skip-if" workaround:**
// VibrateJoystick is local-hardware-feedback (gamepad rumble). A remote
// player has no local controller — the gamepad lives on another machine.
// Running the engine's vibrate routine for a remote player is semantically
// nonsense. The MTA precedent is the same: CCamera::SetNewPlayerWeaponMode
// is byte-patched to 0xC3 (ret) for remote ped ticks (see
// reference/mtasa-blue/Client/multiplayer_sa/multiplayer_keysync.cpp).
//
// **Why not strategy (a) or (d):**
// The vt[13] body's downstream call chain `sub_51F960 -> sub_51EB40 -> ...`
// goes through SecuROM stolen-byte thunks that resolve at runtime to a DLL
// target (0x10059D0 from IAT slot 0xF8E718). The intermediate
// "id -> name" registry that needs an entry for the orphan's id (0x21B)
// lives behind that thunk — static RE is blocked. Three-agent consensus on
// 2026-05-12: strategy (b) is the only principle-4-clean fix shippable
// without weeks of runtime-only RE.

#pragma once

#include <cstdint>

namespace mtr::coop_vibrate_route {

bool install();

struct Stats {
    uint32_t total_calls            = 0;  // vt[13] invocations seen
    uint32_t routed_to_orphan       = 0;  // calls early-returned (owner == orphan)
    uint32_t passed_through         = 0;  // calls that ran the trampoline
    uint32_t owner_read_faults      = 0;  // SEH faults reading *(this+4)
};
Stats stats();

} // namespace mtr::coop_vibrate_route
