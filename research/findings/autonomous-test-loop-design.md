# Autonomous test-loop design (mtr-asi)

Status: **SHIPPED + GREEN as of 2026-05-09 PM. See OUTCOME section below for
what actually got built and the three surprises along the way.**
Goal: an AI assistant (Claude Code on Windows) can launch Wilbur, drive it to
a known state, capture diagnostics, and shut it down — without the user
manually clicking the launcher. Closes the slow `build → user launches →
user reports → diff` loop that the modding workflow currently runs on.

---

## 1. Discovered facts

### Launcher chain
- `Game/Launcher.exe` is a **Disney MFC launcher**. PE strings show:
  - Reads `HKLM\SOFTWARE\Disney Interactive Studios\Meet The Robinsons\Settings`
    for the user-saved resolution.
  - Builds the literal cmdline `-dxfullscreen -dxadapter=0 -dxresolution=<W>x<H>`.
  - Spawns Wilbur.exe via **`ShellExecuteExW`** (verb `open`).
  - Knows about flag `-launchit` — passed to Wilbur to mean "launcher already
    validated, skip your own gating".
  - Holds a global mutex `Global\Disney_s_Meet_The_Robinsons` (singleton
    enforcement; kills our chance of running two instances side-by-side, but
    not relevant to the autonomous loop).
- `Game/Wilbur.exe` is the actual game. It runs **stand-alone** with the
  same cmdline — Launcher.exe is a thin shim, NOT a DRM gate.
- ASI loader: `Game/dinput8.dll` is **Ultimate ASI Loader** by ThirteenAG
  (PDB path `D:\a\Ultimate-ASI-Loader\...\dinput8.pdb`, strings
  `IsUltimateASILoader`, `[HOOKSTART]`, `hook.ini`). Whitelist in
  `Game/hook.ini` already includes the Wilbur.exe path, so loader fires for
  Wilbur.exe regardless of who spawned it.
- DXVK is in place: `Game/d3d9.dll` + `Game/d3d8.dll` are DXVK 2.7.1; the
  dxwrapper trio is backed up to `*.dxwrapper.bak`.

### Existing mod-side capabilities (the leverage)
The mod is far from a passive observer — it already has every piece an
autonomous harness needs **except a CLI entry point and a clean exit**.
- `cmdline_hook.cpp` — already rewrites `GetCommandLineA/W`. Reading custom
  flags is a one-line addition. Already handles `-letitsnow` injection
  toggle stored in `mtr-asi-ui.ini[Boot]`.
- `console.cpp` — F2 in-game console with full dispatch into the engine's
  REPL (`sub_588DB0`, captured `g_console_state` at `0x7415E0`). Already
  forwards every `console_printf` line into `mtr-asi.log` via `[console]`
  prefix. Means: **any in-game cvar / event command can be issued
  programmatically by writing into `g_input_buf` and calling
  `dispatch_line_to_engine`** without simulated keystrokes.
- `screen_push.cpp` — live mirror of the engine's screen stack. `ready()`
  reports when the manager pointer has been captured (i.e. game is past
  splash). `current_top_name()` returns the active screen name — the
  cleanest "where am I in the state machine" signal we'll get.
- `log.cpp` — opens `Game/mtr-asi.log` with `_fsopen("w", _SH_DENYNO)`. The
  file is **tailable while the game runs**, so an external watcher can
  poll it for state markers without locking out the writer.
- `windowmode.cpp` — already replicates dxwrapper's borderless
  fullscreen-window mode natively. Default ON, persisted in ini. Test runs
  should keep this on so the harness window stays composed (alt-tab, focus
  steal, screenshot capture all behave normally) instead of the game owning
  the display mode.
- `freecam.cpp` (F3) and the MMB-teleport code path are already wired —
  scenarios like "fly camera to here, MMB-teleport, dump cull stats" need
  no new game-side work, just a way to invoke them.
- DLL detach handler in `dllmain.cpp` is already split into "process exit"
  vs "FreeLibrary" — we can call `PostQuitMessage(0)` from a mod-side
  scenario runner and the cleanup will route correctly.

