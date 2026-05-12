// In-process test harness for autonomous "build → launch → test → exit".
//
// Activation: pass -mtrasi-test=<scenario> on the cmdline (PowerShell
// watchdog `tools/run-test.ps1` does this; manual: launch Wilbur.exe
// directly with the flag).
//
// Once active, the scenario runs from this module's per-frame tick (called
// out of menu.cpp::on_end_scene). On terminal it writes
// `Game/mtr-asi-test-result.json` and posts WM_CLOSE to Wilbur's main
// window for a clean shutdown that lets DllMain detach properly (no file
// lock leaking onto mtr-asi.asi for the next iteration).
//
// Adding a scenario = one tick function returning Pending/Pass/Fail and
// one entry in g_scenarios.
//
// Architecture writeup: research/findings/autonomous-test-loop-design.md.

#include "mtr/test_harness.h"
#include "mtr/coop/net/net_session.h"
#include "mtr/coop/remote_player_manager.h"
#include "mtr/coop/input/controlmapper_dev.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace mtr::log         { void info(const char* fmt, ...); }
namespace mtr               { HMODULE self_module(); }
namespace mtr::screen_push  {
    bool ready();
    bool current_top_name(char* out, size_t out_size);
    int  stack_depth();
}
namespace mtr::screenshot   { void request(); }
namespace mtr::dinput_hook  { void inject_kb_keypress(uint8_t dik_scancode, int poll_count); }
namespace mtr::widget_probe {
    void arm(int budget); void disarm();
    int  findings_count();
    unsigned int frame_table_size();
    void debug_dump_frame_table();
    void debug_dump_widget_map();
}
namespace mtr::trigger_overlay {
    void set_enabled(bool v);
    void set_show_test_box(bool v);
    void set_export_frames(int n);
    int  export_frames_remaining();
    int  visible_box_count();
}
namespace mtr::npc_overlay {
    void set_enabled(bool v);
    void set_show_name(bool v);
    void set_show_pos(bool v);
    void set_show_distance(bool v);
    void set_export_frames(int n);
    int  export_frames_remaining();
    int  visible_npc_count();
}
namespace mtr::prop_overlay {
    void set_enabled(bool v);
    void set_show_disassembleable(bool v);
    void set_export_frames(int n);
    int  export_frames_remaining();
    int  visible_prop_count();
}

namespace mtr::coop_spawn_probe { bool try_spawn_p2(); }

namespace mtr::test_harness {

namespace {

// === Gameplay detector ====================================================
//
// Empirical finding (2026-05-10): the engine renders gameplay WITHOUT
// pushing a new screen onto the screen stack. After CONTINUE GAME on
// WilburMainMenu is activated, sim ticks resume and the world renders,
// but `current_top_name` continues to read "WilburMainMenu" indefinitely.
// Original v0.2 plan correctly anticipated this — Phase D uses
// transform-list count + entity_lookup("player", 1) as the gate.
//
// Pattern adopted from interp_player.cpp + npc_overlay.cpp:
//   - dword_724DE4 is the head of the active transform list. Walk it via
//     +0x04 (next), filter out entries with kNodeFlagsSkipBit at +0x44,
//     count entities at +0x5C. Empty/menu state has 0-10 entries; live
//     gameplay has 50+.
//   - entity_lookup_by_name_retry @ 0x5AC8F0 is __thiscall — needs the
//     entity-manager pointer (read from 0x7425AC) loaded into ECX. Calling
//     without this walks arbitrary memory off whatever-was-in-ECX.
//   - Both signals together => gameplay is active.

constexpr uintptr_t kEntityLookupByNameRetryVA = 0x005AC8F0;
constexpr uintptr_t kEntityManagerPtrVA        = 0x007425AC;
constexpr uintptr_t kTransformListHeadVA       = 0x00724DE4;
constexpr uintptr_t kNodeNextOffset            = 0x04;
constexpr uintptr_t kNodeFlagsOffset           = 0x44;
constexpr uintptr_t kNodeEntityOffset          = 0x5C;
constexpr uint8_t   kNodeFlagsSkipBit          = 0x10;
constexpr int       kMaxTransformIterations    = 8192;

using PFN_EntityLookupRetry = void* (__thiscall*)(void* self, const char* name, int unused);

int count_transform_list_unsafe() {
    auto* node = *reinterpret_cast<uint8_t**>(kTransformListHeadVA);
    int count = 0;
    int safety = kMaxTransformIterations;
    while (node && safety-- > 0) {
        const uint8_t flags = *(node + kNodeFlagsOffset);
        uint8_t* next = *reinterpret_cast<uint8_t**>(node + kNodeNextOffset);
        if ((flags & kNodeFlagsSkipBit) == 0) {
            void* entity = *reinterpret_cast<void**>(node + kNodeEntityOffset);
            if (entity) ++count;
        }
        node = next;
    }
    return count;
}

int count_transform_list() {
    int n = -1;
    __try { n = count_transform_list_unsafe(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = -1; }
    return n;
}

void* resolve_player_entity() {
    void* mgr = *reinterpret_cast<void* volatile*>(kEntityManagerPtrVA);
    if (!mgr) return nullptr;
    void* p = nullptr;
    __try {
        auto fn = reinterpret_cast<PFN_EntityLookupRetry>(kEntityLookupByNameRetryVA);
        p = fn(mgr, "player", 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p;
}

// Returns true when entity_lookup("player",1) returns non-null. Empirical
// finding (2026-05-10 run): the player entity is allocated by the engine
// the moment CONTINUE GAME activates and gameplay starts — before that,
// at WilburMainMenu and earlier menus, the lookup returns null. So the
// player-entity-exists signal alone is a clean gate. Earlier attempt
// gated on transform_count>=30 was too strict; sparse levels (Robinson
// House intro) had tcount=1 and we mis-classified them as still-in-menu,
// kept firing DIK_RETURN, and one retry eventually activated the wrong
// menu item and pushed us back to the slot picker.
bool is_in_gameplay(int* out_count = nullptr, void** out_player = nullptr) {
    const int count = count_transform_list();
    void* const player = resolve_player_entity();
    if (out_count)  *out_count  = count;
    if (out_player) *out_player = player;
    return player != nullptr;
}

// === Hard-kill shutdown ===================================================
//
// WM_CLOSE during gameplay MAY trigger an engine autosave that overwrites
// the user's save. Pre-3 of the v0.2 plan was supposed to verify whether
// WM_CLOSE is safe; we never ran that check. Until verified, use
// TerminateProcess for harness-driven shutdowns from gameplay state. The
// engine never gets a chance to autosave. File handle on mtr-asi.asi
// gets released by the OS on process termination, so the next iteration
// can replace it.
//
// For non-gameplay scenarios (boot-to-main-menu, menu-nav-smoke etc) the
// existing WM_CLOSE path is fine since no gameplay state exists to save.
void hard_kill_self() {
    mtr::log::info("test_harness: TerminateProcess(self) — bypassing engine"
                   " WM_CLOSE handler to avoid autosave-on-shutdown risk");
    TerminateProcess(GetCurrentProcess(), 0);
    // unreachable
}

enum class Result { Pending, Pass, Fail, Timeout };

constexpr size_t kNameLen   = 64;
constexpr size_t kDetailLen = 256;
constexpr int    kDefaultTimeoutSec = 60;

struct ScenarioContext {
    char     name[kNameLen]     = {0};
    char     detail[kDetailLen] = {0};
    ULONGLONG start_tick_ms     = 0;
    DWORD    timeout_ms         = kDefaultTimeoutSec * 1000;
    uint64_t frame_count        = 0;
};

ScenarioContext g_ctx{};
std::atomic<bool> g_active{false};

using ScenarioFn = Result (*)(ScenarioContext&);

// === Scenario implementations ============================================

// Find Wilbur's main top-level window (owned by current process, visible,
// no owner). Cached after first successful lookup.
HWND find_main_window() {
    static HWND s_cached = nullptr;
    if (s_cached && IsWindow(s_cached)) return s_cached;

    struct EnumCtx { DWORD pid; HWND hwnd; };
    EnumCtx ctx{ GetCurrentProcessId(), nullptr };
    auto cb = [](HWND h, LPARAM l) -> BOOL {
        auto* ec = reinterpret_cast<EnumCtx*>(l);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid != ec->pid)              return TRUE;
        if (!IsWindowVisible(h))         return TRUE;
        if (GetWindow(h, GW_OWNER))      return TRUE;
        ec->hwnd = h;
        return FALSE;
    };
    EnumWindows(reinterpret_cast<WNDENUMPROC>(+cb), reinterpret_cast<LPARAM>(&ctx));
    s_cached = ctx.hwnd;
    return ctx.hwnd;
}

// Send a keystroke directly to our own process. Goes through the OS input
// pipeline (keybd_event) which the game's title-screen WndProc consumes
// and which DirectInput's polling also reads. Reliable than SendInput
// with focus rules because we're calling from in-process: no
// AllowSetForegroundWindow gate. The vk_scan pair is the standard for
// VK_RETURN (0x0D / 0x1C).
void synthetic_keypress(BYTE vk, BYTE scan) {
    keybd_event(vk, scan, 0, 0);
    Sleep(20);
    keybd_event(vk, scan, KEYEVENTF_KEYUP, 0);
}

// Smoke test for the entire harness pipeline:
//   - ASI loaded? (we wouldn't be ticking otherwise)
//   - cmdline_hook armed? (we wouldn't see the flag otherwise)
//   - screen_push tracking? (`ready()` returns true once captured)
//   - engine reached the main menu? (top-of-stack name match)
//
// Title screen dismissal: the game shows "MEET THE ROBINSONS — PRESS ANY
// KEY" before the main menu loads. Send synthetic keypresses (VK_RETURN
// every ~1 sec) until screen_push starts capturing — that's our signal
// that we're past the splash.
Result tick_boot_to_main_menu(ScenarioContext& ctx) {
    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) {
        got_top = mtr::screen_push::current_top_name(top, sizeof(top));
    }

    // Diagnostic: log state every ~240 frames (≈ 1 sec at 240 fps) so a
    // failed scenario shows what state the game DID reach. Cheap.
    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[boot-to-main-menu] frame=%llu ready=%d top=\"%s\"",
            static_cast<unsigned long long>(ctx.frame_count),
            ready ? 1 : 0,
            got_top ? top : "(none)");
    }

