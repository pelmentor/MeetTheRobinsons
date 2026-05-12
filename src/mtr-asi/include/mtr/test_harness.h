// In-process test-harness for autonomous "build → launch → test → exit"
// iteration. The mod runs scenarios from inside the game's tick by reading
// the `-mtrasi-test=<name>` cmdline flag at startup; once the scenario
// reaches a terminal state (Pass/Fail/Timeout) it writes a JSON result
// next to the ASI and posts WM_QUIT for clean shutdown.
//
// External outer-watchdog: `tools/run-test.ps1` wraps Wilbur.exe launch +
// log polling + force-stop on hang. See
// research/findings/autonomous-test-loop-design.md for the full design.

#pragma once

namespace mtr::test_harness {

// Public install — called from dllmain mtr_init at the END of all other
// installs so the harness can drive scenarios that depend on every hook
// being live (camera_apply, sim_aggregator, screen_push, etc.).
void install();

// Per-frame entry point. Called from menu.cpp::on_end_scene at the end of
// every render frame. No-op (single relaxed atomic load) when no scenario
// is armed.
void tick();

// True while a scenario is selected and not yet terminal. Off-path consumers
// (status overlay, telemetry) can use this to suppress confusing user UI.
bool active();

// Name of the active scenario, or empty string when no scenario is running.
const char* scenario_name();

// Phase 0D (2026-05-11): coop port from -mtrasi-coop-port=<N> cmdline.
// 0 = no coop port set = single-process run (default). When non-zero, the
// result JSON path is suffixed `-<port>.json` so concurrent host+client
// Wilbur processes (on different machines — same-machine dual-launch is
// blocked by the Disney mutex) don't race on a single file.
int  coop_port();

// Resolve the result-JSON path used by THIS process (port-suffixed if
// coop_port() != 0). Returns false if `out_size < MAX_PATH` or if the
// mod's directory can't be resolved.
bool result_path(char* out, size_t out_size);

}  // namespace mtr::test_harness
