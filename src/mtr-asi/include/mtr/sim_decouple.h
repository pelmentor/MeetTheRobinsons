// High-FPS render decoupling — public API.
//
// See research/findings/high-fps-decoupling-plan.md for the full milestone
// plan. This header is the interface for M0 (telemetry) and M1 (sim throttle).
//
// State machine:
//   Mode::OFF       — no throttling, no interp. Game runs as if this module
//                     wasn't installed. Engine ticks once per render frame.
//   Mode::THROTTLE  — sim aggregator + PathCam pre-tick run only when
//                     real-time elapsed since last tick >= 1/target_hz.
//                     Render still submits at the limiter's rate.
//
// Telemetry is always-on at zero practical cost (one QPC + atomic add per
// render frame and per sim tick). Detailed logging is gated and rotates.

#pragma once

#include <cstdint>

namespace mtr::sim_decouple {

enum class Mode {
    OFF,         // no-op (default)
    THROTTLE,    // sim throttled to target_hz
};

// Install hooks at module load time. Idempotent.
//
// M0/M1.1: registers callbacks but installs nothing on the engine yet.
// M1.2+:   actually creates MinHook detours on simulation_tick_aggregator
//          + pathcam_smooth_pretick.
void install();

// Configuration --------------------------------------------------------------

Mode mode();
void set_mode(Mode m);

// Target sim rate when mode == THROTTLE. Default 60. Clamped to [15, 480].
int  target_hz();
void set_target_hz(int hz);

// Convenience: returns true iff mode == THROTTLE.
bool is_throttling();

// True iff mode == THROTTLE AND mini-game auto-disable is not vetoing it.
// Hooks read this rather than is_throttling() so the toggle composes cleanly.
bool effective_is_throttling();

// Mini-game auto-disable -----------------------------------------------------
// Default ON. When ON: any time the screen stack contains a mini-game screen
// (DigDug / MiniHamster / ChargeBall), throttle is force-OFF for that frame.

bool auto_disable_in_minigame();
void set_auto_disable_in_minigame(bool on);

// True when the most recent on_render_frame() classified the screen as a
// mini-game. Cached so per-tick hooks don't walk the screen stack.
bool minigame_detected();

// Player-mode classification (best-effort, screen-stack based — does not read
// the entity's vtable). Order matches PlayerMode enum in sim_decouple.cpp.
enum class PlayerMode { Actor = 0, DigDug = 1, Hamster = 2, ChargeBall = 3 };
PlayerMode current_player_mode();
const char* player_mode_label(PlayerMode m);

// Live measurements ----------------------------------------------------------
// All EMAs over ~1 second of activity. Read by the telemetry overlay.

// Render rate (EndScene callbacks per second). Wired via on_render_frame.
double measured_render_hz();

// Sim rate (simulation_tick_aggregator invocations per second). Reads 0.0
// until M1.2 wires the sim hook.
double measured_sim_hz();

// Interpolation alpha in current sim window. Reads 1.0 until M2 ships
// the snapshot infrastructure.
double measured_alpha();

// Telemetry callbacks --------------------------------------------------------
// Called from the existing EndScene path (one site, render thread) and
// from the M1.2 sim hook (sim thread).

void on_render_frame();
void on_sim_tick();

// Diagnostics ----------------------------------------------------------------
// Cumulative skip counters since install. At 240 Hz render with throttle@60,
// expect roughly 3 skips per non-skipped tick (~75% skip ratio).

uint64_t sim_skipped();
uint64_t pathcam_skipped();
uint64_t overlay_skipped();
uint64_t uv_skipped();
uint64_t wave_skipped();             // M1.7: 2D wave grid (sub_4B15C0) decimations
uint64_t chain_skipped();            // M1.7: chain/cloth physics (sub_4AE300) decimations
uint64_t managed_skipped();          // M1.7: managed-object list (sub_4B3BC0) decimations
uint64_t timer_skipped();            // M1.8: timer_wheel_pretick (sub_681380) decimations
uint64_t post_render_skipped();      // M1.8: post_render_entity_sweep (sub_596610) decimations
uint64_t alt_subsys_skipped();       // M1.8: alt_pump_subsystem_sweep (sub_602F10) decimations
uint64_t alt_audio_skipped();        // M1.8: alt_pump_pre_sim_audio_sweep (sub_6582F0) decimations
uint64_t dt_corrections_applied();   // M1.6: corrected dword_6FFCA4 writes

// Logging --------------------------------------------------------------------
// Detailed log to Game/mtr-asi-decouple.log when enabled. ~Zero overhead
// when disabled (one atomic load per call).

bool detailed_log_enabled();
void set_detailed_log_enabled(bool on);

} // namespace mtr::sim_decouple