    // Periodic screenshot capture (~every 5s) so we can SEE what the game
    // is showing during a stuck scenario instead of guessing from logs.
    // Saved to Game/screenshots/mtr_*.bmp; harness archives them to the
    // run dir at exit. F12 also triggers manual capture.
    if (ctx.frame_count == 240 || (ctx.frame_count % 1200) == 0) {
        mtr::screenshot::request();
    }

    if (!ready || !got_top) {
        // Title-screen dismissal: arm a DI keyboard injection for the
        // RETURN scancode every ~60 frames (≈ ¼s at 240 fps). The mod's
        // dinput_hook OR's the bit into the buffer returned by
        // GetDeviceState, which the engine polls in DI-exclusive mode at
        // the title screen. Reaches the engine even when SendInput /
        // keybd_event don't (DI-exclusive bypasses synthetic OS input).
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            // DIK_RETURN = 0x1C, DIK_SPACE = 0x39 — try both since title
            // says "PRESS ANY KEY" and we don't know which the engine
            // actually checks. 5 polls of injection ≈ enough for any
            // input system to observe.
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }
    // Accept any screen name that represents the main entry menu:
    //   GameSelectScreen — the actual visible "MAIN MENU - SELECT AN
    //                       OPTION" with NEW GAME / LOAD / OPTIONS / QUIT.
    //                       This is what the user lands on after the
    //                       title-screen dismissal — verified via in-test
    //                       screenshots 2026-05-09.
    //   WilburMainMenu / ScreenWilburMainMenu — sub-screen that may appear
    //                       further into the menu chain (per .sc + .h
    //                       sources in Game/data/screens/). Accept too
    //                       so the scenario passes if the game routes
    //                       differently in some setup.
    if (std::strcmp(top, "GameSelectScreen")     == 0 ||
        std::strcmp(top, "WilburMainMenu")       == 0 ||
        std::strcmp(top, "ScreenWilburMainMenu") == 0) {
        std::snprintf(ctx.detail, sizeof(ctx.detail),
                      "Reached main menu after %llu frames; top=%s",
                      static_cast<unsigned long long>(ctx.frame_count), top);
        return Result::Pass;
    }
    return Result::Pending;
}

// === verify-main-menu-visible scenario ===================================
//
// Drives boot-to-main-menu, then HOLDS at the main menu while taking a
// screenshot every ~1s for 8 seconds. Lets a human (or me, by reading
// the BMPs) verify the splash actually fades and the main menu becomes
// visible — addresses the false-positive in boot-to-main-menu where
// `screen_push` reports GameSelectScreen before the visual transition
// completes.
Result tick_verify_main_menu_visible(ScenarioContext& ctx) {
    static int  s_first_detect_frame = 0;
    static bool s_detected = false;
    constexpr int kHoldFrames = 1920;  // ~8s at 240Hz

    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) got_top = mtr::screen_push::current_top_name(top, sizeof(top));

    // Periodic state log.
    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[verify-main-menu] frame=%llu ready=%d top=\"%s\"",
            static_cast<unsigned long long>(ctx.frame_count),
            ready ? 1 : 0,
            got_top ? top : "(none)");
    }

    // Title-screen dismissal (same as boot scenario).
    if (!ready || !got_top) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    bool is_main_menu = (std::strcmp(top, "GameSelectScreen")     == 0 ||
                        std::strcmp(top, "WilburMainMenu")       == 0 ||
                        std::strcmp(top, "ScreenWilburMainMenu") == 0);
    if (!is_main_menu) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    if (!s_detected) {
        s_first_detect_frame = static_cast<int>(ctx.frame_count);
        s_detected = true;
        mtr::log::info("verify-main-menu: first detect at frame=%d (top=%s);"
                       " will hold + screenshot for %d frames",
                       s_first_detect_frame, top, kHoldFrames);
    }

    int held = static_cast<int>(ctx.frame_count) - s_first_detect_frame;

    // Take a screenshot every ~240 frames (~1s).
    if ((held % 240) == 1) {
        mtr::screenshot::request();
    }

    if (held < kHoldFrames) return Result::Pending;

    std::snprintf(ctx.detail, sizeof(ctx.detail),
                  "Held at top=%s for %d frames after first detect",
                  top, held);
    return Result::Pass;
}

// === widget-probe scenario ===============================================
//
// Boots to main menu (same dismissal as boot-to-main-menu), arms
// widget_probe at the moment the main-menu screen is detected, holds for
// kProbeFrames frames (~0.5s at 240Hz) to let the probe accumulate
// findings, then disarms and passes.
//
// Drives Phase 0.5 of the UI element identity refactor: locating the
// widget m_pcName offset by inspecting the probe log offline.
Result tick_widget_probe(ScenarioContext& ctx) {
    static int  s_armed_at_frame = 0;
    static bool s_armed = false;
    constexpr int kProbeFrames = 120;   // ~0.5s at 240Hz
    constexpr int kProbeBudget = 800;

    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) got_top = mtr::screen_push::current_top_name(top, sizeof(top));

    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[widget-probe] frame=%llu ready=%d top=\"%s\""
            " armed=%d findings=%d",
            static_cast<unsigned long long>(ctx.frame_count),
            ready ? 1 : 0,
            got_top ? top : "(none)",
            s_armed ? 1 : 0,
            mtr::widget_probe::findings_count());
    }

    if (ctx.frame_count == 240 || (ctx.frame_count % 1200) == 0) {
        mtr::screenshot::request();
    }

    if (!ready || !got_top) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    bool is_main_menu = (std::strcmp(top, "GameSelectScreen")     == 0 ||
                        std::strcmp(top, "WilburMainMenu")       == 0 ||
                        std::strcmp(top, "ScreenWilburMainMenu") == 0);
    if (!is_main_menu) return Result::Pending;

    if (!s_armed) {
        mtr::log::info("widget-probe: main menu reached at frame=%llu (top=%s);"
                       " arming probe for %d frames",
                       static_cast<unsigned long long>(ctx.frame_count),
                       top, kProbeFrames);
        mtr::widget_probe::arm(kProbeBudget);
        s_armed = true;
        s_armed_at_frame = static_cast<int>(ctx.frame_count);
        return Result::Pending;
    }

    int held = static_cast<int>(ctx.frame_count) - s_armed_at_frame;
    if (held < kProbeFrames) return Result::Pending;

    mtr::widget_probe::disarm();
    mtr::widget_probe::debug_dump_widget_map();
    mtr::widget_probe::debug_dump_frame_table();
    int n = mtr::widget_probe::findings_count();
    std::snprintf(ctx.detail, sizeof(ctx.detail),
                  "Probed at top=%s for %d frames; %d findings",
                  top, held, n);
    return n > 0 ? Result::Pass : Result::Fail;
}

// === hold-at-menu scenario ===============================================
//
// Boots to main menu, then HOLDS forever (returns Pending). Never passes,
// never fails, never auto-kills the game. Lets the user drive testing
// manually: scale widgets, verify persistence, observe freezes, etc.
// Watchdog timeout (-mtrasi-test-timeout=N) still applies — set high
// (300+) when launching for manual sessions, OR launch via the user's
// normal launcher with no scenario flag for fully unmanaged play.
//
// Useful when:
// - A hook regression makes the menu transition slow but the user wants
//   to inspect the post-freeze state.
// - Verifying widget_name persistence requires editing a slot, quitting,
//   relaunching, observing — needs the game stay alive long enough to
//   navigate the menu.
Result tick_hold_at_menu(ScenarioContext& ctx) {
    static bool s_first_detect_logged = false;

    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) got_top = mtr::screen_push::current_top_name(top, sizeof(top));

    // Periodic state log (~1s at 240Hz).
    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[hold-at-menu] frame=%llu ready=%d top=\"%s\"",
            static_cast<unsigned long long>(ctx.frame_count),
            ready ? 1 : 0,
            got_top ? top : "(none)");
    }

    // Title-screen dismissal — same as boot scenario.
    if (!ready || !got_top) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    bool is_main_menu = (std::strcmp(top, "GameSelectScreen")     == 0 ||
                        std::strcmp(top, "WilburMainMenu")       == 0 ||
                        std::strcmp(top, "ScreenWilburMainMenu") == 0);
    if (!is_main_menu) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    if (!s_first_detect_logged) {
        mtr::log::info("hold-at-menu: main menu reached at frame=%llu (top=%s);"
                       " holding indefinitely — user-driven testing mode",
                       static_cast<unsigned long long>(ctx.frame_count), top);
        s_first_detect_logged = true;
    }
    // Always Pending — never resolves. User terminates Wilbur.exe manually
    // (close the window, ALT+F4) when done.
    return Result::Pending;
}

