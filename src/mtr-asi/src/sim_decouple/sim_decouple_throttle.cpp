// All 11 throttle hooks for sim_decouple.
//
// Hook map (engine VAs renamed in IDB):
//   simulation_tick_aggregator   0x67F430  main pump sim entry; throttle gate
//   pathcam_smooth_pretick       0x58C000  per-render-frame camera spring
//   tick_2d_overlay_pass         0x4A9F10  HUD tween dt-integrator
//   uv_animator (sub_4C24E0)     0x4C24E0  per-element UV/scroll integrator
//   wave_grid_tick               0x4B15C0  alt-pump 2D water/ripple Laplacian
//   chain_physics_tick_pass      0x4AE300  cape/banner/cloth solver
//   managed_object_list_tick     0x4B3BC0  generic managed-object list tick
//   timer_wheel_pretick          0x681380  alt-pump first call; per-task timers
//   post_render_entity_sweep     0x596610  main pump post-render entity tick
//   alt_pump_subsystem_sweep     0x602F10  alt-pump generic subsystem walk
//   alt_pump_pre_sim_audio_sweep 0x6582F0  alt-pump pre-sim 3-loop audio sweep
//
// Engine pump order (sub_572040): sub_57A2C0 -> simulation_tick_aggregator
// -> sub_609B90 -> render_frame_top_level. So sim ALWAYS runs before render
// in the same pump iteration, which lets the render-path throttles
// downstream (overlay tweens + UV scroll) decimate at the SAME cadence as
// the sim aggregator by checking `g_sim_ran_last_iteration`.
//
// hk_sim_aggregator additionally:
//   - Calls interp::pre_sim_restore_player + pre_sim_restore_npcs PRE.
//   - Writes dword_6FFCA4 (engine frame-dt-ms) with real-elapsed when
//     throttle is on (M1.6 fix).
//   - Calls dt_correctness::write_for_sim_run with real-elapsed seconds.
//   - On post-orig: marks telemetry, sets sim-ticked-this-frame edge,
//     calls interp::on_sim_tick_post + post_sim_capture_player +
//     post_sim_capture_npcs.

#include "sim_decouple_internal.h"
#include "mtr/dt_correctness.h"
#include "mtr/coop/remote_player_manager.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::interp {
    void on_sim_tick_post();
    void pre_sim_restore_player();
    void post_sim_capture_player();
    void pre_sim_restore_npcs();
    void post_sim_capture_npcs();
}
namespace mtr::freecam {
    void on_post_sim_aggregator();
}

