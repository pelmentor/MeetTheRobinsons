// High-FPS render decoupling — module skeleton (M0 telemetry + M1.1 state).
//
// What ships in this version:
//   - Telemetry: render Hz EMA from on_render_frame; sim Hz EMA from
//     on_sim_tick (which the M1.2 hook will start calling).
//   - Configuration state: mode + target_hz + detailed_log_enabled, all
//     thread-safe via std::atomic.
//   - Detailed log file (Game/mtr-asi-decouple.log) gated by toggle.
//
// What does NOT ship in this version:
//   - The actual sim throttle (M1.2): hook on simulation_tick_aggregator
//     @0x67F430 that returns early when target rate not yet reached.
//   - The PathCam throttle (M1.3): same pattern at 0x58C000.
//   - Snapshot infrastructure (M2): prev/curr transforms.
//   - Interpolation (M3+): lerp of view + Wilbur + NPCs.
//
// The telemetry surface is shaped for M2/M3+ already — measured_alpha()
// returns 1.0 today, will read the real interp window once M2 ships.
// This means the menu UI can be built once and stays correct as later
// milestones populate live numbers.

#include <windows.h>
#include <share.h>
#include <MinHook.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <mutex>

#include "mtr/sim_decouple.h"

namespace mtr::screen_push {
    int  stack_depth();
    bool stack_at(int idx, char* out, size_t out_size);
}

