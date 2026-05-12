# Autonomous testing

The mod can drive itself: launch the game, navigate to a target state, exercise a feature, capture artifacts, validate the result mathematically, and shut down cleanly. Useful for overnight runs, regression checks before commits, and validating projection/clip math without eyes on the screen.

Shipped 2026-05-09. Architecture summary in [ARCHITECTURE.md § Autonomous validation pipeline](ARCHITECTURE.md#autonomous-validation-pipeline-new-2026-05-09).

---

## Quick start

```powershell
# Single scenario, build + deploy + run + validate
pwsh tools/run-test.ps1 -Scenario boot-to-main-menu -Redeploy

# Skip rebuild (if you only changed scenario params or are debugging the harness)
pwsh tools/run-test.ps1 -Scenario boot-to-main-menu

# Multi-scenario overnight run
pwsh tools/run-overnight.ps1 -Scenarios "boot-to-main-menu,overlay-phase1-verify,npc-overlay-phase1-verify"
```

Result archive lives at `tools/test-runs/<UTC-stamp>-<scenario>/`. Open `harness-summary.txt` first for argv + exit code, then `mtr-asi.log` for the full session, then `screenshots/*.png` for visual confirmation.

---

## Exit codes

| Exit | Meaning |
|---|---|
| 0 | Pass |
| 1 | Fail (scenario reported `fail`, validator failed, or unrecognized result) |
| 2 | Hang (log file stopped growing for `-LogStallSec` seconds) |
| 3 | Timeout (scenario didn't terminate within `-TimeoutSec`) |
| 4 | Launch failure (process exited without writing the result JSON) |
| 5 | Build failure (CMake build step failed) |

Crashes from inside the engine surface as exit 1 with a `[CRASH]` line in the log + an `mtr-asi-crash-*.dmp` minidump in the archive.

---

## Available scenarios

| Scenario | What it does | Pass criterion |
|---|---|---|
| `boot-to-main-menu` | Launches game, dismisses title via DI keyboard injection, waits for `GameSelectScreen` / `WilburMainMenu` / `ScreenWilburMainMenu` | Reached main menu within timeout |
| `verify-main-menu-visible` | Same as above, then holds at main menu for ~8 seconds taking screenshots every ~1 second | Held for full hold window |
| `widget-probe` | Boots, arms `mtr::widget_probe` for ~0.5 sec at main menu, dumps findings | `findings_count() > 0` |
| `hold-at-menu` | Boots, then holds indefinitely (for manual interactive testing). User terminates Wilbur.exe manually. Use with `-TimeoutSec` ≥ 300. | Never resolves; only exits via watchdog |
| `overlay-phase1-verify` | Trigger-box overlay validation: enables overlay + test box, arms 60-frame export, then `tools/validate-overlay-frames.ps1` re-runs the projection math offline | `validate-overlay-frames.ps1` exits 0 (every logged edge endpoint within 0.5 px of recomputed value) |
| `npc-overlay-phase1-verify` | NPC overlay validation: enables NPC overlay, arms 60-frame export. Walker safety test (main menu has 0 transform-list entities, so full math validation requires gameplay) | Walker did not crash + scenario reached export drain |

Add a new scenario: write `Result tick_my_scenario(ScenarioContext& ctx)` in [test_harness.cpp](../src/mtr-asi/src/test_harness.cpp), register in `g_scenarios[]`, then `pwsh tools/run-test.ps1 -Scenario my-scenario`.

---

## How the harness drives the engine

The harness runs from `menu::on_end_scene` on every render frame (in the engine's render thread). State machine pattern:

1. **Title-screen dismissal**: poll `screen_push::ready()` and `current_top_name()`. While the screen stack isn't yet captured, fire `mtr::dinput_hook::inject_kb_keypress(0x1C, 5)` every ~60 frames. The DI buffer injection bypasses DI-exclusive (which `keybd_event` / `SendInput` cannot reach). See [research/findings/dinput-buffer-injection-2026-05-09.md](../research/findings/dinput-buffer-injection-2026-05-09.md).

2. **Target-state detection**: once the screen stack reports the target (e.g. `GameSelectScreen`), branch into the scenario body.

3. **Settle**: wait N frames after first detection (typical: 60 frames) so the splash fade completes and the engine's actual render state stabilizes.

4. **Exercise feature**: call into the feature module's public API (`set_enabled`, `set_export_frames`, etc.). For autonomous validation of projection/clip math, the feature exports per-frame matrices + outputs to log via structured `<MODULE>_FRAME_*` lines.

5. **Drain**: poll `export_frames_remaining()` until 0.

6. **Pass / fail**: write `Result::Pass` with detail string; harness writes `mtr-asi-test-result.json` and calls `request_shutdown()` which posts `WM_CLOSE` to the engine's main window for clean exit.

---

## Watchdog architecture (4-layer defense)

Each layer catches failures the next can't.

| Layer | Where | Triggers when | Action |
|---|---|---|---|
| 1 | `test_harness.cpp` `tick_impl` | Wall-clock since arm > `-mtrasi-test-timeout` | Write JSON `"result":"timeout"`, request_shutdown |
| 2 | `run-test.ps1` log-stall check (250 ms poll) | `mtr-asi.log` stopped growing for `-LogStallSec` (default 20s) | Force-terminate process, exit 2 |
| 3 | `run-test.ps1` hard-timeout | Total elapsed > `-TimeoutSec` (default 90s) | Force-terminate, exit 3 |
| 4 | `run-overnight.ps1` outer watchdog | Per-scenario wall-clock > `TimeoutSec + 30` | Kill the run-test.ps1 child, mark scenario hung, continue to next |

Crashes from within the engine: `crash_handler.cpp` SUEF writes `mtr-asi-test-result.json` with `"result":"crash"` BEFORE returning `EXCEPTION_CONTINUE_SEARCH`. run-test.ps1 sees this and exits 1 (fail) rather than 4 (launch failure). Without the result-JSON sentinel, crashes would be misreported as launch failures and the post-mortem would start in the wrong place.

---

## Validators (offline math checks)

Some scenarios export per-frame state to the log so an offline PowerShell script can re-run the math independently and assert correctness. This is RULE №1-aligned for projection/clip pipelines: tests the **logic** (matrix conventions, sign flips, Y-axis direction) not the **rendering** (driver, GPU, ImGui AA, timing jitter).

Validators currently shipped:

- `tools/validate-overlay-frames.ps1` — for `overlay-phase1-verify`. Re-runs the row-vector × matrix multiply, the 6-plane homogeneous parametric clip, and the D3D9-Y-flip NDC→screen on each logged frame. Asserts each `TRIGGER_OVERLAY_EDGE`'s `(ax,ay,bx,by)` matches the recomputed value within 0.5 px tolerance. Writes `validation-result.json` with `pass: true/false`.

To add a validator for a new scenario, look at `validate-overlay-frames.ps1` for the structure. Important PowerShell gotchas (caught the hard way during the trigger-overlay validator):

- **Case-insensitive variables**: `$C` and `$c` are the SAME variable. Loop counters silently clobber result matrices. Use names that don't collide (`$out` not `$C`, `$col` not `$c`).
- **Positional binding unrolls arrays**: `MatMul $a $b` passes 32 doubles when `$a` is a 16-element array. Always use named parameters when passing arrays: `MatMul -A $a -B $b`.
- **`return ,$arr` is unreliable across PowerShell versions**: use `Write-Output -NoEnumerate $arr; return`.
- **`New-Object 'double[]' 16` may not work**: use `[System.Array]::CreateInstance([double], 16)`.

---

## What the autonomous pipeline can NOT validate

- **Visual-only correctness in real gameplay.** Both world overlays Phase 1 are validated mathematically against the captured matrices, but verifying labels appear at the right NPCs in the right level requires a save-game load and human eyeballing. The autonomous pipeline can't test what hasn't been programmed. Phase 5 (stress-test in N levels) is human-driven.
- **Game-feel / smoothness.** "Does the camera feel as smooth as PathCam" is subjective. Telemetry (frame time variance, dt jitter histogram) is automatable; the subjective verdict isn't.
- **Side-effects on unrelated systems.** A scenario that only exercises the trigger overlay won't catch that the same change broke saving. We have one save-game crash from yesterday still pending repro under the now-installed crash handler.

---

## File reference

| File | Purpose |
|---|---|
| `tools/run-test.ps1` | Single-scenario orchestrator. Build + deploy + launch + watchdog + archive + per-scenario validator dispatch. |
| `tools/run-overnight.ps1` | Multi-scenario sequential loop with outer watchdog. Writes `overnight-<stamp>.json` summary. |
| `tools/validate-overlay-frames.ps1` | Offline math validator for `overlay-phase1-verify`. |
| `tools/bmp-to-png-thumb.ps1` | Converts archived BMP screenshots to 1024-wide PNG thumbnails using `System.Drawing`. |
| [`src/mtr-asi/src/test_harness.cpp`](../src/mtr-asi/src/test_harness.cpp) | In-mod scenario runner. State machine, JSON output, WM_CLOSE shutdown. |
| [`src/mtr-asi/src/dinput_hook.cpp`](../src/mtr-asi/src/dinput_hook.cpp) | DI buffer keypress injection (the only input path that reliably reaches the DI-exclusive engine). |
| [`src/mtr-asi/src/screen_push.cpp`](../src/mtr-asi/src/screen_push.cpp) | Screen-stack mirror; lets scenarios watch for target state. |
| [`src/mtr-asi/src/screenshot.cpp`](../src/mtr-asi/src/screenshot.cpp) | Backbuffer capture on `request()`. |
| [`src/mtr-asi/src/crash_handler.cpp`](../src/mtr-asi/src/crash_handler.cpp) | SUEF → minidump + result-JSON sentinel. |
| `tools/test-runs/<stamp>-<scenario>/` | Per-run archive. Contains everything the next-morning review needs. |
| `tools/overnight-runs/overnight-<stamp>.json` | Top-level summary for multi-scenario runs. |
