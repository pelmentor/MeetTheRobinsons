#include <windows.h>
#include <MinHook.h>
#include "mtr/version.h"
#include "mtr/coop/remote_player_manager.h"
#include "mtr/coop/net/net_session.h"
#include "mtr/coop/net/remote_pose_packet.h"

namespace mtr::log      { void init(); void info(const char* fmt, ...); void shutdown(); }
namespace mtr::cmdline  { void install(); }
namespace mtr::d3d9hook { void install(); void install_early(); }
namespace mtr::aspect   { void init(); }
namespace mtr::menu     { void shutdown(); }
namespace mtr::console     { bool install(); }
namespace mtr::screen_push { bool install(); }
namespace mtr::input_hook  { void install(); }
namespace mtr::dinput_hook { void install(); }
namespace mtr::cvar_dump   { bool install(); }
namespace mtr::vis_test_probe { bool install(); }
namespace mtr::scene_vis_log  { bool install(); }
namespace mtr::peripheral_cull_probe { bool install(); }
namespace mtr::ui_aspect_rules { void install_defaults(); }
namespace mtr::sprite_probe   { bool install(); }
namespace mtr::widget_probe   { bool install(); void arm(int); }
namespace mtr::coop_spawn_probe { bool install(); }
namespace mtr::coop::controlmapper_dev { bool install(); }
namespace mtr::coop::per_entity_tick_hook { bool install(); }
namespace mtr::protagonist_registry { bool install(); }
namespace mtr::coop_vibrate_route      { bool install(); }
namespace mtr::coop::dual_launch       { void install(); }
namespace mtr::sim_decouple   { void install(); }
namespace mtr::interp         { void install(); }
namespace mtr::freecam        { void install_transform_skip_hook(); }
namespace mtr::dt_correctness { void install(); }
namespace mtr::windowmode     { void install(); }
namespace mtr::test_harness   { void install(); }
namespace mtr::crash_handler  { void install(); }

static HMODULE g_self = nullptr;

// Exposed so other TUs (config-file loaders) can resolve paths relative to
// the ASI's location (Game/) rather than the EXE or current working dir.
namespace mtr { HMODULE self_module() { return g_self; } }