### Windows automation tools available on this box
- PowerShell 7.5.4 with `Add-Type` (so we can compile ad-hoc C# for
  `SendInput`, `SetForegroundWindow`, `BitBlt`) and `System.Windows.Forms`.
- AutoHotkey, NirCmd: **NOT INSTALLED**. (Confirmed via `Get-Command`.)
- `tasklist`/`Get-Process`/`Stop-Process`: standard.
- No third-party harness tooling. Stay native.

---

## 2. Architecture comparison

The three obvious shapes, ordered by "how much trust we put in
SendKeys-style input simulation".

### A: Pure-PowerShell harness
PowerShell launches Wilbur, polls `Get-Process` + window title, sends
`SendKeys.SendWait("{F3}")`, screenshots the window with `BitBlt`, parses
the existing `mtr-asi.log` for state markers, calls `Stop-Process` to end.

- Pros: zero mod-side change. One-file deliverable.
- Cons:
  - **DirectInput exclusive**. Wilbur grabs DI-EXCLUSIVE for keyboard +
    mouse during gameplay; SendInput/SendKeys events go through the WndProc
    path which DI bypasses. We have direct evidence in `dinput_hook.cpp`:
    "OS cursor pin doesn't release" even after we ourselves call
    `SetCooperativeLevel(NONEXCLUSIVE)". A SendKeys-only harness will work
    for menus and break the moment we cross into gameplay, where 90% of the
    interesting mod state lives.
  - Termination via `Stop-Process` is a TerminateProcess. The existing
    DllMain handler explicitly skips cleanup on process exit because
    "Windows tears everything down" — but a hard-kill at an unlucky moment
    can still leave the swapchain or the global mutex in a half-released
    state, requiring a brief retry on next launch. Not a hard blocker, but
    not RULE №1 quality.
  - State detection by polling the log is fine but makes "game has crashed"
    indistinguishable from "game is stuck on a slow load" without timeouts.
- Verdict: a crutch. Acceptable as a fallback for the simplest menu-only
  scenarios; not the design target.

### B: Mod-side test harness (in-process scenario runner)
Add a `mtr::testharness` module. It reads a CLI flag like
`-mtrasi-test=mmb-altitude` (or a JSON path), runs a state-machine
scenario from inside the game's tick, dumps its findings to a fixed log
file, then calls `PostQuitMessage(0)` for a clean shutdown.

- Pros:
  - Bypasses ALL input-simulation problems. The harness IS in the game's
    address space; pressing F3 means flipping `freecam::g_active`, not
    sending VK codes. MMB-teleport means calling the function directly.
    No keyboard / mouse / focus dependency.
  - Deterministic state reads. screen_push.current_top_name() instead of
    OCR'ing a screenshot.
  - Clean exit. PostQuitMessage routes through the engine's normal shutdown
    path; DllMain's lpvReserved-aware cleanup runs correctly. File handles
    on `mtr-asi.asi` get released cleanly, eliminating the
    "Device or resource busy" race when the next iteration redeploys.
  - Composable. Scenarios live as small functions checked into source;
    each test PR adds another scenario, not another fragile keystroke
    sequence.
- Cons:
  - Requires writing a small mod-side state machine + plumbing. ~300-500
    LOC for v1. Roughly 1 evening of work.
  - Failure modes that prevent the ASI from loading at all (broken build,
    MinHook init failure) leave us with no in-process driver — we still
    need an outer watchdog.
- Verdict: this is the right backbone.

### C: Hybrid (PowerShell launcher + mod-side scenario runner)
PowerShell is the **outer watchdog**: spawns Wilbur with the test flag,
sets a 60-90s timeout, polls the log for the scenario's "PASS" / "FAIL"
marker, force-kills if the log goes silent for >15 s (covers boot crashes
where the ASI never loads). The scenario itself runs entirely inside the
mod (per option B).

- Pros: every advantage of B, plus
  - **Watchdog bounds the worst case.** A boot crash before the ASI loads
    can never leave the harness wedged.
  - Outer process owns binary lifecycle: it can `cp build/Release/mtr-asi.asi
    Game/mtr-asi.asi` only after confirming the previous Wilbur.exe is
    fully gone (via `Wait-Process` + a ~500 ms grace).
  - The PowerShell side can also collect external artifacts that the mod
    can't see — DXVK's `Wilbur_d3d9.log`, the screenshots dir,
    `mtr-asi-ui.ini` if it gets clobbered.
- Cons: two moving pieces (a PS1 file + a C++ module) instead of one. But
  each piece does what its environment is best at, so this is just
  honest separation of concerns.
- Verdict: **recommended.**

---

## 3. Recommended approach (option C, in detail)

### 3.1 Mod-side: `src/mtr-asi/src/test_harness.cpp`

New module, install order: AFTER all other modules in `dllmain.cpp` so
every hook is armed when scenarios fire.

Public surface:
```cpp
namespace mtr::test_harness {
    void install();                           // parses cmdline, registers per-frame tick
    bool active();                            // a scenario is currently running
    const char* scenario_name();              // for logging / status panel
}
```

Cmdline flags consumed (parsed once at startup, hooked via the existing
`mtr::cmdline` plumbing — append a "did the user pass `-mtrasi-test=`?"
check after the dxresolution rewrite):
- `-mtrasi-test=<name>` — required to activate the harness
- `-mtrasi-test-timeout=<seconds>` — hard self-kill timeout (default 60)
- `-mtrasi-test-out=<path>` — override results file (default
  `mtr-asi-test-result.json` next to the ASI)

Internal state machine:
- A render-thread per-frame tick installed via the existing d3d9 hook,
  before `EndScene`, gated on `active()`.
- Each scenario is a small free function returning an enum `{ Pending,
  Pass, Fail }` and a string explanation. The harness drives the function
  every render frame until it returns terminal.
- On terminal:
  1. Write `mtr-asi-test-result.json` (`{"scenario": "...", "result":
     "pass|fail", "elapsed_ms": ..., "detail": "...", "log_excerpt":
     [...]}`) to a known absolute path under `Game/`.
  2. Log a marker line: `TESTHARNESS: scenario=X result=Y` so the log
     watcher can also see it without parsing JSON.
  3. Schedule shutdown by posting WM_QUIT to the focus window. Use the
     same focus-window resolution windowmode already does.

Hello-world scenarios (initial set, ~150 LOC each at most):
- `boot-to-main-menu` — wait for screen_push.ready() && current top is
  `ScreenWilburMainMenu`. Pass when both true. Smoke test for the loader,
  the ASI, the engine boot, and screen tracking.
- `freecam-toggle` — wait for main menu, set freecam active, wait 30
  frames, set freecam inactive. Pass if no crash + freecam telemetry
  counters incremented.
- `mmb-teleport-altitude` — depends on a save being loaded; future work.

### 3.2 PowerShell-side: `tools/run-test.ps1`

```
param(
    [string]$Scenario   = "boot-to-main-menu",
    [int]   $TimeoutSec = 90,
    [switch]$Redeploy
)
```

Behaviour:
1. If `-Redeploy`: build (`cmake --build src/mtr-asi/build --config Release`),
   verify previous Wilbur.exe is gone (`Get-Process Wilbur` empty), copy
   ASI into Game.
2. `Remove-Item Game\mtr-asi.log Game\mtr-asi-test-result.json -Force -EA SilentlyContinue`
   for run isolation.
3. **Launch direct, NOT via Launcher.exe.** Mirror what Launcher.exe does:
   ```
   Start-Process -FilePath Game/Wilbur.exe `
                 -WorkingDirectory Game `
                 -ArgumentList @('-launchit',
                                 '-dxfullscreen', '-dxadapter=0',
                                 "-dxresolution=$nativeWxH",
                                 "-mtrasi-test=$Scenario",
                                 "-mtrasi-test-timeout=$TimeoutSec")
   ```
   The mod's existing cmdline_hook will rewrite `-dxresolution` to native
   and inject `-letitsnow` if persisted; that all keeps working. Argv
   parsing in the mod is purely additive.
4. Watchdog loop, polling at 250 ms:
   - If `mtr-asi-test-result.json` appears: parse it, print, return its
     pass/fail as the script's exit code. Wait up to 5 s for Wilbur.exe to
     exit on its own (the mod's PostQuitMessage); after that, force-stop.
   - If `mtr-asi.log` size hasn't grown for 15 s AND the `TESTHARNESS:`
     marker hasn't been written: assume hang, force-stop, exit code 2.
   - If TimeoutSec elapses overall: force-stop, exit code 3.
5. Always: copy `mtr-asi.log`, `Wilbur_d3d9.log` and the result JSON into
   a per-run subdir under `tools/test-runs/<UTC-stamp>/` for archival.

Exit codes: 0=pass, 1=fail (scenario reported failure), 2=hang, 3=timeout,
4=launch failure.

### 3.3 First deliverable (concrete spec)

Scope-limited "hello world": **`boot-to-main-menu`**.

Files to create:
- `src/mtr-asi/include/mtr/test_harness.h` — public install/active surface.
- `src/mtr-asi/src/test_harness.cpp` — the module + ONE scenario
  (`boot-to-main-menu`). ~250 LOC.
- `tools/run-test.ps1` — the watchdog. ~120 LOC.

Files to edit:
- `src/mtr-asi/src/dllmain.cpp` — call `mtr::test_harness::install()` at the
  end of the init thread.
- `src/mtr-asi/src/cmdline_hook.cpp` — public getter `bool
  test_flag_present(const char*, char* out, size_t)` so test_harness can
  read `-mtrasi-test=` etc. without re-parsing GetCommandLineA itself.
- `src/mtr-asi/CMakeLists.txt` — add `src/test_harness.cpp` to sources.

Acceptance: `pwsh tools/run-test.ps1 -Scenario boot-to-main-menu
-Redeploy` exits 0 within 90 s, the JSON shows `"result":"pass"`, Wilbur
shut down cleanly (no Stop-Process needed), and a second invocation
immediately afterwards (no manual cleanup) also passes — proving the
file-lock-on-redeploy story works.

---

## 4. Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| `mtr-asi.asi` busy after Wilbur exit | medium | PostQuitMessage path lets DllMain detach cleanly; PS waits for `Get-Process Wilbur` to be empty + 500 ms grace before redeploying. |
| Engine crash during scenario | low-medium | PS-side log-stall watchdog (15 s no log activity) catches this; force-stop and report exit 2. |
| Scenario passes locally, fails in CI later | low (no CI yet) | All artifacts copied to `tools/test-runs/<stamp>/` for postmortem; result JSON contains a tail of `mtr-asi.log`. |
| Multiple Wilbur instances | n/a | `Global\Disney_s_Meet_The_Robinsons` mutex prevents this. Belt-and-braces: PS aborts if a Wilbur process already exists. |
| `-mtrasi-test=` flag breaks the engine's own cmdline parser | low | Engine cmdline parser is `strstr`-based for known tokens; any unknown token is ignored (verified for `-letitsnow`). New tokens are safe. |
| Force-kill mid-frame leaves DXVK shader cache corrupt | low | Default flow does NOT force-kill; only the timeout/hang paths do, and DXVK's cache is checksummed (it'll just rebuild). |
| Cmdline_hook rewrites strip our test flag | n/a | The current rewriter is dxresolution-only; it preserves all other tokens. Verified by reading `cmdline_hook.cpp`. |
| Game window steals focus / blocks BitBlt | n/a for v1 | First scenarios don't need screenshots. When they do, BitBlt of the windowed (windowmode-on) HWND works regardless of Z-order. |

---

## 5. Open questions for the user

1. **Scenarios beyond v1.** What's the next scenario after
   `boot-to-main-menu` you want — `freecam-toggle`, `mmb-altitude`,
   `cull-counter-baseline`? List 2-3 priorities so we don't over-build the
   harness frontend.
2. **Save-game dependency.** Some scenarios (MMB teleport, NPC interp
   verification) need an existing save. Do you have a "diagnostic save"
   we should use, or should the harness itself drive a Continue-from-save
   flow? Driving from save is harder; using a known save is simpler.
3. **Result reporting style.** Pass/fail JSON is enough for v1. Do you
   want screenshots automatically attached for failures (BitBlt of the
   windowmode HWND at terminal time, written next to the JSON)?
4. **CI ambition.** Is this strictly for local iterative use, or do you
   eventually want this running unattended (e.g. nightly against the main
   branch)? Affects how aggressive we get with timeouts and crash
   isolation.
5. **dinput8.dll-loaded ASI vs direct injection.** Current model relies
   on Ultimate ASI Loader being present in `Game/`. If a future test
   environment ships without it, do we want a fallback `LoadLibrary`-from-
   PowerShell injector, or is "ASI loader is part of the Game/ contract"
   acceptable?

---

## OUTCOME (shipped 2026-05-09 PM)

`pwsh tools/run-test.ps1 -Scenario boot-to-main-menu -Redeploy` exits 0 in
**3.7 seconds end-to-end**. Back-to-back second run also passes (proving
file-lock-on-redeploy works). Result JSON example:

```json
{
  "scenario":   "boot-to-main-menu",
  "result":     "pass",
  "elapsed_ms": 1391,
  "frames":     241,
  "detail":     "Reached main menu after 241 frames; top=GameSelectScreen"
}
```

### Three surprises that shaped the implementation

#### Surprise #1 — game has a "PRESS ANY KEY" title screen

The proposal assumed Wilbur.exe boots straight into the menu. Wrong: there's
a static title screen ("MEET THE ROBINSONS — PRESS ANY KEY") that gates the
main menu. Without dismissal the game sits there forever, no screen pushes
fire, scenario times out.

**Diagnostic**: the in-mod periodic screenshot capture (every ~5s) made
this trivial to identify — first run timed out, the saved BMPs showed the
title screen. Without screenshots this would've taken hours of guessing.

#### Surprise #2 — synthetic OS input does NOT reach the title screen

Tried in this order, all failed:
1. **PowerShell `[SendKeys]::SendWait("{ENTER}")`** — couldn't reach window
   because Windows focus-steal protection blocks `SetForegroundWindow`
   from non-foreground processes.
2. **PowerShell P/Invoke `keybd_event(VK_RETURN, 0x1C, ...)`** — sends
   to OS input pipeline, but the title screen's input poll doesn't see it.
3. **`mouse_event(MOUSEEVENTF_LEFTDOWN | LEFTUP)` from PowerShell** — same
   no-op as keybd_event.
4. **`PostMessageA(hwnd, WM_KEYDOWN, VK_RETURN, ...)`** — engine doesn't
   read input via WndProc messages at this stage.
5. **In-mod `keybd_event` from inside the game's address space** — also
   doesn't reach. Same OS-level synthetic-input pipeline, same
   inconsistent reach into DI-exclusive applications.

The root cause: DirectInput in exclusive-foreground mode reads input from
the kernel's raw-input subsystem, NOT from the same path that synthetic
SendInput / keybd_event feed into. Kernel-level acquisition bypasses
the synthetic injection point. (We had a hint of this in
`dinput_hook.cpp`'s "OS cursor pin doesn't release" note from the earlier
RE pass; this is the keyboard analogue.)

#### Surprise #3 — the only working path is in-mod DI buffer injection

The mod already hooks `IDirectInputDevice8::GetDeviceState`. The fix that
landed: add `mtr::dinput_hook::inject_kb_keypress(dik_scancode, poll_count)`
which sets an atomic flag. In `hk_GetDeviceState`, after calling orig and
applying suppression, OR the bit at `buffer[scancode] |= 0x80` for the
next N polls. The engine reads the buffer **we** return, so the press is
visible to it regardless of where the keystroke "should have come from".

This is the cleanest fix possible: we modify the buffer the engine itself
reads. No focus games, no synthetic-input timing, no missed acquisitions.
**Reusable for any future scenario** that needs to drive the game's input
state.

### Other learnings worth keeping

- **The actual main-menu screen name is `GameSelectScreen`**, not
  `WilburMainMenu`. The `WilburMainMenu` class registry entry refers to
  a different screen further into the menu chain (the Mainsville-name
  hub that we later see during gameplay sub-menus).
- **JSON output: never put unescaped quotes in the `detail` string.**
  Initial bug: `"detail": "top=\"GameSelectScreen\""` broke the parser.
  Fix: write `top=GameSelectScreen` without quotes. If we ever need
  embedded quotes, escape them properly during write.
- **Per-run screenshot isolation.** The harness moves any pre-existing
  `Game/screenshots/mtr_*.bmp` to `Game/screenshots/_pre-run-<stamp>/`
  before launching, so the run's archive contains only this run's
  frames. Otherwise old session screenshots get swept up (we hit this
  with 93 false-positive screenshots from previous sessions).
- **Test harness install order.** `mtr::test_harness::install()` MUST
  be the LAST thing called in the init thread, AFTER all other modules.
  Several scenarios depend on every hook being live (camera_apply,
  sim_aggregator, screen_push, dinput_hook).

### Files actually shipped

NEW:
- `src/mtr-asi/include/mtr/test_harness.h` — public API (install/tick/active)
- `src/mtr-asi/src/test_harness.cpp` — module + boot-to-main-menu scenario
  (~370 LOC)
- `tools/run-test.ps1` — outer watchdog (~150 LOC)

EDIT:
- `src/mtr-asi/src/dllmain.cpp` — install at end of init thread
- `src/mtr-asi/src/menu.cpp` — call `tick()` from `on_end_scene`
- `src/mtr-asi/src/dinput_hook.cpp` — added `inject_kb_keypress(scan, polls)`
  public API + buffer-OR logic in `hk_GetDeviceState`
- `src/mtr-asi/CMakeLists.txt` — add `test_harness.cpp` to sources

Build artefact: `Game/mtr-asi.asi` 587264 bytes.

### How to add a new scenario

1. Write a tick function `Result tick_<name>(ScenarioContext& ctx)` in
   `test_harness.cpp`. Returns `Result::Pending` until the success
   condition is reached, then `Pass` (or `Fail`/`Timeout` for failure).
2. Add an entry to `g_scenarios[]`.
3. (Optional) If the scenario needs to drive game input, use
   `mtr::dinput_hook::inject_kb_keypress(scan, polls)`. NOT SendKeys.
4. (Optional) If the scenario needs a screenshot at a specific moment,
   call `mtr::screenshot::request()` from inside the tick.
5. Build, deploy, run: `pwsh tools/run-test.ps1 -Scenario <name> -Redeploy`.
6. Result JSON written to `Game/mtr-asi-test-result.json`. Archived to
   `tools/test-runs/<stamp>-<scenario>/` with the log + screenshots.

### Next-scenario candidates (not yet implemented)

- `mmb-altitude` — verify the entity_transform_tick skip-bit fix that
  shipped 2026-05-09. Need: F3 freecam, fly up, MMB, screenshot, detect
  Wilbur visually at target (call_site or widget-name based).
- `cull-baseline` — record peripheral-cull stats at main menu + at a
  known savegame load. Regression-detect future cull-pipeline changes.
- `dxvk-smoke` — run for 30 seconds at main menu, verify no DXVK errors
  in `Wilbur_d3d9.log` and >100 fps sustained. Catches DXVK regressions.
- `widget-probe` — at main menu, dump every Sprite widget's first 0x200
  bytes via the existing layout-dump pattern. Solves the m_pcName offset
  question for the UI identity refactor.