// === overlay-phase1-verify scenario ======================================
//
// Validates the trigger-box overlay's projection + clip pipeline (Phase 1+2)
// numerically, without needing a human's eyes. Boots to main menu via the
// shared dismissal path, then enables the overlay + test box, arms an
// export of the next kExportFrames frames. Each exported frame writes the
// engine's view + projection matrices and every drawn edge's screen-space
// endpoints to the log via TRIGGER_OVERLAY_* lines. After export drains,
// the offline tools/validate-overlay-frames.ps1 reads those lines and
// re-runs the projection math, asserting each logged edge matches the
// recomputed value within 0.5 pixel tolerance.
//
// Pass condition: the export drained AND the overlay reported >= 1 visible
// box on at least one of the exported frames. (A box might be off-screen
// for some frames if the camera looks away, but on the main menu the
// world origin is typically inside the menu's environment.)
//
// Failure modes this scenario surfaces:
//   - Projection matrix global at 0x745AA0 holds garbage on the main menu
//     (whole-box-outside test rejects every frame; visible boxes remain 0).
//   - View matrix read fails; tick() bails early; export count never drains.
//   - Whole-box-reject logic is wrong (visible boxes is N but offline
//     validator finds clip errors).
Result tick_overlay_phase1_verify(ScenarioContext& ctx) {
    static int  s_first_detect_frame = 0;
    static bool s_armed              = false;
    static int  s_export_started_at  = 0;
    static int  s_max_visible_seen   = 0;
    constexpr int kExportFrames = 60;
    constexpr int kSettleFrames = 60;  // wait this many frames after main-menu
                                        // detection before arming export so the
                                        // matrices are post-fade-in.

    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) got_top = mtr::screen_push::current_top_name(top, sizeof(top));

    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[overlay-phase1-verify] frame=%llu ready=%d top=\"%s\""
            " armed=%d export_remaining=%d max_visible=%d",
            static_cast<unsigned long long>(ctx.frame_count),
            ready ? 1 : 0,
            got_top ? top : "(none)",
            s_armed ? 1 : 0,
            mtr::trigger_overlay::export_frames_remaining(),
            s_max_visible_seen);
    }

    // Title-screen dismissal — same as the other scenarios.
    if (!ready || !got_top) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    bool is_main_menu = (std::strcmp(top, "GameSelectScreen")     == 0 ||
                        std::strcmp(top, "WilburMainMenu")       == 0 ||
                        std::strcmp(top, "ScreenWilburMainMenu") == 0);
    if (!is_main_menu) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    if (!s_armed) {
        if (s_first_detect_frame == 0) {
            s_first_detect_frame = static_cast<int>(ctx.frame_count);
            mtr::log::info("overlay-phase1-verify: main menu reached at"
                           " frame=%d; settling for %d frames before arming",
                           s_first_detect_frame, kSettleFrames);
        }
        const int settled = static_cast<int>(ctx.frame_count) - s_first_detect_frame;
        if (settled < kSettleFrames) return Result::Pending;

        // Settled. Enable overlay + test box + arm export.
        mtr::trigger_overlay::set_enabled(true);
        mtr::trigger_overlay::set_show_test_box(true);
        mtr::trigger_overlay::set_export_frames(kExportFrames);
        s_armed = true;
        s_export_started_at = static_cast<int>(ctx.frame_count);
        mtr::log::info("overlay-phase1-verify: armed export for %d frames"
                       " starting at frame=%d", kExportFrames, s_export_started_at);
        // Take a screenshot at arm time (pre-export) so the human reviewer
        // also gets a visual reference if they choose to look.
        mtr::screenshot::request();
        return Result::Pending;
    }

    // Track max visible boxes seen during export (any non-zero count means
    // the projection produced a renderable result for at least one frame).
    const int vis = mtr::trigger_overlay::visible_box_count();
    if (vis > s_max_visible_seen) s_max_visible_seen = vis;

    // Wait until export drains.
    if (mtr::trigger_overlay::export_frames_remaining() > 0) {
        return Result::Pending;
    }

    // Export complete. Take final screenshot for human reference.
    mtr::screenshot::request();

    if (s_max_visible_seen <= 0) {
        std::snprintf(ctx.detail, sizeof(ctx.detail),
                      "Export drained but the test box was never visible "
                      "(whole-box-outside rejected every frame). Likely "
                      "the projection at 0x745AA0 is not the main-camera "
                      "perspective on this screen, or the test box at "
                      "world (0,0,0) is outside the camera frustum.");
        return Result::Fail;
    }

    std::snprintf(ctx.detail, sizeof(ctx.detail),
                  "Exported %d frames; max visible boxes per frame = %d. "
                  "Validator script (tools/validate-overlay-frames.ps1) "
                  "re-checks the projection math offline.",
                  kExportFrames, s_max_visible_seen);
    return Result::Pass;
}

// === npc-overlay-phase1-verify scenario ==================================
//
// Autonomous validation of the NPC overlay's projection + walker pipeline.
// Boots to main menu, settles, enables npc_overlay, arms 60-frame export.
// Pass criteria are deliberately loose — main menu has no real gameplay
// NPCs, but the engine populates the transform list (dword_724DE4) with
// menu-related sub-objects. The walker should:
//   1) not crash (single SEH wraps the per-frame walk)
//   2) iterate at least once (total_iter > 0 on most frames)
//   3) NOT crash on any name validations regardless of how many succeed
//
// Visual validation of NPC labels in actual gameplay needs a save game
// load — out of scope for autonomous overnight per the v2 plan. The
// offline validator (tools/validate-npc-overlay-frames.ps1) re-runs the
// projection math on logged matrices for each NPC that DID render and
// asserts the screen coordinates match within 0.5 px.
Result tick_npc_overlay_phase1_verify(ScenarioContext& ctx) {
    static int  s_first_detect_frame = 0;
    static bool s_armed              = false;
    static int  s_export_started_at  = 0;
    static int  s_max_visible_seen   = 0;
    constexpr int kExportFrames = 60;
    constexpr int kSettleFrames = 60;

    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) got_top = mtr::screen_push::current_top_name(top, sizeof(top));

    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[npc-overlay-phase1-verify] frame=%llu ready=%d top=\"%s\""
            " armed=%d export_remaining=%d max_visible=%d",
            static_cast<unsigned long long>(ctx.frame_count),
            ready ? 1 : 0,
            got_top ? top : "(none)",
            s_armed ? 1 : 0,
            mtr::npc_overlay::export_frames_remaining(),
            s_max_visible_seen);
    }

    if (!ready || !got_top) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    bool is_main_menu = (std::strcmp(top, "GameSelectScreen")     == 0 ||
                        std::strcmp(top, "WilburMainMenu")       == 0 ||
                        std::strcmp(top, "ScreenWilburMainMenu") == 0);
    if (!is_main_menu) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    if (!s_armed) {
        if (s_first_detect_frame == 0) {
            s_first_detect_frame = static_cast<int>(ctx.frame_count);
            mtr::log::info("npc-overlay-phase1-verify: main menu reached at"
                           " frame=%d; settling for %d frames before arming",
                           s_first_detect_frame, kSettleFrames);
        }
        const int settled = static_cast<int>(ctx.frame_count) - s_first_detect_frame;
        if (settled < kSettleFrames) return Result::Pending;

        mtr::npc_overlay::set_enabled(true);
        mtr::npc_overlay::set_show_name(true);
        mtr::npc_overlay::set_show_pos(true);
        mtr::npc_overlay::set_show_distance(true);
        mtr::npc_overlay::set_export_frames(kExportFrames);
        s_armed = true;
        s_export_started_at = static_cast<int>(ctx.frame_count);
        mtr::log::info("npc-overlay-phase1-verify: armed export for %d frames"
                       " starting at frame=%d", kExportFrames, s_export_started_at);
        mtr::screenshot::request();
        return Result::Pending;
    }

    const int vis = mtr::npc_overlay::visible_npc_count();
    if (vis > s_max_visible_seen) s_max_visible_seen = vis;

    if (mtr::npc_overlay::export_frames_remaining() > 0) {
        return Result::Pending;
    }

    mtr::screenshot::request();

    // PASS criteria: walker survived (no crash — we got here, that's
    // already proof). Whether visible_npc_count was > 0 is a secondary
    // signal — main menu may have 0 visible labels, that's expected.
    std::snprintf(ctx.detail, sizeof(ctx.detail),
                  "Exported %d frames; max visible NPCs per frame = %d. "
                  "Walker did not crash. Validator re-checks projection "
                  "math offline for any NPCs that did render.",
                  kExportFrames, s_max_visible_seen);
    return Result::Pass;
}