namespace mtr::sim_decouple {

namespace {

// === Engine frame-dt global (M1.6 fix) ====================================
//
// Written by sub_57A2C0 at the start of every pump iteration (in ms).
// Read by sub_60EED0 (sim subscriber tick list) and tick_2d_overlay_pass
// + sub_41A870/41BE20/41CC60.
//
// Bug discovered after M5 ship: when we throttle the aggregator at 60 Hz
// while the engine pump runs at 240 Hz, sub_57A2C0 writes 4 ms every
// iteration. Sim subscribers on sim-tick iterations therefore receive
// "4 ms" when ~16.7 ms has actually passed since the last sim tick — so
// time-based effects in sub_60EED0 subscribers (background timers, fades,
// shader uniforms) tick at ~25% of intended speed at 240/60.
//
// Fix: PRE-orig-sim, overwrite dword_6FFCA4 with the real elapsed ms
// since our last actual sim run. Render-path consumers on non-sim
// iterations keep seeing sub_57A2C0's natural per-frame dt.
constexpr uintptr_t kFrameDtMsGlobalVA = 0x006FFCA4;

// === Engine VAs ===========================================================

constexpr uintptr_t kSimAggregatorVA      = 0x0067F430;
constexpr uintptr_t kPathCamPreTickVA     = 0x0058C000;
constexpr uintptr_t kTick2DOverlayPassVA  = 0x004A9F10;
constexpr uintptr_t kUVAnimatorVA         = 0x004C24E0;
constexpr uintptr_t kWaveGridTickVA       = 0x004B15C0;
constexpr uintptr_t kChainPhysicsTickVA   = 0x004AE300;
constexpr uintptr_t kManagedObjListTickVA = 0x004B3BC0;
constexpr uintptr_t kTimerWheelPretickVA  = 0x00681380;
constexpr uintptr_t kPostRenderSweepVA    = 0x00596610;
constexpr uintptr_t kAltPumpSubsysSweepVA = 0x00602F10;
constexpr uintptr_t kAltPumpAudioSweepVA  = 0x006582F0;

// MSVC __thiscall maps to __fastcall(this in ECX, edx unused, then stack args).
using PFN_SimAggregator    = int  (__fastcall*)(void* this_, int /*edx*/);
using PFN_PathCamPreTick   = int  (__fastcall*)(void* this_, int /*edx*/);
using PFN_TickOverlay      = char (__fastcall*)(void* this_, int /*edx*/);
using PFN_UVAnimator       = int  (__fastcall*)(int this_, int /*edx*/);
using PFN_WaveGridTick     = void (__fastcall*)(float* this_, int /*edx*/, float a2);
using PFN_ChainPhysicsTick = void (__cdecl*)();
using PFN_ManagedObjList   = int  (__fastcall*)(void* this_, int /*edx*/);
using PFN_TimerWheel       = int  (__cdecl*)();
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

// Per-hook gating clocks. Independent because the hooks fire from
// different call sites — a single shared clock would cross-talk.
std::atomic<uint64_t> g_last_sim_qpc{0};
std::atomic<uint64_t> g_last_pathcam_qpc{0};

// QPC at which last orig-sim-run completed; used to compute corrected
// frame-dt we write to dword_6FFCA4 + dt-correctness sim_dt.
std::atomic<uint64_t> g_last_actual_sim_qpc{0};
std::atomic<uint64_t> g_dt_corrections_applied{0};

// Skip counters (cumulative since install). Verifying the throttle
// engages — at 240 Hz render with throttle@60, we expect ~3 skips per
// ~1 invocation.
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

// Accumulator-pattern throttle: when this call should fire, advance
// `last_qpc` by exactly `target_dt`, not by full elapsed time. Eliminates
// jitter at sim_hz ≈ pump_hz: instead of "0 or 2 fires per window"
// depending on pump-time-jitter, the schedule self-corrects to fire
// exactly once per target_dt of real time.
//
// Drift cap: if `last_qpc` has fallen more than half a second behind
// `now` (pump stalled — level load, debugger break, OS scheduler hop),
// snap forward instead of firing many catch-up ticks. Avoids spiral-of-
// death.
bool should_skip(std::atomic<uint64_t>& last_qpc) {
    detail::ensure_qpc_freq();
    const uint64_t freq = detail::qpc_freq();
    if (!freq) return false;

    const int hz = detail::g_target_hz.load(std::memory_order_relaxed);
    if (hz <= 0) return false;
    const uint64_t target_dt = freq / static_cast<uint64_t>(hz);

    const uint64_t now = detail::qpc_now();
    uint64_t prev = last_qpc.load(std::memory_order_relaxed);
    if (prev == 0) {
        last_qpc.store(now, std::memory_order_relaxed);
        return false;
    }
    if (now - prev < target_dt) {
        return true;
    }

    uint64_t next = prev + target_dt;
    const uint64_t max_lag = freq / 2;   // 500 ms
    if (now > next && now - next > max_lag) {
        // Long stall — drop the accumulator and resume fresh.
        next = now - target_dt;
    }
    if (last_qpc.compare_exchange_strong(prev, next, std::memory_order_relaxed)) {
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

    if (detail::effective_is_throttling_impl()) {
        if (should_skip(g_last_sim_qpc)) {
            g_sim_skipped.fetch_add(1, std::memory_order_relaxed);
            // Tell render-path throttles "this iteration's sim is skipped"
            // so they decimate at the same cadence.
            detail::g_sim_ran_last_iteration.store(false, std::memory_order_relaxed);
            return 0;
        }
    }
    detail::g_sim_ran_last_iteration.store(true, std::memory_order_relaxed);

    // M1.6 dt correction: when throttle is engaged and we're about to run
    // a real sim tick, overwrite the engine's frame-dt global with the
    // real time elapsed since our last actual sim run. Otherwise sim's
    // dt-driven subscribers (sub_60EED0 list) and tick_2d_overlay_pass'
    // tween path see the 4ms-at-240Hz value sub_57A2C0 just wrote, when
    // ~16.7ms actually elapsed.
    //
    // dt-correctness extension: ALSO write flt_6FFCBC = real_sim_dt so
    // sim's children (entity_transform_tick, physics_state_machine_tick,
    // anim_update_all_tracks, etc.) which integrate against the 0.003
    // fixed-step global advance at the correct real-time rate.
    {
        detail::ensure_qpc_freq();
        const uint64_t freq = detail::qpc_freq();
        if (freq) {
            const uint64_t now = detail::qpc_now();
            const uint64_t prev = g_last_actual_sim_qpc.load(std::memory_order_relaxed);
            if (prev != 0) {
                const uint64_t qpc_delta = now - prev;
                const double real_dt_sec = static_cast<double>(qpc_delta) /
                                           static_cast<double>(freq);

                // dword_6FFCA4 (ms): only correct when throttle is on
                // (otherwise sub_57A2C0's natural per-pump-iteration ms
                // is what render-path consumers expect).
                if (detail::effective_is_throttling_impl()) {
                    const uint64_t dt_ms_64 = (qpc_delta * 1000ULL) / freq;
                    const uint32_t dt_ms = (dt_ms_64 > 100) ? 100u : static_cast<uint32_t>(dt_ms_64);
                    if (dt_ms > 0) {
                        *reinterpret_cast<volatile uint32_t*>(kFrameDtMsGlobalVA) = dt_ms;
                        g_dt_corrections_applied.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                // flt_6FFCBC: ALWAYS write when dt-correctness is
                // enabled, regardless of throttle. The whole point is to
                // scale every 0.003 consumer to real-time.
                mtr::dt_correctness::write_for_sim_run(real_dt_sec);
            }
            g_last_actual_sim_qpc.store(now, std::memory_order_relaxed);
        }
    }

    int rc = g_orig_sim_aggregator(this_, 0);
    on_sim_tick();
    // Mark "sim ticked this render frame" for VisualLockToSim consumers
    // (dt_correctness::write_for_render_frame / write_for_alt_pump /
    // hk_sub_4F45F0). Cleared at end of frame in on_render_frame.
    detail::mark_sim_ticked_this_render_frame();
    // Tell the interp module: a new sim state is about to be reflected
    // through camera_apply_all_active in this same render iteration.
    mtr::interp::on_sim_tick_post();
    // M4: snapshot the player entity post-sim.
    mtr::interp::post_sim_capture_player();
    // M5: walk the engine transform list and snapshot every visible NPC.
    mtr::interp::post_sim_capture_npcs();
    // Freecam MMB-teleport hold: re-write player+0x58 to the teleport
    // target so entity_transform_tick's just-completed snap-back doesn't
    // reach the render. No-op when no MMB-teleport is active.
    mtr::freecam::on_post_sim_aggregator();
    // Phase 1 coop (2026-05-12): MTA-shaped DoPulse — once per real sim
    // step, iterate registered players and call each wrapper's pulse.
    // Empty pulse bodies in skeleton; future input replay, position interp,
    // task pulse, etc. plug in here on a per-player basis. See
    // research/findings/coop-mtr-remote-player-design-2026-05-12.md.
    mtr::coop::MtrPlayerManager::instance().do_pulse();
    return rc;
}

int __fastcall hk_pathcam_pretick(void* this_, int /*edx*/) {
    // PathCam smoothing is a per-render-frame critically-damped spring.
    // Under dt-correctness, flt_6FFCBC is set to real_render_dt at the
    // top of every camera_apply_all_active, so the spring response is
    // framerate-independent regardless of how often we call it.
    // Throttling pathcam at sim_hz used to choke the spring (60Hz × 0.003
    // = 0.18 sec/sec spring time = sluggish camera). With dt-correct,
    // running every render frame is correct.
    if (detail::subsystem_throttles_active()) {
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
// rest — corrupting the per-frame batch semantics.

char __fastcall hk_tick_overlay_pass(void* this_, int /*edx*/) {
    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_overlay_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;  // skip the whole 2D-overlay tween advance for this render frame
    }
    return g_orig_tick_overlay(this_, 0);
}

int __fastcall hk_uv_animator(int this_, int /*edx*/) {
    // Called once PER overlay element by sub_4C4110's loop — many calls
    // per render frame. Skipping here keeps the parent loop running so
    // each element's draw (sub_4C3790) still happens with last-frame UVs.
    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_uv_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_uv_animator(this_, 0);
}

void __fastcall hk_wave_grid_tick(float* this_, int /*edx*/, float a2) {
    // 2D wave/ripple grid integrator. Called from engine_pump_alt
    // BEFORE sim_aggregator, so it runs every render frame when the alt
    // pump is active — at 240 Hz it'd 4× the wave speed.
    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_wave_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_orig_wave_grid_tick(this_, 0, a2);
}

void __cdecl hk_chain_physics_tick_pass() {
    // Cape/banner/cloth chain solver pass. Called from
    // engine_pump_alt_pre_sim BEFORE sim_aggregator. Each chain instance
    // ticks via chain_physics_solver(0.003, damping) where damping is
    // frame-rate-aware but the 0.003 dt is hardcoded.
    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_chain_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_orig_chain_phys_tick();
}

int __fastcall hk_managed_obj_list_tick(void* this_, int /*edx*/) {
    // Generic managed-object list tick: walks `*this` linked list,
    // calling vtable[11](dt=0.003) on each. Common pattern for managed
    // resources (timed effects, scripted timers, decals, etc.).
    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_managed_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_managed_obj_list(this_, 0);
}

int __cdecl hk_timer_wheel_pretick() {
    // Engine timer wheel — decrements per-task timers, fires callbacks
    // when expired. First call in engine_pump_alt. At 240Hz alt pump,
    // timers decrement 4× as often → scheduled callbacks fire 4× early.
    //
    // dt-correctness extension: timer_wheel_pretick is the FIRST function
    // called inside engine_pump_alt, so this is our earliest opportunity
    // to write flt_6FFCBC for alt-pump consumers (wave_grid_tick,
    // chain_physics, managed_object_list, alt-subsys sweep, alt-audio
    // sweep). Writing here lets all subsequent alt-pump consumers see a
    // per-alt-pump-iteration dt.
    mtr::dt_correctness::write_for_alt_pump();

    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
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
    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_post_render_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_post_render(this_, 0, a2);
}

int __fastcall hk_alt_subsys_sweep(int* this_, int /*edx*/) {
    // Alt-pump subsystem sweep: walks list, calls per-element advance
    // with 0.003 dt. Gated in alt pump by `if (dword_74467C)`.
    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_alt_subsys_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return g_orig_alt_subsys(this_, 0);
}

void __fastcall hk_alt_audio_sweep(float** this_, int /*edx*/, int a2) {
    // Alt-pump pre-sim sweep with 3 loops; last loop integrates with
    // 0.003 dt. Other loops compute per-frame velocity-magnitude damping
    // that's frame-rate-aware. Throttling the whole function is a slight
    // over-skip on loops 1-2 but safe.
    if (detail::subsystem_throttles_active()
        && !detail::g_sim_ran_last_iteration.load(std::memory_order_relaxed)) {
        g_alt_audio_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_orig_alt_audio(this_, 0, a2);
}

} // namespace

namespace detail {

// Render-side decimation flag: reflects whether the most recent sim
// aggregator call ran (true) or was skipped (false). Owned by this TU
// (hk_sim_aggregator writes it), exposed via internal header so the
// other render-path hooks above and any future cross-TU consumers can
// read it. Initialized true so the first render frame after install
// proceeds normally before sim has a chance to run.
std::atomic<bool> g_sim_ran_last_iteration{true};

// True when the user has the throttle on AND we're not auto-disabling
// because of mini-game detection. Hooks check this rather than the bare
// `mode()` so the mini-game auto-disable applies uniformly.
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

// dt-correctness mode supersedes the per-subsystem decimation we used
// to do on render-path / alt-pump consumers. With flt_6FFCBC re-written
// to real-dt at every consumer phase, each subsystem integrates correctly
// at its native call rate — so skipping calls would actually corrupt
// the integration (subsystem advances at call_rate * native_dt instead
// of the intended 1.0 sec / sec).
//
// EXCEPTION: TimeScale::VisualLockToSim deliberately wants visual
// subsystems to advance only on sim-tick boundaries — that's the whole
// point of the mode. Re-enable per-subsystem throttles in that case.
//
// sim_aggregator throttle still respects effective_is_throttling_impl()
// in all modes because the user explicitly chose a sim_hz; that's a
// fidelity dial, not a side-effect.
bool subsystem_throttles_active() {
    if (mtr::dt_correctness::enabled()) {
        if (mtr::dt_correctness::time_scale() ==
                mtr::dt_correctness::TimeScale::VisualLockToSim) {
            return effective_is_throttling_impl();
        }
        return false;
    }
    return effective_is_throttling_impl();
}

void install_throttle_hooks() {
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

} // namespace detail

// === Public API: skip counters + dt-correction counter =====================

uint64_t sim_skipped()           { return g_sim_skipped.load(std::memory_order_relaxed); }
uint64_t pathcam_skipped()       { return g_pathcam_skipped.load(std::memory_order_relaxed); }
uint64_t overlay_skipped()       { return g_overlay_skipped.load(std::memory_order_relaxed); }
uint64_t uv_skipped()            { return g_uv_skipped.load(std::memory_order_relaxed); }
uint64_t wave_skipped()          { return g_wave_skipped.load(std::memory_order_relaxed); }
uint64_t chain_skipped()         { return g_chain_skipped.load(std::memory_order_relaxed); }
uint64_t managed_skipped()       { return g_managed_skipped.load(std::memory_order_relaxed); }
uint64_t timer_skipped()         { return g_timer_skipped.load(std::memory_order_relaxed); }
uint64_t post_render_skipped()   { return g_post_render_skipped.load(std::memory_order_relaxed); }
uint64_t alt_subsys_skipped()    { return g_alt_subsys_skipped.load(std::memory_order_relaxed); }
uint64_t alt_audio_skipped()     { return g_alt_audio_skipped.load(std::memory_order_relaxed); }
uint64_t dt_corrections_applied() {
    return g_dt_corrections_applied.load(std::memory_order_relaxed);
}

// Public-linkage forwarder (declared in mtr/sim_decouple.h). interp.cpp
// calls this from its camera_apply_all_active POST hook to gate view-interp
// writes on the same conditions hooks in this TU use.
bool effective_is_throttling() {
    return detail::effective_is_throttling_impl();
}

} // namespace mtr::sim_decouple
