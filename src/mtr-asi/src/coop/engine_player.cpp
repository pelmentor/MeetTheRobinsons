// See mtr/coop/engine_player.h for rationale.

#include "mtr/coop/engine_player.h"

#include <windows.h>

#include <cstdint>

namespace mtr::coop::engine_player {

namespace {

// engine_wilbur lives at *(0x728A40 + 4). The base 0x728A40 is the
// player-control-manager singleton; slot +4 inside it is the "current
// player entity" pointer (the engine's term for what we call wilbur).
// Captured 2026-05-11 (Phase 0C). Don't move this constant — it's the
// last engine-wrapping detail in this module.
constexpr uintptr_t kPlayerCtrlMgrVA = 0x00728A40;

} // anon

void* engine_wilbur_ptr() noexcept {
    __try {
        return *reinterpret_cast<void**>(kPlayerCtrlMgrVA + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace mtr::coop::engine_player