// === prop-overlay-phase1-verify scenario =================================
//
// Validates the prop overlay's walker safety + entity_kv accessor at the
// main menu. Main menu typically has 0 entities in dword_724DE4 so this
// scenario PRIMARILY tests:
//   1. Walker doesn't crash (single SEH per entity body)
//   2. entity_kv::get with a sentinel key on a non-existent entity is
//      cleanly handled (call short-circuits before deref)
//   3. Export pipeline survives empty-walk frames cleanly
//
// Real visual validation (props labeled correctly in gameplay) requires
// a save game load and is human-driven.
Result tick_prop_overlay_phase1_verify(ScenarioContext& ctx) {
    static int  s_first_detect_frame = 0;
    static bool s_armed              = false;
    static int  s_max_visible_seen   = 0;
    constexpr int kExportFrames = 60;
    constexpr int kSettleFrames = 60;

    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) got_top = mtr::screen_push::current_top_name(top, sizeof(top));

    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[prop-overlay-phase1-verify] frame=%llu ready=%d top=\"%s\""
            " armed=%d export_remaining=%d max_visible=%d",
            static_cast<unsigned long long>(ctx.frame_count),
            ready ? 1 : 0,
            got_top ? top : "(none)",
            s_armed ? 1 : 0,
            mtr::prop_overlay::export_frames_remaining(),
            s_max_visible_seen);
    }

    if (!ready || !got_top) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    bool is_main_menu = (std::strcmp(top, "GameSelectScreen")     == 0 ||
                        std::strcmp(top, "WilburMainMenu")       == 0 ||
                        std::strcmp(top, "ScreenWilburMainMenu") == 0);
    if (!is_main_menu) {
        if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
            mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
        }
        return Result::Pending;
    }

    if (!s_armed) {
        if (s_first_detect_frame == 0) {
            s_first_detect_frame = static_cast<int>(ctx.frame_count);
            mtr::log::info("prop-overlay-phase1-verify: main menu reached at"
                           " frame=%d; settling for %d frames before arming",
                           s_first_detect_frame, kSettleFrames);
        }
        const int settled = static_cast<int>(ctx.frame_count) - s_first_detect_frame;
        if (settled < kSettleFrames) return Result::Pending;

        mtr::prop_overlay::set_enabled(true);
        mtr::prop_overlay::set_show_disassembleable(true);
        mtr::prop_overlay::set_export_frames(kExportFrames);
        s_armed = true;
        mtr::log::info("prop-overlay-phase1-verify: armed export for %d frames",
                       kExportFrames);
        mtr::screenshot::request();
        return Result::Pending;
    }

    const int vis = mtr::prop_overlay::visible_prop_count();
    if (vis > s_max_visible_seen) s_max_visible_seen = vis;

    if (mtr::prop_overlay::export_frames_remaining() > 0) {
        return Result::Pending;
    }

    mtr::screenshot::request();

    std::snprintf(ctx.detail, sizeof(ctx.detail),
                  "Exported %d frames; max visible props per frame = %d. "
                  "Walker + entity_kv survived. Real validation needs a "
                  "save-game load (props don't exist on main menu).",
                  kExportFrames, s_max_visible_seen);
    return Result::Pass;
}

// === menu-nav-smoke scenario =============================================
//
// Pre-1 gate for the autonomous save-load probe loop. Verifies that DIK
// injection through dinput_hook actually drives Wilbur's MENU input layer
// (widget/ControlMapper polling), not just the title-splash "press any
// key" handler. If this fails, the entire DIK-based harness pivots to a
// widget-callback dispatcher hook.
//
// Plan v0.2 said "inject DIK_DOWN, watch for screen change", but DIK_DOWN
// on a menu only highlights a sibling button — no screen push/pop, so no
// observable side-channel from inside the mod. Real smoke test:
//   1. Inject DIK_DOWN ×10 polls (proves arrow-nav routes to engine; no
//                                 observable effect on screen state).
//   2. Hold 30 frames.
//   3. Inject DIK_RETURN ×10 polls (proves activate routes; SHOULD cause
//                                   a screen push if either key reached
//                                   the engine and a menu item is now
//                                   highlighted).
//   4. Hold 90 frames for the screen transition.
// Pass if (current_top_name OR stack_depth) ever differs from baseline
// during steps 1-4. Either path proves DIK reaches the menu input layer.
// Both silent ⇒ DIK is splash-only ⇒ pivot to widget-callback dispatcher.
//
// Capture screenshots at: post-boot baseline, after DIK_DOWN, after
// DIK_RETURN, at evaluate. Screenshot also fires the moment a change is
// first observed, naming the new state visually.
Result tick_menu_nav_smoke(ScenarioContext& ctx) {
    enum Phase {
        kBoot = 0,        // dismiss title splash; reuse existing pattern
        kSettle,          // 60 frames of stillness on main menu
        kInjectDown,      // fire DIK_DOWN once
        kHoldDown,        // 30 frames
        kInjectReturn,    // fire DIK_RETURN once
        kHoldReturn,      // 90 frames
        kEvaluate,
    };
    static int  s_phase                  = kBoot;
    static int  s_phase_start_frame      = 0;
    static char s_baseline_top[kNameLen] = {0};
    static int  s_baseline_depth         = 0;
    static bool s_observed_change        = false;
    static int  s_observed_frame         = 0;
    static char s_observed_top[kNameLen] = {0};
    static int  s_observed_depth         = 0;

    constexpr int kSettleFrames     = 60;
    constexpr int kHoldDownFrames   = 30;
    constexpr int kHoldReturnFrames = 90;

    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) got_top = mtr::screen_push::current_top_name(top, sizeof(top));

    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[menu-nav-smoke] frame=%llu phase=%d ready=%d top=\"%s\""
            " observed_change=%d",
            static_cast<unsigned long long>(ctx.frame_count),
            s_phase, ready ? 1 : 0,
            got_top ? top : "(none)",
            s_observed_change ? 1 : 0);
    }

    // Phase kBoot: reuse boot-to-main-menu dismissal pattern.
    if (s_phase == kBoot) {
        if (!ready || !got_top) {
            if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
                mtr::dinput_hook::inject_kb_keypress(0x1C, 5);  // DIK_RETURN
            }
            return Result::Pending;
        }
        bool is_main_menu = (std::strcmp(top, "GameSelectScreen")     == 0 ||
                            std::strcmp(top, "WilburMainMenu")       == 0 ||
                            std::strcmp(top, "ScreenWilburMainMenu") == 0);
        if (!is_main_menu) {
            if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
                mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
            }
            return Result::Pending;
        }
        // Reached menu. Capture baseline. Move to settle phase.
        std::strncpy(s_baseline_top, top, sizeof(s_baseline_top) - 1);
        s_baseline_top[sizeof(s_baseline_top) - 1] = 0;
        s_baseline_depth = mtr::screen_push::stack_depth();
        s_phase = kSettle;
        s_phase_start_frame = static_cast<int>(ctx.frame_count);
        mtr::log::info("menu-nav-smoke: main menu reached at frame=%d top=\"%s\""
                       " depth=%d; settling for %d frames",
                       s_phase_start_frame, s_baseline_top, s_baseline_depth,
                       kSettleFrames);
        mtr::screenshot::request();
        return Result::Pending;
    }

    // From here on, observe top + depth every frame. Capture first divergence.
    int depth = mtr::screen_push::stack_depth();
    bool changed = (got_top && std::strcmp(top, s_baseline_top) != 0)
                || (depth != s_baseline_depth);
    if (changed && !s_observed_change) {
        s_observed_change = true;
        s_observed_frame  = static_cast<int>(ctx.frame_count);
        std::strncpy(s_observed_top,
                     got_top ? top : "(none)",
                     sizeof(s_observed_top) - 1);
        s_observed_top[sizeof(s_observed_top) - 1] = 0;
        s_observed_depth = depth;
        mtr::log::info("menu-nav-smoke: observed change at frame=%d"
                       " top=\"%s\" depth=%d (baseline top=\"%s\" depth=%d)",
                       s_observed_frame, s_observed_top, s_observed_depth,
                       s_baseline_top, s_baseline_depth);
        mtr::screenshot::request();
    }

    int held = static_cast<int>(ctx.frame_count) - s_phase_start_frame;

    switch (s_phase) {
    case kSettle: {
        if (held >= kSettleFrames) {
            s_phase = kInjectDown;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
            mtr::log::info("menu-nav-smoke: injecting DIK_DOWN at frame=%d",
                           s_phase_start_frame);
            mtr::dinput_hook::inject_kb_keypress(0xD0, 10);  // DIK_DOWN
            mtr::screenshot::request();
        }
        return Result::Pending;
    }
    case kInjectDown:
        // Inject was fired in kSettle's advance — now move to hold.
        s_phase = kHoldDown;
        s_phase_start_frame = static_cast<int>(ctx.frame_count);
        return Result::Pending;
    case kHoldDown: {
        if (held >= kHoldDownFrames) {
            s_phase = kInjectReturn;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
            mtr::log::info("menu-nav-smoke: injecting DIK_RETURN at frame=%d",
                           s_phase_start_frame);
            mtr::dinput_hook::inject_kb_keypress(0x1C, 10);  // DIK_RETURN
            mtr::screenshot::request();
        }
        return Result::Pending;
    }
    case kInjectReturn:
        s_phase = kHoldReturn;
        s_phase_start_frame = static_cast<int>(ctx.frame_count);
        return Result::Pending;
    case kHoldReturn: {
        if (held >= kHoldReturnFrames) {
            s_phase = kEvaluate;
            mtr::screenshot::request();
        }
        return Result::Pending;
    }
    case kEvaluate: {
        if (s_observed_change) {
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "DIK injection drove menu input layer. Baseline "
                          "top=%s depth=%d -> observed top=%s depth=%d "
                          "at frame=%d.",
                          s_baseline_top, s_baseline_depth,
                          s_observed_top, s_observed_depth,
                          s_observed_frame);
            return Result::Pass;
        }
        std::snprintf(ctx.detail, sizeof(ctx.detail),
                      "DIK injection had NO effect on menu. Baseline top=%s"
                      " depth=%d unchanged after DIK_DOWN + DIK_RETURN. "
                      "Pivot harness to widget-callback dispatcher hook.",
                      s_baseline_top, s_baseline_depth);
        return Result::Fail;
    }
    default:
        return Result::Pending;
    }
}

