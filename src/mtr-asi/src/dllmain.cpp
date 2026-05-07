#include <windows.h>
#include <MinHook.h>
#include "mtr/version.h"

namespace mtr::log      { void init(); void info(const char* fmt, ...); void shutdown(); }
namespace mtr::cmdline  { void install(); }
namespace mtr::d3d9hook { void install(); }
namespace mtr::aspect   { void init(); }
namespace mtr::menu     { void shutdown(); }
namespace mtr::console     { bool install(); }
namespace mtr::screen_push { bool install(); }
namespace mtr::input_hook  { void install(); }
namespace mtr::dinput_hook { void install(); }
namespace mtr::cvar_dump   { bool install(); }
namespace mtr::vis_test_probe { bool install(); }
namespace mtr::scene_vis_log  { bool install(); }
namespace mtr::ui_aspect_rules { void install_defaults(); }
namespace mtr::sprite_probe   { bool install(); }
namespace mtr::sim_decouple   { void install(); }
namespace mtr::interp         { void install(); }

static HMODULE g_self = nullptr;

// Exposed so other TUs (config-file loaders) can resolve paths relative to
// the ASI's location (Game/) rather than the EXE or current working dir.
namespace mtr { HMODULE self_module() { return g_self; } }

static DWORD WINAPI mtr_init(LPVOID) {
    mtr::log::info("mtr-asi v%s init thread (pid=%lu)", mtr::kModVersion, GetCurrentProcessId());
    mtr::log::info("display path: -dxresolution rewritten to native; D3D9 hooks for menu only");
    mtr::d3d9hook::install();
    // After d3d9 install: rr01 (console_printf address space) is decompressed
    // because the SecuROM stub ran before WinMain, before d3d9 was even loaded.
    mtr::console::install();
    mtr::screen_push::install();
    mtr::input_hook::install();
    mtr::dinput_hook::install();
    // vis_test probe: patches the SecuROM thunk's IAT slot at 0xF92F34. Must
    // be installed AFTER rr01 unpack (after d3d9hook::install which waits
    // for d3d9.dll, by which time the SecuROM stub has resolved the slot).
    mtr::vis_test_probe::install();
    // Scene-visibility tracker: hooks scene_set_visible + script_set_instance_hidden
    // for runtime corner-cull diagnostics. No SecuROM dependency; install order is
    // not critical, but we install after vis_test_probe for symmetry with the
    // other diagnostic modules.
    mtr::scene_vis_log::install();
    // Sprite-batcher list probe (Phase 3 M3.2). Now uses CALL-SITE detour
    // (5-byte rewrite at 0x4D23BF) instead of MinHook prologue trampoline,
    // which previously caused a black-screen freeze. Off by default —
    // the wrapper just forwards to render_sprite_batcher when not armed.
    mtr::sprite_probe::install();
    // Seed default per-screen UI aspect rules so the "Auto from rules" path in
    // sprite_matrix has sensible baseline coverage out-of-the-box (no manual
    // table editing required for the common menu/loading/mini-game screens).
    mtr::ui_aspect_rules::install_defaults();
    // Sim/render decouple module (M0+M1.1): registers state + UI; the
    // simulation_tick_aggregator + pathcam hooks wire up in M1.2/M1.3.
    mtr::sim_decouple::install();
    // Interp snapshot infra (M2). Hooks camera_apply_all_active POST to
    // capture prev/curr view+world matrices on each fresh sim tick.
    // Telemetry-only until M3 view-interp client lands.
    mtr::interp::install();
    mtr::log::info("init thread done -- waiting for d3d9 device, then press Insert (menu) / F2 (console) in-game");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID lpvReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = h;
        DisableThreadLibraryCalls(h);

        // Init log + cmdline hook synchronously: CRT calls GetCommandLineA before
        // any thread we spawn here gets a chance to run. Must hook before that.
        mtr::log::init();
        mtr::log::info("mtr-asi v%s DllMain attach (pid=%lu)", mtr::kModVersion, GetCurrentProcessId());
        if (MH_Initialize() != MH_OK) {
            mtr::log::info("MH_Initialize failed in DllMain");
        } else {
            mtr::cmdline::install();
            // Cvar registration hooks must be armed before the game's WinMain
            // runs the cvar registration code (sub_655FA0 etc.) — otherwise
            // we miss the early registrations. DllMain runs before WinMain,
            // so installing here catches everything.
            mtr::cvar_dump::install();
        }

        // Patch the camera aspect constant synchronously here, before the game's
        // CRT runs main() and triggers any camera setup that would cache the
        // (still 4/3) value.
        mtr::aspect::init();

        if (HANDLE t = CreateThread(nullptr, 0, mtr_init, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
        break;
    case DLL_PROCESS_DETACH:
        // lpvReserved != NULL  -> ExitProcess (process is terminating).
        // lpvReserved == NULL  -> FreeLibrary (DLL explicitly unloaded).
        // On process termination, Windows tears everything down; doing further
        // work under loader lock (MH_Uninitialize, ImGui_ImplDX9_Shutdown,
        // unsubclassing WndProc) risks deadlocks or leaking handles that keep
        // Wilbur in zombie state. Skip cleanup entirely on process exit.
        if (lpvReserved == nullptr) {
            mtr::menu::shutdown();
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            mtr::log::shutdown();
        } else {
            mtr::log::info("DllMain detach (process exit) -- skipping cleanup, OS will reclaim");
            mtr::log::shutdown();
        }
        break;
    }
    return TRUE;
}
