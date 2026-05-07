// Direct level transition: queue any level by name via the engine's state
// machine. Reverse-engineered chain (verified from disasm of sub_45D880,
// sub_524380, sub_5A0400):
//
//   sub_5A0400("TargetGameLevel")  -> string ptr (current target)
//   sub_524380(state_mgr, name)    -> writes name into state_mgr+8 and
//                                     dword_741868[263]; engine's main loop
//                                     reads from there next tick and either
//                                     transitions to a UI screen by that name
//                                     OR (when no matching screen) treats it
//                                     as a level/world name and loads.
//   *(void**)0x728A30              -> the state-machine singleton pointer.
//
// We use the engine's actual setter and engine's state-machine instance.
// No bypassing of save state, no progression-flag tampering, no fake
// dialogs. The same path the game uses internally when the user presses
// "Continue" on the game-select screen.

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::level_select {

namespace {

constexpr uintptr_t kStateMgrPtrVA   = 0x00728A30;  // void** -> state machine
constexpr uintptr_t kSetNextStateVA  = 0x00524380;  // sub_524380(this, name)

// __thiscall via __fastcall trick.
using PFN_SetNextState = int (__fastcall*)(void* this_, void* /*edx*/, const char* name);

void* state_machine() {
    return *reinterpret_cast<void**>(kStateMgrPtrVA);
}

} // namespace

bool ready() {
    return state_machine() != nullptr;
}

bool queue_state(const char* name) {
    void* sm = state_machine();
    if (!sm || !name) return false;
    auto fn = reinterpret_cast<PFN_SetNextState>(kSetNextStateVA);
    fn(sm, nullptr, name);
    mtr::log::info("level_select: queued next state \"%s\" via sub_524380(%p)", name, sm);
    return true;
}

// Hand-curated from Game/data/WorldDatabase.txt. These are the *.scn /
// world directory names recognised by the engine. Strings are mtr-asi
// statics so they outlive any single use.
struct Level {
    const char* category;
    const char* display;
    const char* name;
};
static const Level kLevels[] = {
    // Act 1 - Robinson House / Today
    {"Act 1", "Robinson House",            "a1_robinson"},
    {"Act 1", "Robinson House Exterior",   "a1_robinsonHouse_ext"},
    {"Act 1", "Robinson House Storage",    "a1_robinson_storage"},
    {"Act 1", "Robinson Train Room",       "a1_robinson_trainroom"},
    {"Act 1", "Egypt",                     "a1_egypt"},
    {"Act 1", "Science Fair",              "a1_sciencefair"},
    {"Act 1", "Sub-Basement 1",            "a1_subbasement"},
    {"Act 1", "Sub-Basement 2",            "a1_subbasement2"},
    {"Act 1", "Sub-Basement 3",            "a1_SubBasement3"},
    {"Act 1", "Sub-Basement Boss",         "a1_subbasement_boss"},

    // Act 2 - Future / Bowler Hat Guy
    {"Act 2", "Alt Future",                "a2_altfuture"},
    {"Act 2", "Alt Future Warehouse",      "a2_altfuture_warehouse"},
    {"Act 2", "Alt Future Warehouse 2",    "a2_altfuture_warehouse2"},
    {"Act 2", "Alt Future Robinson Ext",   "a2_altfuture_robinsonext"},
    {"Act 2", "Lizzy",                     "a2_lizzy"},
    {"Act 2", "Lizzy Boss",                "a2_lizzy_boss"},
    {"Act 2", "Prometheus",                "a2_prometheus"},
    {"Act 2", "Old Town",                  "a2_oldtown"},
    {"Act 2", "Old Town 2",                "a2_oldtown2"},
    {"Act 2", "Magma",                     "a2_magma"},
    {"Act 2", "Magma Interior",            "a2_magma_interior"},
    {"Act 2", "Robinson (Act 2)",          "a2_robinson"},

    // Act 3 - Industries
    {"Act 3", "Industries",                "a3_industries"},
    {"Act 3", "Robinson (Act 3)",          "a3_robinson"},

    // Mini-games
    {"Minigame", "Dig Dug",                "digdug"},
    {"Minigame", "Charge Ball",            "chargeball"},

    // Special
    {"Special", "Action Figure Viewer",    "actionfigureviewer"},
    {"Special", "Frontend",                "frontend"},
    {"Special", "Actor Test",              "actortest"},
};

const Level* levels(size_t* out_count) {
    if (out_count) *out_count = sizeof(kLevels) / sizeof(kLevels[0]);
    return kLevels;
}

} // namespace mtr::level_select