namespace mtr::interp {
    void on_sim_tick_post();
    void pre_sim_restore_player();
    void post_sim_capture_player();
    void pre_sim_restore_npcs();
    void post_sim_capture_npcs();
}

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::sim_decouple {

namespace {

// Configuration state --------------------------------------------------------

std::atomic<int> g_mode{static_cast<int>(Mode::OFF)};
std::atomic<int> g_target_hz{60};
std::atomic<bool> g_detailed_log{false};
std::atomic<bool> g_auto_disable_in_minigame{true};

// Cached mini-game detection result, refreshed once per render frame so
// per-tick hooks pay one atomic-bool load instead of walking the screen
// stack hundreds of times per frame.
std::atomic<bool> g_minigame_detected{false};

// Per-frame player-mode classification for the UI. PlayerMode comes from
// the public header; we cache it as int so the atomic stays trivial.
std::atomic<int> g_player_mode{static_cast<int>(PlayerMode::Actor)};

// Case-insensitive substring search — std::string::find isn't safe to use
// across DLL boundaries with frame-pack atomics, and we want to keep this
// header-light.
bool contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    const size_t hn = std::strlen(haystack);
    const size_t nn = std::strlen(needle);
    if (nn == 0 || nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; ++i) {
        bool match = true;
        for (size_t j = 0; j < nn; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Walks every entry in the screen stack and tags the player mode. Returns
// PlayerMode::Actor when nothing mini-game-like is found. Called once per
// render frame from on_render_frame so per-tick hooks can read the cached
// result.
PlayerMode classify_player_mode_from_stack() {
    char buf[64];
    const int depth = mtr::screen_push::stack_depth();
    bool any_minigames_hub = false;
    for (int i = 0; i < depth; ++i) {
        if (!mtr::screen_push::stack_at(i, buf, sizeof(buf))) continue;
        // ChargeBall: substring "ChargeBall" or "Chargeball" (case-insensitive).
        if (contains_ci(buf, "ChargeBall")) return PlayerMode::ChargeBall;
        // Hamster mini-game: any "MiniHamster" screen.
        if (contains_ci(buf, "MiniHamster")) return PlayerMode::Hamster;
        // DigDug: substring "DigDug".
        if (contains_ci(buf, "DigDug")) return PlayerMode::DigDug;
        // Mini-game hub screen: appears when navigating to/from any mini-game.
        // Doesn't tell us WHICH mini-game, but it's enough to veto the
        // throttle (ScreenWilburMiniGames is transient during navigation).
        if (contains_ci(buf, "WilburMiniGames")) any_minigames_hub = true;
    }
    if (any_minigames_hub) return PlayerMode::DigDug;  // generic mini-game bucket; UI shows the hub via the screen-name display
    return PlayerMode::Actor;
}

// Measurement state ----------------------------------------------------------
//
// Strategy: EMA over a 1-second window. We accumulate sample count + last
// measurement time per channel, recompute Hz once per second from the count
// and elapsed time, then blend into the EMA. This is more numerically stable
// than per-tick instantaneous-Hz blending (which spikes on long frames).

uint64_t g_qpc_freq = 0;

uint64_t qpc_now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

void ensure_qpc_freq() {
    if (g_qpc_freq) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = static_cast<uint64_t>(f.QuadPart);
}

struct RateChannel {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> window_start_qpc{0};
    std::atomic<uint64_t> ema_milliHz{0};   // store as fixed-point milli-Hz so ema is integer-atomic

    void tick() {
        ensure_qpc_freq();
        if (!g_qpc_freq) return;

        const uint64_t now = qpc_now();
        uint64_t start = window_start_qpc.load(std::memory_order_relaxed);
        if (start == 0) {
            window_start_qpc.store(now, std::memory_order_relaxed);
            count.store(1, std::memory_order_relaxed);
            return;
        }
        const uint64_t cur_count = count.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t elapsed = now - start;
        // Recompute every ~250 ms; 4 samples/sec is enough resolution for a
        // human-readable overlay and keeps the EMA responsive.
        if (elapsed >= g_qpc_freq / 4) {
            // Hz over the just-elapsed window.
            const double hz = static_cast<double>(cur_count) * static_cast<double>(g_qpc_freq)
                              / static_cast<double>(elapsed);
            uint64_t prev = ema_milliHz.load(std::memory_order_relaxed);
            uint64_t blended;
            if (prev == 0) {
                blended = static_cast<uint64_t>(hz * 1000.0);
            } else {
                // 0.4 * new + 0.6 * old — quick-warming, modest smoothing.
                blended = static_cast<uint64_t>(hz * 1000.0 * 0.4 + prev * 0.6);
            }
            ema_milliHz.store(blended, std::memory_order_relaxed);
            count.store(0, std::memory_order_relaxed);
            window_start_qpc.store(now, std::memory_order_relaxed);
        }
    }

    void reset() {
        count.store(0, std::memory_order_relaxed);
        window_start_qpc.store(0, std::memory_order_relaxed);
        ema_milliHz.store(0, std::memory_order_relaxed);
    }

    double hz() const {
        return static_cast<double>(ema_milliHz.load(std::memory_order_relaxed)) / 1000.0;
    }
};

RateChannel g_render_rate;
RateChannel g_sim_rate;

// Detailed log file ----------------------------------------------------------

FILE* g_log_file = nullptr;
std::mutex g_log_mu;

void open_log_file_locked() {
    if (g_log_file) return;
    g_log_file = _fsopen("mtr-asi-decouple.log", "w", _SH_DENYNO);
    if (g_log_file) {
        std::fputs("=== mtr-asi decouple log ===\n", g_log_file);
        std::fflush(g_log_file);
    }
}

void close_log_file_locked() {
    if (!g_log_file) return;
    std::fclose(g_log_file);
    g_log_file = nullptr;
}

void detailed_log(const char* fmt, ...) {
    if (!g_detailed_log.load(std::memory_order_relaxed)) return;
    std::scoped_lock lock(g_log_mu);
    if (!g_log_file) return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log_file, "[%02u:%02u:%02u.%03u] ",
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log_file, fmt, ap);
    va_end(ap);

    std::fputc('\n', g_log_file);
    std::fflush(g_log_file);
}

// Engine frame-dt global (in ms). Written by sub_57A2C0 at the start of
// every pump iteration. Read by sub_60EED0 (sim subscriber tick list) and
// by tick_2d_overlay_pass + sub_41A870/41BE20/41CC60.
//
// Bug discovered after M5 ship: when we throttle the aggregator at 60 Hz
// while the engine pump runs at 240 Hz, sub_57A2C0 writes a 4 ms dt every
// iteration. Sim subscribers on sim-tick iterations therefore receive
// "4 ms" when ~16.7 ms has actually passed since the last sim tick — so
// time-based effects in sub_60EED0 subscribers (background timers, fades,
// shader uniforms) tick at ~25% of intended speed at 240/60.
//
// Fix: PRE-orig-sim, overwrite dword_6FFCA4 with the real elapsed ms
// since our last actual sim run. tick_2d_overlay_pass + sim subscribers
// then see the correct dt. Render-path consumers on non-sim iterations
// keep seeing sub_57A2C0's natural per-frame dt (we only overwrite on
// sim-run iterations).
constexpr uintptr_t kFrameDtMsGlobalVA = 0x006FFCA4;

// Engine VAs (renamed in IDB) ----------------------------------------------
//
// Engine pump order (sub_572040): sub_57A2C0 -> simulation_tick_aggregator
// -> sub_609B90 -> render_frame_top_level. So sim ALWAYS runs before render
// in the same pump iteration, which lets the render-path throttles
// downstream (overlay tweens + UV scroll) decimate at the SAME cadence as
// the sim aggregator by checking `g_sim_ran_last_iteration`.

constexpr uintptr_t kSimAggregatorVA      = 0x0067F430;  // simulation_tick_aggregator
constexpr uintptr_t kPathCamPreTickVA     = 0x0058C000;  // pathcam_smooth_pretick
constexpr uintptr_t kTick2DOverlayPassVA  = 0x004A9F10;  // tick_2d_overlay_pass — HUD tween dt-integrator
constexpr uintptr_t kUVAnimatorVA         = 0x004C24E0;  // sub_4C24E0 — per-element UV/scroll integrator (literal 0.003)
constexpr uintptr_t kWaveGridTickVA       = 0x004B15C0;  // wave_grid_tick — 2D water/ripple Laplacian; called from alternative pump engine_pump_alt (sub_682010) BEFORE sim_aggregator with literal 0.003 dt
constexpr uintptr_t kChainPhysicsTickVA   = 0x004AE300;  // chain_physics_tick_pass — walks list `unk_71D33C` calling chain_physics_solver(0.003, damping); cape/banner/cloth physics in alt pump
constexpr uintptr_t kManagedObjListTickVA = 0x004B3BC0;  // managed_object_list_tick — generic list of objects each ticked with 0.003 dt via vtable[11]; alt pump
// M1.8 — additional alt-pump and main-pump 0.003 integrators discovered
// after extending the xref scan past its initial limit (153 total xrefs).
constexpr uintptr_t kTimerWheelPretickVA  = 0x00681380;  // timer_wheel_pretick — first call in engine_pump_alt; calls timer_wheel_advance with 0.003 fixed step + real-dt
constexpr uintptr_t kPostRenderSweepVA    = 0x00596610;  // post_render_entity_sweep — called from sub_5908C0 in MAIN pump POST-render; iterates list calling per-entity step with 0.003 dt
constexpr uintptr_t kAltPumpSubsysSweepVA = 0x00602F10;  // alt_pump_subsystem_sweep — called in alt pump under `if (dword_74467C)`; walks subsystem list with 0.003 dt
constexpr uintptr_t kAltPumpAudioSweepVA  = 0x006582F0;  // alt_pump_pre_sim_audio_sweep — called in engine_pump_alt_pre_sim; 3-loop sweep, last loop integrates with 0.003

// MSVC __thiscall maps to __fastcall(this in ECX, edx unused, then stack args).
// Both throttle-target functions take only `this` so the dummy edx arg is
// the only addition. tick_2d_overlay_pass is the same shape.
using PFN_SimAggregator    = int (__fastcall*)(void* this_, int /*edx*/);
using PFN_PathCamPreTick   = int (__fastcall*)(void* this_, int /*edx*/);
using PFN_TickOverlay      = char (__fastcall*)(void* this_, int /*edx*/);
using PFN_UVAnimator       = int (__fastcall*)(int this_, int /*edx*/);
using PFN_WaveGridTick     = void (__fastcall*)(float* this_, int /*edx*/, float a2);
using PFN_ChainPhysicsTick = void (__cdecl*)();          // no args (decompile shows void())
using PFN_ManagedObjList   = int  (__fastcall*)(void* this_, int /*edx*/);
// M1.8 calling conventions (each verified from decompile):
using PFN_TimerWheel       = int  (__cdecl*)();                     // no args
using PFN_PostRenderSweep  = int  (__fastcall*)(int* this_, int /*edx*/, float a2);
using PFN_AltSubsysSweep   = int  (__fastcall*)(int* this_, int /*edx*/);
using PFN_AltAudioSweep    = void (__fastcall*)(float** this_, int /*edx*/, int a2);

PFN_SimAggregator    g_orig_sim_aggregator   = nullptr;
PFN_PathCamPreTick   g_orig_pathcam_pretick  = nullptr;
PFN_TickOverlay      g_orig_tick_overlay     = nullptr;
PFN_UVAnimator       g_orig_uv_animator      = nullptr;
PFN_WaveGridTick     g_orig_wave_grid_tick   = nullptr;
PFN_ChainPhysicsTick g_orig_chain_phys_tick  = nullptr;
PFN_ManagedObjList   g_orig_managed_obj_list = nullptr;
PFN_TimerWheel       g_orig_timer_wheel      = nullptr;
PFN_PostRenderSweep  g_orig_post_render      = nullptr;
PFN_AltSubsysSweep   g_orig_alt_subsys       = nullptr;
PFN_AltAudioSweep    g_orig_alt_audio        = nullptr;

// Per-hook gating clocks. Independent because the two hooks fire from
// different call sites (sim_aggregator from the engine pump, pathcam from
// the render path), so a single shared clock would cross-talk.
std::atomic<uint64_t> g_last_sim_qpc{0};
std::atomic<uint64_t> g_last_pathcam_qpc{0};

// Tracks the QPC at which our last orig-sim-run actually completed, used
// to compute the corrected frame-dt we write to dword_6FFCA4.
std::atomic<uint64_t> g_last_actual_sim_qpc{0};
std::atomic<uint64_t> g_dt_corrections_applied{0};

// Render-side decimation flag: reflects whether the most recent sim
// aggregator call ran (true) or was skipped (false). Set in
// hk_sim_aggregator. Read by the render-path hooks so they decimate at
// the same cadence as sim ticks. Initialized true so the first render
// frame after install proceeds normally before sim has a chance to run.
std::atomic<bool> g_sim_ran_last_iteration{true};

// Skip counts (cumulative since install). Useful for verifying the
// throttle is actually engaging — at 240 Hz render with throttle@60, we
// expect ~3 skips per ~1 invocation.
std::atomic<uint64_t> g_sim_skipped{0};
std::atomic<uint64_t> g_pathcam_skipped{0};
std::atomic<uint64_t> g_overlay_skipped{0};
std::atomic<uint64_t> g_uv_skipped{0};
std::atomic<uint64_t> g_wave_skipped{0};
std::atomic<uint64_t> g_chain_skipped{0};
std::atomic<uint64_t> g_managed_skipped{0};
std::atomic<uint64_t> g_timer_skipped{0};
std::atomic<uint64_t> g_post_render_skipped{0};
std::atomic<uint64_t> g_alt_subsys_skipped{0};
std::atomic<uint64_t> g_alt_audio_skipped{0};

// True when the user has the throttle on AND we're not auto-disabling
// because of mini-game detection. Hooks check this rather than the bare
// `mode()` so the mini-game auto-disable applies uniformly. The anon-ns
// version is the in-TU implementation; the public forwarder at the
// bottom of the file is what the header declares so other TUs can call it.
bool effective_is_throttling_impl() {
    if (g_mode.load(std::memory_order_relaxed) != static_cast<int>(Mode::THROTTLE)) {
        return false;
    }
    if (g_auto_disable_in_minigame.load(std::memory_order_relaxed)
        && g_minigame_detected.load(std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

// Helper: returns true iff the caller should skip this tick. Updates
// `last_qpc` to `now` only when NOT skipping (so the next tick is gated
// against the last-actually-run, not the last-checked).
bool should_skip(std::atomic<uint64_t>& last_qpc) {
    ensure_qpc_freq();
    if (!g_qpc_freq) return false;

    const int hz = g_target_hz.load(std::memory_order_relaxed);
    if (hz <= 0) return false;
    const uint64_t target_dt = g_qpc_freq / static_cast<uint64_t>(hz);

    const uint64_t now = qpc_now();
    uint64_t prev = last_qpc.load(std::memory_order_relaxed);
    if (prev == 0) {
        // First call ever — allow it through and seed.
        last_qpc.store(now, std::memory_order_relaxed);
        return false;
    }
    if (now - prev < target_dt) {
        return true;
    }
    // CAS so two threads can't both decide "we're the next tick" — one
    // wins, the other skips (matters if this hook ever fires concurrently;
    // in practice the engine sim is single-threaded, but cheap insurance).
    if (last_qpc.compare_exchange_strong(prev, now, std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

int __fastcall hk_sim_aggregator(void* this_, int /*edx*/) {
    // M2.3 fence (M4 + M5): restore the player + every tracked NPC entity
    // to their real sim_curr state BEFORE orig sim runs, so sim's reads
    // see clean (non-interp) data. Both no-op if their respective toggle
    // didn't write last frame.
    mtr::interp::pre_sim_restore_player();
    mtr::interp::pre_sim_restore_npcs();

    if (effective_is_throttling_impl()) {
        if (should_skip(g_last_sim_qpc)) {
            g_sim_skipped.fetch_add(1, std::memory_order_relaxed);
            // Tell render-path throttles "this iteration's sim is skipped"
            // so they decimate at the same cadence.
            g_sim_ran_last_iteration.store(false, std::memory_order_relaxed);
            return 0;
        }
    }
    g_sim_ran_last_iteration.store(true, std::memory_order_relaxed);

    // M1.6 dt correction: when throttle is engaged and we're about to run
    // a real sim tick, overwrite the engine's frame-dt global with the
    // real time elapsed since our last actual sim run. Otherwise sim's
    // dt-driven subscribers (sub_60EED0 list) and tick_2d_overlay_pass'
    // tween path see the 4ms-at-240Hz value sub_57A2C0 just wrote, when
    // ~16.7ms actually elapsed.
    if (effective_is_throttling_impl()) {
        ensure_qpc_freq();
        if (g_qpc_freq) {
            const uint64_t now = qpc_now();
            const uint64_t prev = g_last_actual_sim_qpc.load(std::memory_order_relaxed);
            if (prev != 0) {
                // ms = (now - prev) / (qpc_freq / 1000)
                const uint64_t dt_ms_64 = ((now - prev) * 1000ULL) / g_qpc_freq;
                // Clamp absurd values (long pause / level load): cap at
                // 100ms = 10Hz minimum apparent rate. Beyond that the
                // engine itself would already be misbehaving.
                const uint32_t dt_ms = (dt_ms_64 > 100) ? 100u : static_cast<uint32_t>(dt_ms_64);
                if (dt_ms > 0) {
                    *reinterpret_cast<volatile uint32_t*>(kFrameDtMsGlobalVA) = dt_ms;
                    g_dt_corrections_applied.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_last_actual_sim_qpc.store(now, std::memory_order_relaxed);
        }
    }

    int rc = g_orig_sim_aggregator(this_, 0);
    on_sim_tick();
    // Tell the interp module: a new sim state is about to be reflected
    // through camera_apply_all_active in this same render iteration.
    // The interp hook on camera_apply will shift prev <- curr and
    // capture the new globals.
    mtr::interp::on_sim_tick_post();
    // M4: snapshot the player entity post-sim. Done here (not in the
    // camera_apply hook) so prev/curr_player advance at sim-tick cadence,
    // matching how view snapshots are gated by dirty.
    mtr::interp::post_sim_capture_player();
    // M5: walk the engine transform list and snapshot every visible NPC.
    mtr::interp::post_sim_capture_npcs();
    return rc;
}

int __fastcall hk_pathcam_pretick(void* this_, int /*edx*/) {
    if (effective_is_throttling_impl()) {
        if (should_skip(g_last_pathcam_qpc)) {
            g_pathcam_skipped.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }
    return g_orig_pathcam_pretick(this_, 0);
}

// Render-path hooks below decimate by checking the sim-aggregator flag
// rather than running their own time-gate. Reason: both fire in tight
// patterns inside a single render frame (overlay pass walks 3 lists of
// tweens; UV animator runs in a 132-iteration loop in sub_4C4110), so a
// per-call time-gate would let only the first call through and skip the
// rest — corrupting the per-frame batch semantics. Tying the decimation
// to the sim aggregator's decision gives clean "all or none per frame"
// behaviour at the same effective Hz.

char __fastcall hk_tick_overlay_pass(void* this_, int /*edx*/) {
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_overlay_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;  // skip the whole 2D-overlay tween advance for this render frame
    }
    return g_orig_tick_overlay(this_, 0);
}

int __fastcall hk_uv_animator(int this_, int /*edx*/) {
    // Called once PER overlay element by sub_4C4110's loop — many calls
    // per render frame. Skipping at this level keeps the parent loop
    // running so each element's draw (sub_4C3790) still happens with
    // last-frame UVs. UV advances at sim Hz, draws at render Hz.
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_uv_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_uv_animator(this_, 0);
}

void __fastcall hk_wave_grid_tick(float* this_, int /*edx*/, float a2) {
    // 2D wave/ripple grid integrator. Called from engine_pump_alt
    // (sub_682010) BEFORE sim_aggregator, so it runs every render frame
    // when the alt pump is active — at 240 Hz it'd 4× the wave speed.
    // Throttle on the same flag as overlay/uv: skip when sim was
    // decimated this iteration. Note: sim_ran_last_iteration reflects
    // the PREVIOUS sim_aggregator call (since sub_4B15C0 runs before
    // this iteration's sim) — that's the right cadence (decimate when
    // the previous sim run was a skip).
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_wave_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_orig_wave_grid_tick(this_, 0, a2);
}

void __cdecl hk_chain_physics_tick_pass() {
    // Cape/banner/cloth chain solver pass. Called from
    // engine_pump_alt_pre_sim (sub_6813C0) BEFORE sim_aggregator. Each
    // chain instance ticks via chain_physics_solver(0.003, damping)
    // where damping is frame-rate-aware but the 0.003 dt is hardcoded.
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_chain_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_orig_chain_phys_tick();
}

int __fastcall hk_managed_obj_list_tick(void* this_, int /*edx*/) {
    // Generic managed-object list tick: walks `*this` linked list,
    // calling vtable[11](dt=0.003) on each. Common pattern for managed
    // resources (timed effects, scripted timers, decals, etc.). In alt
    // pump path before sim_aggregator.
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_managed_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_managed_obj_list(this_, 0);
}

// M1.8 hooks — alt-pump and main-pump 0.003 integrators outside what
// the M1.7 sweep covered.

int __cdecl hk_timer_wheel_pretick() {
    // Engine timer wheel — decrements per-task timers, fires callbacks
    // when expired. First call in engine_pump_alt. At 240Hz alt pump,
    // timers decrement 4× as often → scheduled callbacks fire 4× early.
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_timer_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_timer_wheel();
}

int __fastcall hk_post_render_sweep(int* this_, int /*edx*/, float a2) {
    // Post-render entity sweep called from MAIN pump's sub_5908C0 after
    // render_frame_top_level returns. Walks an entity list and ticks
    // each with 0.003 dt. Cleanup + integration mixed; throttling the
    // outer loop is safe (entity list state is mutable per-tick;
    // skipping just means no advance this iteration).
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_post_render_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_post_render(this_, 0, a2);
}

int __fastcall hk_alt_subsys_sweep(int* this_, int /*edx*/) {
    // Alt-pump subsystem sweep: walks list, calls per-element advance
    // with 0.003 dt. Gated in alt pump by `if (dword_74467C)`.
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_alt_subsys_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_alt_subsys(this_, 0);
}

void __fastcall hk_alt_audio_sweep(float** this_, int /*edx*/, int a2) {
    // Alt-pump pre-sim sweep with 3 loops; last loop integrates with
    // 0.003 dt. Other loops compute per-frame velocity-magnitude
    // damping that's frame-rate-aware. Throttling the whole function is
    // a slight over-skip on loops 1-2 but safe (those are damping
    // updates that would simply not advance for one frame).
    if (effective_is_throttling_impl()
        && !g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_alt_audio_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_orig_alt_audio(this_, 0, a2);
}

bool install_one(const char* tag, void* va, void* hk, void** orig_pp) {
    if (MH_CreateHook(va, hk, orig_pp) != MH_OK) {
        mtr::log::info("sim_decouple: MH_CreateHook(%s @%p) failed", tag, va);
        return false;
    }
    if (MH_EnableHook(va) != MH_OK) {
        mtr::log::info("sim_decouple: MH_EnableHook(%s) failed", tag);
        return false;
    }
    mtr::log::info("sim_decouple: hooked %s at %p", tag, va);
    return true;
}

} // namespace

void install() {
    install_one("simulation_tick_aggregator",
                reinterpret_cast<void*>(kSimAggregatorVA),
                reinterpret_cast<void*>(&hk_sim_aggregator),
                reinterpret_cast<void**>(&g_orig_sim_aggregator));
    install_one("pathcam_smooth_pretick",
                reinterpret_cast<void*>(kPathCamPreTickVA),
                reinterpret_cast<void*>(&hk_pathcam_pretick),
                reinterpret_cast<void**>(&g_orig_pathcam_pretick));
    install_one("tick_2d_overlay_pass",
                reinterpret_cast<void*>(kTick2DOverlayPassVA),
                reinterpret_cast<void*>(&hk_tick_overlay_pass),
                reinterpret_cast<void**>(&g_orig_tick_overlay));
    install_one("uv_animator (sub_4C24E0)",
                reinterpret_cast<void*>(kUVAnimatorVA),
                reinterpret_cast<void*>(&hk_uv_animator),
                reinterpret_cast<void**>(&g_orig_uv_animator));
    install_one("wave_grid_tick (sub_4B15C0)",
                reinterpret_cast<void*>(kWaveGridTickVA),
                reinterpret_cast<void*>(&hk_wave_grid_tick),
                reinterpret_cast<void**>(&g_orig_wave_grid_tick));
    install_one("chain_physics_tick_pass (sub_4AE300)",
                reinterpret_cast<void*>(kChainPhysicsTickVA),
                reinterpret_cast<void*>(&hk_chain_physics_tick_pass),
                reinterpret_cast<void**>(&g_orig_chain_phys_tick));
    install_one("managed_object_list_tick (sub_4B3BC0)",
                reinterpret_cast<void*>(kManagedObjListTickVA),
                reinterpret_cast<void*>(&hk_managed_obj_list_tick),
                reinterpret_cast<void**>(&g_orig_managed_obj_list));
    install_one("timer_wheel_pretick (sub_681380)",
                reinterpret_cast<void*>(kTimerWheelPretickVA),
                reinterpret_cast<void*>(&hk_timer_wheel_pretick),
                reinterpret_cast<void**>(&g_orig_timer_wheel));
    install_one("post_render_entity_sweep (sub_596610)",
                reinterpret_cast<void*>(kPostRenderSweepVA),
                reinterpret_cast<void*>(&hk_post_render_sweep),
                reinterpret_cast<void**>(&g_orig_post_render));
    install_one("alt_pump_subsystem_sweep (sub_602F10)",
                reinterpret_cast<void*>(kAltPumpSubsysSweepVA),
                reinterpret_cast<void*>(&hk_alt_subsys_sweep),
                reinterpret_cast<void**>(&g_orig_alt_subsys));
    install_one("alt_pump_pre_sim_audio_sweep (sub_6582F0)",
                reinterpret_cast<void*>(kAltPumpAudioSweepVA),
                reinterpret_cast<void*>(&hk_alt_audio_sweep),
                reinterpret_cast<void**>(&g_orig_alt_audio));
}

uint64_t sim_skipped()     { return g_sim_skipped.load(std::memory_order_relaxed); }
uint64_t pathcam_skipped() { return g_pathcam_skipped.load(std::memory_order_relaxed); }
uint64_t overlay_skipped() { return g_overlay_skipped.load(std::memory_order_relaxed); }
uint64_t uv_skipped()      { return g_uv_skipped.load(std::memory_order_relaxed); }
uint64_t wave_skipped()    { return g_wave_skipped.load(std::memory_order_relaxed); }
uint64_t chain_skipped()   { return g_chain_skipped.load(std::memory_order_relaxed); }
uint64_t managed_skipped() { return g_managed_skipped.load(std::memory_order_relaxed); }
uint64_t timer_skipped()         { return g_timer_skipped.load(std::memory_order_relaxed); }
uint64_t post_render_skipped()   { return g_post_render_skipped.load(std::memory_order_relaxed); }
uint64_t alt_subsys_skipped()    { return g_alt_subsys_skipped.load(std::memory_order_relaxed); }
uint64_t alt_audio_skipped()     { return g_alt_audio_skipped.load(std::memory_order_relaxed); }
uint64_t dt_corrections_applied() {
    return g_dt_corrections_applied.load(std::memory_order_relaxed);
}

bool auto_disable_in_minigame() {
    return g_auto_disable_in_minigame.load(std::memory_order_relaxed);
}

void set_auto_disable_in_minigame(bool on) {
    g_auto_disable_in_minigame.store(on, std::memory_order_relaxed);
}

bool minigame_detected() {
    return g_minigame_detected.load(std::memory_order_relaxed);
}

PlayerMode current_player_mode() {
    return static_cast<PlayerMode>(g_player_mode.load(std::memory_order_relaxed));
}

// Public-linkage forwarder for the anonymous-namespace impl. interp.cpp
// calls this from its camera_apply_all_active POST hook to gate view-interp
// writes on the same conditions hooks in this TU use.
bool effective_is_throttling() {
    return effective_is_throttling_impl();
}

const char* player_mode_label(PlayerMode m) {
    switch (m) {
        case PlayerMode::Actor:      return "actor";
        case PlayerMode::DigDug:     return "DigDug";
        case PlayerMode::Hamster:    return "MiniHamster";
        case PlayerMode::ChargeBall: return "ChargeBall";
    }
    return "?";
}

Mode mode() {
    return static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
}

void set_mode(Mode m) {
    const Mode prev = static_cast<Mode>(g_mode.exchange(static_cast<int>(m),
                                                        std::memory_order_relaxed));
    if (prev != m) {
        mtr::log::info("sim_decouple: mode %d -> %d (target_hz=%d)",
                       static_cast<int>(prev), static_cast<int>(m), g_target_hz.load());
        // Reset sim measurement so the EMA doesn't carry stale Hz from a
        // previous mode into the new one.
        g_sim_rate.reset();
    }
}

int target_hz() {
    return g_target_hz.load(std::memory_order_relaxed);
}

void set_target_hz(int hz) {
    if (hz < 15)  hz = 15;
    if (hz > 480) hz = 480;
    g_target_hz.store(hz, std::memory_order_relaxed);
}

bool is_throttling() {
    return mode() == Mode::THROTTLE;
}

double measured_render_hz() { return g_render_rate.hz(); }
double measured_sim_hz()    { return g_sim_rate.hz(); }
double measured_alpha()     { return 1.0; }   // M2 will populate

void on_render_frame() {
    g_render_rate.tick();
    // Refresh mini-game detection once per render frame so per-tick hooks
    // pay one atomic-bool load each, not a stack walk.
    const PlayerMode pm = classify_player_mode_from_stack();
    g_player_mode.store(static_cast<int>(pm), std::memory_order_relaxed);
    g_minigame_detected.store(pm != PlayerMode::Actor, std::memory_order_relaxed);
}
void on_sim_tick()     { g_sim_rate.tick(); }

bool detailed_log_enabled() {
    return g_detailed_log.load(std::memory_order_relaxed);
}

void set_detailed_log_enabled(bool on) {
    const bool prev = g_detailed_log.exchange(on, std::memory_order_relaxed);
    if (on && !prev) {
        std::scoped_lock lock(g_log_mu);
        open_log_file_locked();
        mtr::log::info("sim_decouple: detailed log opened (mtr-asi-decouple.log)");
    } else if (!on && prev) {
        mtr::log::info("sim_decouple: detailed log closed");
        std::scoped_lock lock(g_log_mu);
        close_log_file_locked();
    }
}

} // namespace mtr::sim_decouple