// === coop-spawn-probe-from-save-1 ========================================
//
// Full v0.2-plan scenario: boots, navigates Main Menu → Load A Saved Game
// → slot picker → confirm slot 1 → CONTINUE GAME → wait for gameplay →
// fire coop_spawn_probe::try_spawn_p2() → log result → hard-kill exit.
//
// Phase contracts (per v0.2 plan, refined per 2026-05-10 user observations):
//   A. BootDismissTitle      — DIK_RETURN at title splash
//   B. ClickLoadGame         — DIK_DOWN + DIK_RETURN at GameSelectScreen
//   C. ConfirmSlot1          — DIK_RETURN at WilburNewLoadSave (slot 1
//                              is highlighted by default)
//   D. DriveContinueGame     — DIK_RETURN at WilburMainMenu (post-load
//                              menu) until is_in_gameplay() detects sim
//                              activity. NB: the engine does NOT push a
//                              new screen for gameplay — gameplay renders
//                              under the still-stacked WilburMainMenu.
//                              Use transform-list + entity_lookup as the
//                              gate, NOT current_top_name.
//   E. SettleInGameplay      — wait K=120 frames after gameplay detected
//                              for transform list to stabilize
//   F. FireProbe             — call coop_spawn_probe::try_spawn_p2(),
//                              emit COOP_PROBE_RESULT line
//   G. HardKillExit          — TerminateProcess(self), bypass engine
//                              autosave-on-shutdown (Pre-3 unverified)
//
// Each menu-nav phase retries DIK_RETURN every 180 frames if the expected
// next state isn't observed (the engine's input layer is timing-sensitive
// during fade transitions; a single fire sometimes lands too early).
//
// Goal: probe iteration without the user playing through every cycle.
// Shared menu-nav helper: drive Wilbur from boot to settled-in-gameplay via
// "load save slot 1 → continue game". Returns Done after is_in_gameplay()
// has held for kSettleInGameplayFrames (transform list stabilised). Used
// by tick_load_save_1_show_ingame and tick_coop_lan_soak — both need the
// same six menu-nav phases before their own scenario-specific tails.
// Per-process static state: only one driving scenario per process per
// test_harness invariant.
enum class DriveResult { Pending, Done, Fail };

DriveResult drive_to_gameplay(ScenarioContext& ctx, int* out_baseline_tcount) {
    enum Phase {
        kBoot = 0,
        kSettleAtMenu,
        kDriveLoadButton,     // retry DOWN+RETURN until slot picker appears
        kDriveSlotConfirm,    // retry RETURN until top != slot picker
        kDriveContinueGame,   // at WilburMainMenu: retry RETURN until is_in_gameplay()
        kSettleInGameplay,    // wait kSettleInGameplayFrames after detection
        kDone,                // terminal — helper returns Done forever after
    };
    enum DriveStep { kDriveIdle = 0, kDriveDownSent, kDriveReturnSent };
    static Phase s_phase                 = kBoot;
    static int   s_phase_start_frame     = 0;
    static char  s_last_logged_top[kNameLen] = {0};
    static int   s_last_shot_frame       = 0;
    static DriveStep s_drive_step        = kDriveIdle;
    static int   s_drive_step_frame      = 0;
    static int   s_drive_attempts        = 0;
    static int   s_slot_confirm_last_attempt = -1;
    static int   s_continue_first_seen       = -1;
    static int   s_continue_last_attempt     = -1;
    static int   s_gameplay_first_seen       = -1;
    static int   s_baseline_count            = -1;

    constexpr int kSettleInGameplayFrames = 120;   // ~0.5s @ 240Hz

    if (s_phase == kDone) {
        if (out_baseline_tcount) *out_baseline_tcount = s_baseline_count;
        return DriveResult::Done;
    }

    const bool ready = mtr::screen_push::ready();
    char top[kNameLen] = {0};
    bool got_top = false;
    if (ready) got_top = mtr::screen_push::current_top_name(top, sizeof(top));

    if ((ctx.frame_count % 240) == 1) {
        mtr::log::info(
            "test_harness[drive-to-gameplay] frame=%llu phase=%d ready=%d"
            " top=\"%s\" depth=%d",
            static_cast<unsigned long long>(ctx.frame_count),
            s_phase, ready ? 1 : 0,
            got_top ? top : "(none)",
            mtr::screen_push::stack_depth());
    }

    if (got_top && std::strcmp(top, s_last_logged_top) != 0) {
        mtr::log::info("drive-to-gameplay: top transition at frame=%llu \"%s\" -> \"%s\""
                       " depth=%d",
                       static_cast<unsigned long long>(ctx.frame_count),
                       s_last_logged_top, top, mtr::screen_push::stack_depth());
        std::strncpy(s_last_logged_top, top, sizeof(s_last_logged_top) - 1);
        s_last_logged_top[sizeof(s_last_logged_top) - 1] = 0;
        mtr::screenshot::request();
        s_last_shot_frame = static_cast<int>(ctx.frame_count);
    }

    int held = static_cast<int>(ctx.frame_count) - s_phase_start_frame;

    switch (s_phase) {
    case kBoot: {
        if (!ready || !got_top) {
            if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
                mtr::dinput_hook::inject_kb_keypress(0x1C, 5);  // DIK_RETURN
            }
            return DriveResult::Pending;
        }
        bool is_main_menu = (std::strcmp(top, "GameSelectScreen")     == 0 ||
                            std::strcmp(top, "WilburMainMenu")       == 0 ||
                            std::strcmp(top, "ScreenWilburMainMenu") == 0);
        if (!is_main_menu) {
            if (ctx.frame_count > 60 && (ctx.frame_count % 60) == 0) {
                mtr::dinput_hook::inject_kb_keypress(0x1C, 5);
            }
            return DriveResult::Pending;
        }
        s_phase = kSettleAtMenu;
        s_phase_start_frame = static_cast<int>(ctx.frame_count);
        mtr::log::info("drive-to-gameplay: reached main menu at frame=%d, settling 60 frames",
                       s_phase_start_frame);
        return DriveResult::Pending;
    }
    case kSettleAtMenu: {
        if (held >= 240) {
            s_phase = kDriveLoadButton;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
            s_drive_step = kDriveIdle;
            s_drive_attempts = 0;
            mtr::log::info("drive-to-gameplay: menu settled, beginning DOWN+RETURN"
                           " retry cycle at frame=%d", s_phase_start_frame);
        }
        return DriveResult::Pending;
    }
    case kDriveLoadButton: {
        if (got_top && std::strstr(top, "NewLoadSave")) {
            s_phase = kDriveSlotConfirm;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
            s_slot_confirm_last_attempt = -1;
            mtr::log::info("drive-to-gameplay: slot picker reached top=\"%s\""
                           " after %d drive attempts at frame=%d",
                           top, s_drive_attempts, s_phase_start_frame);
            return DriveResult::Pending;
        }
        const int drive_held = static_cast<int>(ctx.frame_count) - s_drive_step_frame;
        if (s_drive_step == kDriveIdle) {
            mtr::log::info("drive-to-gameplay: drive attempt #%d: DIK_DOWN at frame=%llu"
                           " (top=\"%s\")",
                           s_drive_attempts + 1,
                           static_cast<unsigned long long>(ctx.frame_count),
                           got_top ? top : "(none)");
            mtr::dinput_hook::inject_kb_keypress(0xD0, 30);
            s_drive_step = kDriveDownSent;
            s_drive_step_frame = static_cast<int>(ctx.frame_count);
        } else if (s_drive_step == kDriveDownSent && drive_held >= 60) {
            mtr::log::info("drive-to-gameplay: drive attempt #%d: DIK_RETURN at frame=%llu"
                           " (top=\"%s\")",
                           s_drive_attempts + 1,
                           static_cast<unsigned long long>(ctx.frame_count),
                           got_top ? top : "(none)");
            mtr::dinput_hook::inject_kb_keypress(0x1C, 30);
            s_drive_step = kDriveReturnSent;
            s_drive_step_frame = static_cast<int>(ctx.frame_count);
        } else if (s_drive_step == kDriveReturnSent && drive_held >= 120) {
            ++s_drive_attempts;
            s_drive_step = kDriveIdle;
            s_drive_step_frame = static_cast<int>(ctx.frame_count);
        }
        return DriveResult::Pending;
    }
    case kDriveSlotConfirm: {
        if (!got_top || !std::strstr(top, "NewLoadSave")) {
            mtr::log::info("drive-to-gameplay: slot picker dismissed (top=\"%s\");"
                           " driving to CONTINUE GAME at frame=%llu",
                           got_top ? top : "(none)",
                           static_cast<unsigned long long>(ctx.frame_count));
            s_phase = kDriveContinueGame;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
            s_continue_first_seen = -1;
            s_continue_last_attempt = -1;
            return DriveResult::Pending;
        }
        const int since = (s_slot_confirm_last_attempt < 0)
            ? -1
            : static_cast<int>(ctx.frame_count) - s_slot_confirm_last_attempt;
        const bool first_attempt = (s_slot_confirm_last_attempt < 0 && held >= 90);
        const bool retry         = (since >= 180);
        if (first_attempt || retry) {
            mtr::log::info("drive-to-gameplay: %s DIK_RETURN to confirm slot 1 at"
                           " frame=%llu (top=\"%s\")",
                           first_attempt ? "first" : "retry",
                           static_cast<unsigned long long>(ctx.frame_count),
                           top);
            mtr::dinput_hook::inject_kb_keypress(0x1C, 30);
            s_slot_confirm_last_attempt = static_cast<int>(ctx.frame_count);
        }
        return DriveResult::Pending;
    }
    case kDriveContinueGame: {
        const int depth = mtr::screen_push::stack_depth();
        if (depth >= 4) {
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "D/DriveContinueGame: depth grew to %d (top=%s);"
                          " a retry RETURN activated the wrong menu item"
                          " (likely LOAD A SAVED GAME again). Abort to"
                          " avoid load loop.",
                          depth, got_top ? top : "(none)");
            mtr::log::info("drive-to-gameplay: %s", ctx.detail);
            return DriveResult::Fail;
        }
        int tcount = -1;
        void* player = nullptr;
        const bool gameplay = is_in_gameplay(&tcount, &player);
        if (gameplay) {
            mtr::log::info("drive-to-gameplay: GAMEPLAY DETECTED at frame=%llu"
                           " (transform_count=%d player_entity=%p)",
                           static_cast<unsigned long long>(ctx.frame_count),
                           tcount, player);
            mtr::screenshot::request();
            s_gameplay_first_seen = static_cast<int>(ctx.frame_count);
            s_baseline_count = tcount;
            s_phase = kSettleInGameplay;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
            return DriveResult::Pending;
        }
        if (got_top && std::strcmp(top, "WilburMainMenu") == 0) {
            if (s_continue_first_seen < 0) {
                s_continue_first_seen = static_cast<int>(ctx.frame_count);
                mtr::log::info("drive-to-gameplay: WilburMainMenu first seen at"
                               " frame=%d (transform_count=%d player=%p),"
                               " will fire RETURN after 90-frame settle"
                               " then retry every 180 frames",
                               s_continue_first_seen, tcount, player);
            }
            const int since_seen = static_cast<int>(ctx.frame_count) - s_continue_first_seen;
            const int since_attempt = (s_continue_last_attempt < 0)
                ? -1
                : static_cast<int>(ctx.frame_count) - s_continue_last_attempt;
            const bool first_attempt = (s_continue_last_attempt < 0 && since_seen >= 90);
            const bool retry         = (since_attempt >= 180);
            if (first_attempt || retry) {
                mtr::log::info("drive-to-gameplay: %s DIK_RETURN to activate"
                               " CONTINUE GAME at frame=%llu (waiting for"
                               " is_in_gameplay; tcount=%d player=%p)",
                               first_attempt ? "first" : "retry",
                               static_cast<unsigned long long>(ctx.frame_count),
                               tcount, player);
                mtr::dinput_hook::inject_kb_keypress(0x1C, 30);
                s_continue_last_attempt = static_cast<int>(ctx.frame_count);
            }
        }
        if ((ctx.frame_count % 240) == 0) {
            mtr::log::info("drive-to-gameplay: drive_continue heartbeat: top=\"%s\""
                           " tcount=%d player=%p",
                           got_top ? top : "(none)", tcount, player);
        }
        return DriveResult::Pending;
    }
    case kSettleInGameplay: {
        int tcount = -1;
        void* player = nullptr;
        is_in_gameplay(&tcount, &player);
        if (held >= kSettleInGameplayFrames) {
            mtr::log::info("drive-to-gameplay: gameplay settled (tcount=%d"
                           " baseline=%d player=%p), DONE at frame=%llu",
                           tcount, s_baseline_count, player,
                           static_cast<unsigned long long>(ctx.frame_count));
            mtr::screenshot::request();
            s_phase = kDone;
            if (out_baseline_tcount) *out_baseline_tcount = s_baseline_count;
            return DriveResult::Done;
        }
        return DriveResult::Pending;
    }
    case kDone:
        if (out_baseline_tcount) *out_baseline_tcount = s_baseline_count;
        return DriveResult::Done;
    }
    return DriveResult::Pending;
}

