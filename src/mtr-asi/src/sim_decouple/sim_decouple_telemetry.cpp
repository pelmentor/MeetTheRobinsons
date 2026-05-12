// Render + sim Hz telemetry for the sim_decouple module.
//
// Strategy: EMA over a 1-second window. Each `tick()` accumulates a count
// + last measurement time, recomputes Hz once per ~250 ms from count and
// elapsed time, then blends into the EMA. More numerically stable than
// per-tick instantaneous-Hz blending (which spikes on long frames).
//
// The "sim ticked this render frame" edge flag also lives here because
// it's consumed by dt-correctness's VisualLockToSim mode (a telemetry-
// adjacent concept — "did the engine actually progress sim during this
// render frame?"). Set true inside hk_sim_aggregator when sim ran;
// cleared at the end of every render frame from on_render_frame.

#include "sim_decouple_internal.h"

#include <atomic>
#include <cstdint>

namespace mtr::sim_decouple {

namespace {

struct RateChannel {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> window_start_qpc{0};
    std::atomic<uint64_t> ema_milliHz{0};   // fixed-point milli-Hz so ema is integer-atomic

    void tick() {
        detail::ensure_qpc_freq();
        const uint64_t freq = detail::qpc_freq();
        if (!freq) return;

        const uint64_t now = detail::qpc_now();
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
        if (elapsed >= freq / 4) {
            const double hz = static_cast<double>(cur_count) * static_cast<double>(freq)
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

// "Sim ticked since the start of THIS render frame" flag.
// Set true at the END of hk_sim_aggregator when sim actually ran.
// Cleared at the END of every render frame (in on_render_frame, called
// from EndScene). Read by dt_correctness::TimeScale::VisualLockToSim
// so visual-path consumers (particles, sprite UV, glow) advance only on
// frames where the engine actually ticked sim, instead of every render
// frame. This makes sim_hz behave as a "discrete update rate" dial for
// visual systems that otherwise inherit render-rate cadence.
std::atomic<bool> g_sim_ticked_this_render_frame{false};

} // namespace

namespace detail {

void on_render_frame_telemetry() {
    g_render_rate.tick();
    // Reset the sim-tick-edge flag for the NEXT render frame.
    // (Single-threaded D3D8 game: this runs in EndScene, the next pump
    // iteration runs after EndScene returns. So clearing here means
    // sim_aggregator in the next frame starts from a clean false, and
    // sets true if it actually runs that frame.)
    g_sim_ticked_this_render_frame.store(false, std::memory_order_relaxed);
}

void on_sim_tick_telemetry() {
    g_sim_rate.tick();
}

void mark_sim_ticked_this_render_frame() {
    g_sim_ticked_this_render_frame.store(true, std::memory_order_relaxed);
}

void reset_sim_rate_ema() { g_sim_rate.reset(); }

} // namespace detail

// === Public API ============================================================

void on_render_frame() {
    detail::on_render_frame_telemetry();
    // Refresh mini-game detection once per render frame so per-tick hooks
    // pay one atomic-bool load each, not a stack walk.
    const PlayerMode pm = detail::classify_player_mode_from_stack();
    detail::g_player_mode.store(static_cast<int>(pm), std::memory_order_relaxed);
    detail::g_minigame_detected.store(pm != PlayerMode::Actor, std::memory_order_relaxed);
}

void on_sim_tick() { detail::on_sim_tick_telemetry(); }

bool sim_ticked_this_render_frame() {
    return g_sim_ticked_this_render_frame.load(std::memory_order_relaxed);
}

double measured_render_hz() { return g_render_rate.hz(); }
double measured_sim_hz()    { return g_sim_rate.hz(); }
double measured_alpha()     { return 1.0; }   // M2 will populate

} // namespace mtr::sim_decouple
