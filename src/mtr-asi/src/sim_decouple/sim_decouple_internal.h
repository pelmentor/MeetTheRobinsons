// Private header for the sim_decouple module's split TUs.
//
// sim_decouple.cpp (core) owns the config + mini-game detection + log +
// QPC + public API. The split files under sim_decouple/ implement:
//
//   sim_decouple_throttle.cpp — the 11 throttle hooks + should_skip +
//                               effective_is_throttling_impl +
//                               subsystem_throttles_active +
//                               install_throttle_hooks
//   sim_decouple_telemetry.cpp — RateChannel + render/sim Hz EMA +
//                                sim-ticked-this-render-frame flag
//
// Cross-TU state is exposed as `extern std::atomic<...>` in detail::
// namespace below. Each shared symbol is owned by exactly one TU
// (the comment on the declaration says which) and read-only or
// freely-writable from the others — single-thread engine, but the
// atomics are insurance.

#pragma once

#include <atomic>
#include <cstdint>

#include "mtr/sim_decouple.h"

namespace mtr::sim_decouple::detail {

// === QPC plumbing (defined in sim_decouple.cpp / core) ====================

uint64_t qpc_now();
void     ensure_qpc_freq();
uint64_t qpc_freq();

// === Mini-game classification (defined in sim_decouple.cpp / core) ========

PlayerMode classify_player_mode_from_stack();

// === Detailed log (defined in sim_decouple.cpp / core) ====================
// Throttle hooks may eventually log structured per-tick events here.

void detailed_log(const char* fmt, ...);

// === Throttle predicates (defined in sim_decouple_throttle.cpp) ===========

bool effective_is_throttling_impl();
bool subsystem_throttles_active();

// === Install (defined in sim_decouple_throttle.cpp) =======================
// Installs all 11 throttle hooks. Called from sim_decouple.cpp's install().

void install_throttle_hooks();

// === Telemetry hooks (defined in sim_decouple_telemetry.cpp) ==============

void on_render_frame_telemetry();         // g_render_rate.tick(); reset sim-ticked flag
void on_sim_tick_telemetry();             // g_sim_rate.tick()
void mark_sim_ticked_this_render_frame(); // set the per-frame edge flag
void reset_sim_rate_ema();                // called from set_mode() in core

// === Cross-TU shared state ================================================
// Owner annotated in each declaration. Every symbol is std::atomic so the
// extern read-write patterns are well-defined.

// Owned by sim_decouple.cpp (core): user-facing config.
extern std::atomic<int>  g_mode;                       // Mode enum as int
extern std::atomic<int>  g_target_hz;                  // 15..480
extern std::atomic<bool> g_auto_disable_in_minigame;
extern std::atomic<bool> g_minigame_detected;          // refreshed once per render frame
extern std::atomic<int>  g_player_mode;                // PlayerMode as int
extern std::atomic<bool> g_detailed_log;

// Owned by sim_decouple_throttle.cpp: cross-TU read by telemetry's
// on_render_frame to decide whether to count this frame's edge.
// (Currently telemetry doesn't read it; declared here for future use.)
extern std::atomic<bool> g_sim_ran_last_iteration;

} // namespace mtr::sim_decouple::detail