// === Existing single-process scenario: drive to gameplay → fire probe → soak.
//
// Validates that a single-process orphan (forced via try_spawn_p2) holds
// stable across kOrphanSoakFrames of gameplay ticking. The companion
// scenario tick_coop_lan_soak below uses the SAME drive_to_gameplay helper
// but waits for a real network peer to materialise an orphan instead of
// forcing one synthetically.
Result tick_load_save_1_show_ingame(ScenarioContext& ctx) {
    enum Phase {
        kDriving = 0,
        kFireProbe,
        kSoakOrphan,
        kReportAndExit,
    };
    static Phase s_phase                 = kDriving;
    static int   s_phase_start_frame     = 0;
    static int   s_baseline_count        = -1;
    static int   s_soak_min_transform_count  = INT32_MAX;
    static int   s_soak_last_heartbeat_frame = 0;

    constexpr int kOrphanSoakFrames    = 3600;
    constexpr int kOrphanSoakHeartbeat = 600;

    if (s_phase == kDriving) {
        DriveResult dr = drive_to_gameplay(ctx, &s_baseline_count);
        if (dr == DriveResult::Pending) return Result::Pending;
        if (dr == DriveResult::Fail)    return Result::Fail;
        s_phase = kFireProbe;
        s_phase_start_frame = static_cast<int>(ctx.frame_count);
        return Result::Pending;
    }

    int held = static_cast<int>(ctx.frame_count) - s_phase_start_frame;
    switch (s_phase) {
    case kDriving:
        return Result::Pending;  // unreachable, satisfies enum exhaustiveness
    case kFireProbe: {
        mtr::log::info("load-save-1: firing coop_spawn_probe::try_spawn_p2()"
                       " at frame=%llu",
                       static_cast<unsigned long long>(ctx.frame_count));
        const bool ok = mtr::coop_spawn_probe::try_spawn_p2();
        mtr::log::info("load-save-1: try_spawn_p2 returned %s",
                       ok ? "true" : "false");
        mtr::screenshot::request();
        std::snprintf(ctx.detail, sizeof(ctx.detail),
                      "F/RunProbe: try_spawn_p2 returned %s",
                      ok ? "true" : "false");
        if (!ok) {
            s_phase = kReportAndExit;
        } else {
            s_phase = kSoakOrphan;
            s_soak_last_heartbeat_frame = static_cast<int>(ctx.frame_count);
            s_soak_min_transform_count = INT32_MAX;
            mtr::log::info("load-save-1: entering orphan soak for %d frames",
                           kOrphanSoakFrames);
        }
        s_phase_start_frame = static_cast<int>(ctx.frame_count);
        return Result::Pending;
    }
    case kSoakOrphan: {
        int tcount = -1;
        void* player = nullptr;
        const bool gameplay = is_in_gameplay(&tcount, &player);
        if (!gameplay) {
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "F/Soak: lost gameplay at frame=%llu held=%d"
                          " tcount=%d player=%p",
                          static_cast<unsigned long long>(ctx.frame_count),
                          held, tcount, player);
            mtr::log::info("load-save-1: %s", ctx.detail);
            return Result::Fail;
        }
        if (tcount > 0 && tcount < s_soak_min_transform_count) {
            s_soak_min_transform_count = tcount;
        }
        const int since_hb = static_cast<int>(ctx.frame_count) - s_soak_last_heartbeat_frame;
        if (since_hb >= kOrphanSoakHeartbeat) {
            mtr::log::info("load-save-1: soak HB frame=%llu held=%d/%d"
                           " tcount=%d min_tcount=%d player=%p",
                           static_cast<unsigned long long>(ctx.frame_count),
                           held, kOrphanSoakFrames, tcount,
                           s_soak_min_transform_count, player);
            s_soak_last_heartbeat_frame = static_cast<int>(ctx.frame_count);
            mtr::screenshot::request();
        }
        if (held >= kOrphanSoakFrames) {
            mtr::log::info("load-save-1: orphan soak COMPLETE after %d frames"
                           " (tcount final=%d min=%d player=%p)",
                           held, tcount, s_soak_min_transform_count, player);
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "F/RunProbe+Soak: probe ok; soaked %d frames clean"
                          " (min_tcount=%d)",
                          held, s_soak_min_transform_count);
            s_phase = kReportAndExit;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
        }
        return Result::Pending;
    }
    case kReportAndExit: {
        if (held < 60) return Result::Pending;
        mtr::log::info("load-save-1: reporting Pass; will hard-kill in"
                       " harness epilogue to bypass autosave");
        return Result::Pass;
    }
    }
    return Result::Pending;
}