static DWORD WINAPI mtr_init(LPVOID) {
    mtr::log::info("mtr-asi v%s init thread (pid=%lu)", mtr::kModVersion, GetCurrentProcessId());
    mtr::log::info("display path: -dxresolution rewritten to native; D3D9 hooks for menu only");
    // Install windowmode BEFORE d3d9hook so the ChangeDisplaySettings hooks
    // are armed before any code path that might call them (some engines
    // call CDS as part of d3d9.dll load). The actual D3D9 vtable hooks
    // wait for d3d9.dll to load via a deferred thread, so this ordering
    // does not race the device creation path either.
    mtr::windowmode::install();
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
    // Peripheral-cull probe: instruments cull_aabb_corners_vs_global_frustum
    // (0x4E0370) + cull_sphere_vs_global_frustum (0x4DFF20) + the dispatch
    // entry (0x4E0AD0). Reads the engine's two cull counters at 0x724EC4 /
    // 0x724EC8 and the live 7-plane frustum at 0x726498+128. See
    // research/findings/peripheral-cull-pipeline-2026-05-09.md.
    mtr::peripheral_cull_probe::install();
    // Sprite-batcher list probe (Phase 3 M3.2). Now uses CALL-SITE detour
    // (5-byte rewrite at 0x4D23BF) instead of MinHook prologue trampoline,
    // which previously caused a black-screen freeze. Off by default —
    // the wrapper just forwards to render_sprite_batcher when not armed.
    mtr::sprite_probe::install();
    // UI widget Phase 0.5: hook engine SubmitSprite (sub_4E9350) and scan
    // caller stack frames for ASCII strings on candidate widget objects.
    // Goal: identify the m_pcName offset so the UI identity refactor MVP
    // can key by (screen_name, widget_name) instead of heap-pointer
    // state_keys. Install only — arming is deferred to a controlled moment
    // (test_harness arms it after main menu reach to avoid spamming on
    // process startup paths). Output: Game/mtr-asi-widget-probe.log.
    mtr::widget_probe::install();
    // Phase 0A coop derisk: arms a PRE-logger on sub_55AF00's runtime-resolved
    // real function (read from IAT slot 0x00F8DED0 at install time, NOT the
    // 6-byte stolen-byte thunk at 0x0055AF00 which crashed the process at
    // boot when MinHook'd directly — see coop_spawn_probe.cpp::install for
    // the rationale). Idle until try_spawn_p2 fires.
    mtr::coop_spawn_probe::install();
    // Phase 1.6 (2026-05-12): ControlMapper dev-pointer module. PRE hooks
    // engine Tick (ControlMapper vtable[3] @ static VA 0x006A639C+12) to
    // capture the instance address and the engine's hardware-fed `dev`
    // pointer at `*(instance+4)` on first invocation. Subsequent ticks use
    // controlmapper_dev::swap_to_player(idx) inside per_entity_tick_hook to
    // route P1/P2 input (MTA SwitchContext analogue). See
    // include/mtr/coop/input/controlmapper_dev.h.
    mtr::coop::controlmapper_dev::install();
    // Phase 2 step (b1+b2): protagonist instance registry. Hooks
    // protagonist_derived_ctor POST-return + protagonist dtor PRE to maintain
    // a small (instance ptr -> player_idx) side table. First registered = P1,
    // second = P2.
    mtr::protagonist_registry::install();
    // Phase 1.6 step 4 (2026-05-12): per-wilbur-entity tick PRE/POST hook on
    // sub_5AD9B0 @ 0x005AD9B0 (wilbur+0xD04 subscriber-list walker). Step 4
    // is logging-only: identifies P1 vs P2 per-tick invocations via
    // protagonist_registry. Step 5+ adds the controlmapper_dev::swap_to_player
    // call (MTA SwitchContext analogue). Installed AFTER protagonist_registry
    // so the player_idx_for() lookup is wired before the first walker fire.
    mtr::coop::per_entity_tick_hook::install();
    // C1 targeted fix (2026-05-12, strategy b): PRE hook on
    // VibrateJoystick::vt[13] @ 0x00532B40 short-circuits when called on the
    // orphan. VibrateJoystick is local-hardware-feedback (gamepad rumble);
    // a remote player has no local controller. MTA precedent:
    // reference/mtasa-blue/.../multiplayer_keysync.cpp. See
    // research/findings/coop-c1-investigation-2026-05-12.md.
    //
    // This route + coop_registry_mirror replace the retired
    // coop_orphan_filter (broad-suppression unlink mechanism). Filter
    // retired 2026-05-12 after 3600-frame orphan-soak validation with and
    // without filter showed route alone is sufficient.
    mtr::coop_vibrate_route::install();
    // Phase 1 (2026-05-12): MtrPlayerManager — the parallel-class registry
    // per MTA's CClientPlayerManager. Owns MtrRemotePlayer wrappers. No
    // networking yet; the manager wraps engine_wilbur as the local player
    // (lazy on first .local() call) and wraps any orphan that
    // coop_spawn_probe materialises (eagerly, right after registry mirror).
    // See research/findings/coop-mtr-remote-player-design-2026-05-12.md.
    mtr::coop::MtrPlayerManager::instance().install();
    // Phase 1.4b (2026-05-12 night): UDP transport. NetSession owns the
    // socket and a recv thread; it knows nothing about packet types
    // (Principle 7). The recv callback below is the neutral bridge: it
    // parses headers + bodies via the Phase 1.4a wire format and hands
    // valid pose snapshots to MtrPlayerManager::on_remote_packet, which
    // applies the modular time_ctx out-of-order gate and pushes into
    // the matching MtrRemotePlayer's interp scaffold. Callback MUST be
    // set BEFORE NetSession::install() — the recv thread reads it once
    // at startup.
    mtr::coop::net::NetSession::instance().set_recv_callback(
        [](const uint8_t* bytes, std::size_t len) -> bool {
            using namespace mtr::coop::net;
            PacketHeader hdr{};
            if (!parse_header(bytes, len, hdr)) return false;
            if (static_cast<PacketType>(hdr.type) != PacketType::PoseSnapshot) {
                // Unknown packet type for this protocol version. Drop;
                // future packet types append values, never reuse.
                return false;
            }
            PoseSnapshotBody body{};
            if (!parse_pose_snapshot(bytes, len, body)) return false;
            mtr::coop::MtrPlayerManager::instance().on_remote_packet(body);
            return true;
        });
    mtr::coop::net::NetSession::instance().install();
    // Seed default per-screen UI aspect rules so the "Auto from rules" path in
    // sprite_matrix has sensible baseline coverage out-of-the-box (no manual
    // table editing required for the common menu/loading/mini-game screens).
    mtr::ui_aspect_rules::install_defaults();
    // Sim/render decouple module (M0+M1.1): registers state + UI; the
    // simulation_tick_aggregator + pathcam hooks wire up in M1.2/M1.3.
    mtr::sim_decouple::install();
    // dt-correctness: writes flt_6FFCBC = real-dt at the start of each
    // render frame and at each sim tick, so all 150+ subsystems that
    // integrate against the hardcoded 0.003 dt become framerate-
    // independent. See research/findings/dt-correctness-root-cause-2026-05-07.md.
    mtr::dt_correctness::install();
    // Interp snapshot infra (M2). Hooks camera_apply_all_active POST to
    // capture prev/curr view+world matrices on each fresh sim tick.
    // Telemetry-only until M3 view-interp client lands.
    mtr::interp::install();
    // FreeCam MMB-tp player-skip on entity_transform_tick. Sets the
    // engine's own skip-bit (0x10 at node+68) on the player's transform
    // node during teleport-hold so the anim-driven snap-back to the
    // navmesh anchor doesn't fight our hold-write.
    mtr::freecam::install_transform_skip_hook();
    // Test harness: parses -mtrasi-test=<scenario> on cmdline; idle when
    // absent. Install LAST so every other module is armed when scenarios
    // tick. Outer watchdog: tools/run-test.ps1.
    mtr::test_harness::install();
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
        // Install crash handler FIRST after log init — so any subsequent
        // init failure (including ours) gets a stack trace + dump.
        mtr::crash_handler::install();
        if (MH_Initialize() != MH_OK) {
            mtr::log::info("MH_Initialize failed in DllMain");
        } else {
            mtr::cmdline::install();
            // Dual-launch bypass: hooks kernel32!CreateMutex{A,W} to rename
            // the Disney singleton mutex with a per-PID suffix so two
            // Wilbur.exe instances can co-exist on the same machine for
            // coop live-test. Installed only when a coop session flag
            // (-mtrasi-coop-host / -mtrasi-coop-connect) is present;
            // normal launches keep singleton enforcement. Must precede
            // any module that could ever call CreateMutex on the Disney
            // name (i.e. before Wilbur's WinMain).
            mtr::coop::dual_launch::install();
            // Cvar registration hooks must be armed before the game's WinMain
            // runs the cvar registration code (sub_655FA0 etc.) — otherwise
            // we miss the early registrations. DllMain runs before WinMain,
            // so installing here catches everything.
            mtr::cvar_dump::install();
            // Early Direct3DCreate9 hook: catches the engine's first call
            // to Direct3DCreate9 in WinMain, hooks CreateDevice on the
            // returned IDirect3D9 vtable BEFORE the engine creates its
            // real device. Required for cold-launch MSAA / windowmode
            // overrides to apply (the deferred-thread vtable hook misses
            // CreateDevice because it runs after the engine has already
            // called it).
            mtr::d3d9hook::install_early();
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
