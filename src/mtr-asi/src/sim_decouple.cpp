// Sim-decouple module — core (config + mini-game detection + log + QPC).
//
// The throttle hooks live in sim_decouple/sim_decouple_throttle.cpp.
// The render/sim Hz EMAs live in sim_decouple/sim_decouple_telemetry.cpp.
// This TU owns:
//   - User-facing configuration (mode + target_hz + auto_disable + log toggle).
//   - Mini-game detection (screen-stack walk + substring check).
//   - QPC plumbing.
//   - Detailed log file (Game/mtr-asi-decouple.log).
//   - install() dispatcher (wires up the throttle hooks).
//   - Most of the public API (mode/target_hz/is_throttling/etc.).
//
// Cross-TU shared state is exposed via extern in sim_decouple/sim_decouple_internal.h.

#include "sim_decouple/sim_decouple_internal.h"

#include <windows.h>
#include <share.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mtr::screen_push {
    int  stack_depth();
    bool stack_at(int idx, char* out, size_t out_size);
}

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::sim_decouple {

namespace detail {

// === Cross-TU shared state (storage) ======================================
// Declared extern in sim_decouple_internal.h.

std::atomic<int>  g_mode{static_cast<int>(Mode::OFF)};
std::atomic<int>  g_target_hz{60};
std::atomic<bool> g_auto_disable_in_minigame{true};
std::atomic<bool> g_minigame_detected{false};
std::atomic<int>  g_player_mode{static_cast<int>(PlayerMode::Actor)};
std::atomic<bool> g_detailed_log{false};

} // namespace detail

namespace {

// === QPC plumbing =========================================================

uint64_t g_qpc_freq = 0;

// === Detailed log file ====================================================

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

// === Mini-game detection ==================================================
//
// Case-insensitive substring search. std::string::find isn't safe to use
// across DLL boundaries with frame-pack atomics, and this keeps the
// header light.

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

} // namespace

namespace detail {

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

uint64_t qpc_freq() { return g_qpc_freq; }

// Walks every entry in the screen stack and tags the player mode.
// PlayerMode::Actor when nothing mini-game-like is found. Called once
// per render frame from telemetry's on_render_frame so per-tick hooks
// can read the cached result.
PlayerMode classify_player_mode_from_stack() {
    char buf[64];
    const int depth = mtr::screen_push::stack_depth();
    bool any_minigames_hub = false;
    for (int i = 0; i < depth; ++i) {
        if (!mtr::screen_push::stack_at(i, buf, sizeof(buf))) continue;
        if (contains_ci(buf, "ChargeBall"))  return PlayerMode::ChargeBall;
        if (contains_ci(buf, "MiniHamster")) return PlayerMode::Hamster;
        if (contains_ci(buf, "DigDug"))      return PlayerMode::DigDug;
        // Mini-game hub screen: appears when navigating to/from any
        // mini-game. Doesn't tell us WHICH mini-game, but it's enough
        // to veto the throttle (ScreenWilburMiniGames is transient
        // during navigation).
        if (contains_ci(buf, "WilburMiniGames")) any_minigames_hub = true;
    }
    if (any_minigames_hub) return PlayerMode::DigDug;
    return PlayerMode::Actor;
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

} // namespace detail

// === Public API ============================================================

void install() {
    detail::install_throttle_hooks();
}

bool auto_disable_in_minigame() {
    return detail::g_auto_disable_in_minigame.load(std::memory_order_relaxed);
}

void set_auto_disable_in_minigame(bool on) {
    detail::g_auto_disable_in_minigame.store(on, std::memory_order_relaxed);
}

bool minigame_detected() {
    return detail::g_minigame_detected.load(std::memory_order_relaxed);
}

PlayerMode current_player_mode() {
    return static_cast<PlayerMode>(detail::g_player_mode.load(std::memory_order_relaxed));
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
    return static_cast<Mode>(detail::g_mode.load(std::memory_order_relaxed));
}

void set_mode(Mode m) {
    const Mode prev = static_cast<Mode>(
        detail::g_mode.exchange(static_cast<int>(m), std::memory_order_relaxed));
    if (prev != m) {
        mtr::log::info("sim_decouple: mode %d -> %d (target_hz=%d)",
                       static_cast<int>(prev), static_cast<int>(m),
                       detail::g_target_hz.load());
        // Reset sim-rate EMA so a stale Hz from a previous mode doesn't
        // carry into the new one.
        detail::reset_sim_rate_ema();
    }
}

int target_hz() {
    return detail::g_target_hz.load(std::memory_order_relaxed);
}

void set_target_hz(int hz) {
    if (hz < 15)  hz = 15;
    if (hz > 480) hz = 480;
    detail::g_target_hz.store(hz, std::memory_order_relaxed);
}

bool is_throttling() {
    return mode() == Mode::THROTTLE;
}

bool detailed_log_enabled() {
    return detail::g_detailed_log.load(std::memory_order_relaxed);
}

void set_detailed_log_enabled(bool on) {
    const bool prev = detail::g_detailed_log.exchange(on, std::memory_order_relaxed);
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