// === Phase 1.6 autonomous LAN soak ========================================
//
// Drive Wilbur to gameplay, then wait for the network peer (other Wilbur in
// the same dual-launch session) to: (a) deliver at least one valid packet
// — `NetSession::peer_known()` flips true, AND (b) cause MtrPlayerManager
// to auto-spawn the P2 orphan — `has_remote()` flips true. Then soak for
// kLanSoakFrames asserting on every tick that the cm_dev poll probe
// reports fires_p2 == 0 (the engine must never poll dev_p2; the gap-close
// shim depends on this). Fail immediately on any non-zero fires_p2 OR if
// peer/orphan don't materialise within kPeerWaitFrames.
//
// MTA precedent for the live-peer soak structure: there is none — MTA's
// test harness is human-driven. This scenario is novel to mtr-asi and is
// the empirical end-to-end check that the Phase 1.6 design holds under a
// real two-Wilbur scenario (vs. the single-process synthetic-spawn check
// that load-save-1-show-ingame performs).
Result tick_coop_lan_soak(ScenarioContext& ctx) {
    enum Phase {
        kDriving = 0,
        kWaitForPeer,
        kSoak,
        kReportAndExit,
    };
    static Phase s_phase                  = kDriving;
    static int   s_phase_start_frame      = 0;
    static int   s_baseline_count         = -1;
    static int   s_soak_last_heartbeat_frame = 0;
    static int   s_soak_min_transform_count  = INT32_MAX;
    static uint64_t s_fires_p2_at_soak_start = 0;

    constexpr int kPeerWaitFrames     = 3600;   // ~15s @ 240Hz — generous
    constexpr int kLanSoakFrames      = 1800;   // ~7.5s @ 240Hz — enough to surface any per-frame regression
    constexpr int kLanSoakHeartbeat   = 300;    // ~1.25s

    auto& net  = mtr::coop::net::NetSession::instance();
    auto& mgr  = mtr::coop::MtrPlayerManager::instance();
    auto  poll = mtr::coop::controlmapper_dev::poll_probe_stats();

    if (s_phase == kDriving) {
        DriveResult dr = drive_to_gameplay(ctx, &s_baseline_count);
        if (dr == DriveResult::Pending) return Result::Pending;
        if (dr == DriveResult::Fail)    return Result::Fail;
        s_phase = kWaitForPeer;
        s_phase_start_frame = static_cast<int>(ctx.frame_count);
        mtr::log::info("coop-lan-soak: drive_to_gameplay DONE — entering"
                       " peer wait (timeout %d frames)", kPeerWaitFrames);
        return Result::Pending;
    }

    int held = static_cast<int>(ctx.frame_count) - s_phase_start_frame;

    if (s_phase == kWaitForPeer) {
        if (!net.active()) {
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "F/WaitForPeer: NetSession inactive — missing"
                          " -mtrasi-coop-host or -mtrasi-coop-connect cmdline flag");
            mtr::log::info("coop-lan-soak: %s", ctx.detail);
            return Result::Fail;
        }
        const bool peer = net.peer_known();
        const bool remote = mgr.has_remote();
        if ((ctx.frame_count % 240) == 0) {
            mtr::log::info("coop-lan-soak: peer-wait held=%d/%d peer_known=%d"
                           " has_remote=%d packets_recvd=%llu count=%zu",
                           held, kPeerWaitFrames, peer ? 1 : 0, remote ? 1 : 0,
                           static_cast<unsigned long long>(net.packets_recvd()),
                           mgr.count());
        }
        if (peer && remote) {
            s_fires_p2_at_soak_start = poll.fires_p2;
            mtr::log::info("coop-lan-soak: peer present + remote orphan present"
                           " at frame=%llu (packets_recvd=%llu fires_p2=%llu);"
                           " entering soak for %d frames",
                           static_cast<unsigned long long>(ctx.frame_count),
                           static_cast<unsigned long long>(net.packets_recvd()),
                           static_cast<unsigned long long>(poll.fires_p2),
                           kLanSoakFrames);
            mtr::screenshot::request();
            s_phase = kSoak;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
            s_soak_last_heartbeat_frame = static_cast<int>(ctx.frame_count);
            return Result::Pending;
        }
        if (held >= kPeerWaitFrames) {
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "F/WaitForPeer: timeout after %d frames"
                          " (peer_known=%d has_remote=%d packets_recvd=%llu)",
                          kPeerWaitFrames, peer ? 1 : 0, remote ? 1 : 0,
                          static_cast<unsigned long long>(net.packets_recvd()));
            mtr::log::info("coop-lan-soak: %s", ctx.detail);
            return Result::Fail;
        }
        return Result::Pending;
    }

    if (s_phase == kSoak) {
        int tcount = -1;
        void* player = nullptr;
        const bool gameplay = is_in_gameplay(&tcount, &player);
        if (!gameplay) {
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "F/Soak: lost gameplay at frame=%llu held=%d tcount=%d",
                          static_cast<unsigned long long>(ctx.frame_count),
                          held, tcount);
            mtr::log::info("coop-lan-soak: %s", ctx.detail);
            return Result::Fail;
        }
        // The hard assertion: dev_p2 must not be polled. If fires_p2 grew
        // since soak entry, the engine has begun reading dev_p2 in a real
        // two-Wilbur scenario — the zero-state shim alone is no longer
        // sufficient and we need a NOP-when-this==dev_p2 gate inside
        // hook_poll. Fail immediately so the log captures the exact frame.
        if (poll.fires_p2 > s_fires_p2_at_soak_start) {
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "F/Soak: dev_p2 was polled during LAN soak —"
                          " fires_p2 grew from %llu to %llu at held=%d frame=%llu."
                          " Zero-state shim insufficient; gate hook_poll on"
                          " this==dev_p2.",
                          static_cast<unsigned long long>(s_fires_p2_at_soak_start),
                          static_cast<unsigned long long>(poll.fires_p2),
                          held,
                          static_cast<unsigned long long>(ctx.frame_count));
            mtr::log::info("coop-lan-soak: %s", ctx.detail);
            return Result::Fail;
        }
        if (tcount > 0 && tcount < s_soak_min_transform_count) {
            s_soak_min_transform_count = tcount;
        }
        const int since_hb = static_cast<int>(ctx.frame_count) - s_soak_last_heartbeat_frame;
        if (since_hb >= kLanSoakHeartbeat) {
            mtr::log::info("coop-lan-soak: HB held=%d/%d tcount=%d min=%d"
                           " packets_sent=%llu packets_recvd=%llu"
                           " fires_p1=%llu fires_p2=%llu",
                           held, kLanSoakFrames, tcount, s_soak_min_transform_count,
                           static_cast<unsigned long long>(net.packets_sent()),
                           static_cast<unsigned long long>(net.packets_recvd()),
                           static_cast<unsigned long long>(poll.fires_p1),
                           static_cast<unsigned long long>(poll.fires_p2));
            s_soak_last_heartbeat_frame = static_cast<int>(ctx.frame_count);
            mtr::screenshot::request();
        }
        if (held >= kLanSoakFrames) {
            std::snprintf(ctx.detail, sizeof(ctx.detail),
                          "F/Soak: LAN soak clean for %d frames"
                          " (fires_p2=%llu unchanged, packets_sent=%llu recvd=%llu"
                          " min_tcount=%d)",
                          held, static_cast<unsigned long long>(poll.fires_p2),
                          static_cast<unsigned long long>(net.packets_sent()),
                          static_cast<unsigned long long>(net.packets_recvd()),
                          s_soak_min_transform_count);
            mtr::log::info("coop-lan-soak: SOAK COMPLETE — %s", ctx.detail);
            s_phase = kReportAndExit;
            s_phase_start_frame = static_cast<int>(ctx.frame_count);
        }
        return Result::Pending;
    }

    if (s_phase == kReportAndExit) {
        if (held < 60) return Result::Pending;
        return Result::Pass;
    }

    return Result::Pending;
}

struct Scenario { const char* name; ScenarioFn fn; };
constexpr Scenario g_scenarios[] = {
    { "boot-to-main-menu",         &tick_boot_to_main_menu         },
    { "menu-nav-smoke",            &tick_menu_nav_smoke            },
    { "load-save-1-show-ingame",   &tick_load_save_1_show_ingame   },
    { "coop-lan-soak",             &tick_coop_lan_soak             },
    { "widget-probe",              &tick_widget_probe              },
    { "verify-main-menu-visible",  &tick_verify_main_menu_visible  },
    { "hold-at-menu",              &tick_hold_at_menu              },
    { "overlay-phase1-verify",     &tick_overlay_phase1_verify     },
    { "npc-overlay-phase1-verify", &tick_npc_overlay_phase1_verify },
    { "prop-overlay-phase1-verify",&tick_prop_overlay_phase1_verify},
};

ScenarioFn g_active_fn = nullptr;

// === Cmdline parsing =====================================================

// Find "-flag=" in `line` (case-sensitive, prefix-anchored at word boundary)
// and copy the value (up to next whitespace or NUL) to `out`. Returns true
// if found and value non-empty.
bool find_arg_value(const char* line, const char* flag_name,
                    char* out, size_t out_size) {
    if (!line || !flag_name || !out || out_size == 0) return false;
    out[0] = 0;

    char needle[64];
    int n = std::snprintf(needle, sizeof(needle), "-%s=", flag_name);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;

    const char* p = std::strstr(line, needle);
    if (!p) return false;
    // Require word-boundary prefix (start of string or whitespace before).
    if (p != line && p[-1] != ' ' && p[-1] != '\t') return false;

    p += static_cast<size_t>(n);
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t' && i + 1 < out_size) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

// === Result reporting + shutdown =========================================

// Phase 0D (2026-05-11): when -mtrasi-coop-port=<N> is on the cmdline, the
// result-JSON path is suffixed with -<port> so two simultaneous Wilbur
// processes (host + client on different machines) don't race on a single
// file. Set by install(); 0 = "no coop port" = current single-process path.
std::atomic<int> g_coop_port{0};

bool resolve_result_path_with_port(char* out, size_t out_size, int port) {
    if (!out || out_size < MAX_PATH) return false;
    HMODULE self = mtr::self_module();
    char modpath[MAX_PATH] = {0};
    DWORD got = GetModuleFileNameA(self, modpath, sizeof(modpath));
    if (got == 0 || got >= sizeof(modpath)) return false;
    char* slash = std::strrchr(modpath, '\\');
    if (!slash) slash = std::strrchr(modpath, '/');
    if (!slash) return false;
    *(slash + 1) = 0;
    int n;
    if (port > 0) {
        n = std::snprintf(out, out_size,
                          "%smtr-asi-test-result-%d.json", modpath, port);
    } else {
        n = std::snprintf(out, out_size,
                          "%smtr-asi-test-result.json", modpath);
    }
    return n > 0 && static_cast<size_t>(n) < out_size;
}

bool resolve_result_path(char* out, size_t out_size) {
    return resolve_result_path_with_port(
        out, out_size, g_coop_port.load(std::memory_order_relaxed));
}

const char* result_str(Result r) {
    switch (r) {
        case Result::Pass:    return "pass";
        case Result::Fail:    return "fail";
        case Result::Timeout: return "timeout";
        case Result::Pending: return "pending";
    }
    return "unknown";
}

void write_result_json(const ScenarioContext& ctx, Result r) {
    char path[MAX_PATH] = {0};
    if (!resolve_result_path(path, sizeof(path))) return;

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "w") != 0 || !fp) return;

    const ULONGLONG now = GetTickCount64();
    const unsigned long long elapsed_ms =
        (ctx.start_tick_ms != 0 && now >= ctx.start_tick_ms)
            ? (now - ctx.start_tick_ms) : 0ULL;

    // Compact JSON. No external dep; small enough to hand-emit.
    // coop_port = 0 = "single-process run" (back-compat). Non-zero means
    // this result belongs to a host or client in a multi-process coop test.
    const int port = g_coop_port.load(std::memory_order_relaxed);

    // Coop-state diagnostics (Phase 1.6 LAN soak addition): emit
    // net.peer_known / mgr.has_remote / packet counters / cm_dev poll
    // probe counters so the outer harness can assert programmatically
    // (rather than parsing log lines). Always-present fields so the JSON
    // shape is stable across scenarios; single-process scenarios that
    // don't exercise networking emit zeros, which is fine.
    auto& net  = mtr::coop::net::NetSession::instance();
    auto& mgr  = mtr::coop::MtrPlayerManager::instance();
    auto  poll = mtr::coop::controlmapper_dev::poll_probe_stats();

    std::fprintf(fp,
        "{\n"
        "  \"scenario\":         \"%s\",\n"
        "  \"result\":           \"%s\",\n"
        "  \"elapsed_ms\":       %llu,\n"
        "  \"frames\":           %llu,\n"
        "  \"coop_port\":        %d,\n"
        "  \"net_active\":       %d,\n"
        "  \"peer_known\":       %d,\n"
        "  \"has_remote\":       %d,\n"
        "  \"packets_sent\":     %llu,\n"
        "  \"packets_recvd\":    %llu,\n"
        "  \"send_errors\":      %llu,\n"
        "  \"bad_packets\":      %llu,\n"
        "  \"fires_p1\":         %llu,\n"
        "  \"fires_p2\":         %llu,\n"
        "  \"fires_other\":      %llu,\n"
        "  \"detail\":           \"%s\"\n"
        "}\n",
        ctx.name, result_str(r),
        elapsed_ms,
        static_cast<unsigned long long>(ctx.frame_count),
        port,
        net.active() ? 1 : 0,
        net.peer_known() ? 1 : 0,
        mgr.has_remote() ? 1 : 0,
        static_cast<unsigned long long>(net.packets_sent()),
        static_cast<unsigned long long>(net.packets_recvd()),
        static_cast<unsigned long long>(net.send_errors()),
        static_cast<unsigned long long>(net.bad_packets()),
        static_cast<unsigned long long>(poll.fires_p1),
        static_cast<unsigned long long>(poll.fires_p2),
        static_cast<unsigned long long>(poll.fires_other),
        ctx.detail);
    std::fclose(fp);

    mtr::log::info(
        "TESTHARNESS: scenario=%s result=%s elapsed_ms=%llu frames=%llu"
        " coop_port=%d peer_known=%d has_remote=%d fires_p2=%llu"
        " detail=\"%s\"",
        ctx.name, result_str(r), elapsed_ms,
        static_cast<unsigned long long>(ctx.frame_count),
        port, net.peer_known() ? 1 : 0, mgr.has_remote() ? 1 : 0,
        static_cast<unsigned long long>(poll.fires_p2),
        ctx.detail);
}

// EnumWindows callback: find a top-level window owned by our process.
struct FindWndCtx { DWORD pid; HWND found; };
BOOL CALLBACK enum_main_window(HWND hwnd, LPARAM lparam) {
    FindWndCtx* ctx = reinterpret_cast<FindWndCtx*>(lparam);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;  // skip owned windows
    ctx->found = hwnd;
    return FALSE;  // stop enumeration
}

void request_shutdown() {
    FindWndCtx ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows(&enum_main_window, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) {
        mtr::log::info("test_harness: posting WM_CLOSE to hwnd=%p", ctx.found);
        PostMessageA(ctx.found, WM_CLOSE, 0, 0);
    } else {
        mtr::log::info("test_harness: no main window found; PostQuitMessage(0)");
        PostQuitMessage(0);
    }
}

// === Per-frame tick =======================================================

std::atomic<bool> g_terminated{false};

void tick_impl() {
    if (!g_active.load(std::memory_order_relaxed)) return;
    if (g_terminated.load(std::memory_order_relaxed)) return;
    if (!g_active_fn) return;

    g_ctx.frame_count++;

    // Watchdog: timeout if we've been pending too long.
    const ULONGLONG now = GetTickCount64();
    if (g_ctx.start_tick_ms != 0 &&
        now - g_ctx.start_tick_ms > g_ctx.timeout_ms) {
        std::snprintf(g_ctx.detail, sizeof(g_ctx.detail),
                      "Timeout after %lu ms (%llu frames)",
                      static_cast<unsigned long>(g_ctx.timeout_ms),
                      static_cast<unsigned long long>(g_ctx.frame_count));
        write_result_json(g_ctx, Result::Timeout);
        g_terminated.store(true, std::memory_order_release);
        request_shutdown();
        return;
    }

    Result r = Result::Pending;
    __try {
        r = g_active_fn(g_ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(g_ctx.detail, sizeof(g_ctx.detail),
                      "Scenario tick faulted at frame %llu",
                      static_cast<unsigned long long>(g_ctx.frame_count));
        r = Result::Fail;
    }

    if (r != Result::Pending) {
        write_result_json(g_ctx, r);
        g_terminated.store(true, std::memory_order_release);
        // Scenarios that exit while the engine is in gameplay state risk
        // an autosave on WM_CLOSE that would overwrite slot 1. Hard-kill
        // for those. Menu-resident scenarios exit cleanly via WM_CLOSE.
        const bool ends_in_gameplay =
            (std::strcmp(g_ctx.name, "load-save-1-show-ingame") == 0) ||
            (std::strcmp(g_ctx.name, "coop-lan-soak")           == 0);
        if (ends_in_gameplay) {
            hard_kill_self();  // does not return
        } else {
            request_shutdown();
        }
    }
}

}  // namespace

// === Public API ===========================================================

void install() {
    LPSTR cmdline = GetCommandLineA();
    if (!cmdline) {
        mtr::log::info("test_harness: GetCommandLineA returned null; idle");
        return;
    }

    char scenario[kNameLen] = {0};
    if (!find_arg_value(cmdline, "mtrasi-test", scenario, sizeof(scenario))) {
        // No flag → harness sleeps. This is the user's normal launch path.
        return;
    }

    char timeout_str[16] = {0};
    int  timeout_sec = kDefaultTimeoutSec;
    if (find_arg_value(cmdline, "mtrasi-test-timeout",
                       timeout_str, sizeof(timeout_str))) {
        int parsed = atoi(timeout_str);
        if (parsed > 0 && parsed <= 600) timeout_sec = parsed;
    }

    // Phase 0D: optional -mtrasi-coop-port=<N> for multi-process orchestration.
    // Picks the suffixed result-JSON path so two simultaneous Wilbur processes
    // (on different machines — the Disney mutex blocks same-machine dual-launch)
    // don't race on a single Game/mtr-asi-test-result.json. Valid range
    // 1..65535; out-of-range = ignored (single-process path stays in effect).
    char coop_port_str[16] = {0};
    if (find_arg_value(cmdline, "mtrasi-coop-port",
                       coop_port_str, sizeof(coop_port_str))) {
        int parsed = atoi(coop_port_str);
        if (parsed > 0 && parsed <= 65535) {
            g_coop_port.store(parsed, std::memory_order_release);
        }
    }

    // Resolve scenario.
    ScenarioFn fn = nullptr;
    for (const auto& s : g_scenarios) {
        if (std::strcmp(s.name, scenario) == 0) {
            fn = s.fn;
            break;
        }
    }
    if (!fn) {
        mtr::log::info("test_harness: unknown scenario \"%s\" — IDLE", scenario);
        std::strncpy(g_ctx.name, scenario, kNameLen - 1);
        std::snprintf(g_ctx.detail, sizeof(g_ctx.detail),
                      "Unknown scenario name \"%s\"", scenario);
        // Still write a result so the watchdog doesn't hang waiting for one.
        write_result_json(g_ctx, Result::Fail);
        request_shutdown();
        return;
    }

    std::strncpy(g_ctx.name, scenario, kNameLen - 1);
    g_ctx.name[kNameLen - 1] = 0;
    g_ctx.start_tick_ms = GetTickCount64();
    g_ctx.timeout_ms = static_cast<DWORD>(timeout_sec) * 1000U;
    g_ctx.frame_count = 0;
    g_ctx.detail[0] = 0;
    g_active_fn = fn;
    g_active.store(true, std::memory_order_release);

    mtr::log::info(
        "test_harness: scenario=\"%s\" armed (timeout=%ds)",
        scenario, timeout_sec);
}

bool active() { return g_active.load(std::memory_order_relaxed); }

const char* scenario_name() {
    return g_active.load(std::memory_order_relaxed) ? g_ctx.name : "";
}

}  // namespace mtr::test_harness

// Per-frame entry point. Called from menu.cpp::on_end_scene at the end of
// every render frame so scenarios run regardless of whether the in-game UI
// is visible. No-op when no scenario is armed.
namespace mtr::test_harness {
void tick() { tick_impl(); }

// Phase 0D exposure: crash_handler needs the same port-suffixed path so its
// crash sentinel doesn't clobber the right per-process result JSON.
int  coop_port() { return g_coop_port.load(std::memory_order_relaxed); }
bool result_path(char* out, size_t out_size) {
    return resolve_result_path(out, out_size);
}
}
